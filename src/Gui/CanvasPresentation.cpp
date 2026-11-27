#include "Gui/CanvasPresentation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>

namespace iiSharedCanvas {

std::expected<CanvasInputEvent, CanvasInputError>
CanvasInputTranslator::execute(const ii::gui::EditorCanvasInput &input) const noexcept
{
    const auto &native = input.native;
    const auto local = native.local_position.value_or(ii::gui::WidgetPoint{native.x, native.y});
    const auto finite = [](double value) { return std::isfinite(value); };
    if (!finite(input.document.x) || !finite(input.document.y) || std::abs(input.document.x) > 1e15
        || std::abs(input.document.y) > 1e15 || !finite(local.x) || !finite(local.y)
        || std::abs(local.x) > 1e15 || std::abs(local.y) > 1e15 || !finite(native.pressure) || native.pressure < 0
        || native.pressure > 1 || !finite(native.tilt_x) || !finite(native.tilt_y)
        || !finite(native.rotation) || std::abs(native.rotation) > 1e6 || !finite(native.timestamp)
        || !finite(native.scroll_x) || !finite(native.scroll_y))
        return std::unexpected(CanvasInputError::InvalidValue);

    CanvasInputEvent result;
    result.deviceId = native.device_id; result.nativeButtons = native.buttons;
    result.screenX = local.x; result.screenY = local.y;
    result.scrollX = native.scroll_x; result.scrollY = native.scroll_y;
    result.control = native.control; result.shift = native.shift; result.alt = native.alt;
    result.meta = native.meta; result.preciseScroll = native.precise_scroll;
    auto &pointer = result.pointer;
    pointer.documentPosition = {input.document.x, input.document.y};
    pointer.pressure = native.pressure;
    pointer.tiltX = std::clamp(native.tilt_x / 90., -1., 1.);
    pointer.tiltY = std::clamp(native.tilt_y / 90., -1., 1.);
    pointer.rotationRadians = native.rotation * std::numbers::pi / 180.;
    pointer.time = native.timestamp;
    switch (native.device) {
    case ii::gui::PointerDevice::mouse:
        pointer.device = PointerDeviceKind::Mouse; pointer.tool = PointerToolKind::Mouse; break;
    case ii::gui::PointerDevice::touch:
        pointer.device = PointerDeviceKind::Touch; pointer.tool = PointerToolKind::Finger; break;
    case ii::gui::PointerDevice::pen:
        pointer.device = PointerDeviceKind::Tablet; pointer.tool = PointerToolKind::Pen; break;
    case ii::gui::PointerDevice::eraser:
        pointer.device = PointerDeviceKind::Tablet; pointer.tool = PointerToolKind::Eraser;
        pointer.eraserActive = true; pointer.deviceState |= PointerDeviceStateEraser; break;
    default: return std::unexpected(CanvasInputError::InvalidValue);
    }
    switch (input.phase) {
    case ii::gui::EditorCanvasPhase::begin:
        pointer.phase = PointerEventPhase::Press; break;
    case ii::gui::EditorCanvasPhase::update: pointer.phase = PointerEventPhase::Move; break;
    case ii::gui::EditorCanvasPhase::end:
        pointer.phase = PointerEventPhase::Release; break;
    case ii::gui::EditorCanvasPhase::cancel: pointer.phase = PointerEventPhase::Cancel; break;
    case ii::gui::EditorCanvasPhase::hover:
        pointer.phase = PointerEventPhase::Move; pointer.hovering = true; pointer.deviceState |= PointerDeviceStateHover; break;
    case ii::gui::EditorCanvasPhase::scroll:
        result.kind = CanvasInputKind::Scroll; pointer.phase = PointerEventPhase::Move; break;
    default: return std::unexpected(CanvasInputError::InvalidValue);
    }
    // iiGUI reports the currently held button mask, not a distinct released
    // button. Preserve that uncertainty for release and cancellation.
    if (pointer.eraserActive) pointer.button = PointerButton::Eraser;
    else if (native.buttons & 1U) pointer.button = PointerButton::Primary;
    else if (native.buttons & 2U) pointer.button = PointerButton::Secondary;
    else if (native.buttons & 4U) pointer.button = PointerButton::Middle;
    pointer.primaryButtonDown = !pointer.hovering && input.phase != ii::gui::EditorCanvasPhase::end
        && input.phase != ii::gui::EditorCanvasPhase::cancel && (native.buttons & 1U) != 0;
    if (pointer.primaryButtonDown) pointer.deviceState |= PointerDeviceStatePrimaryButton;
    return result;
}

namespace {

// Bilinear interpolation uses premultiplied alpha so transparent source RGB
// cannot create coloured fringes. Clamping is confined to the supplied region.
std::array<std::uint8_t, 4> smoothPixel(const RasterLayer &source, double x, double y)
{
    const auto x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const auto fx = x - x0, fy = y - y0;
    double alpha = 0, red = 0, green = 0, blue = 0;
    for (int dy = 0; dy != 2; ++dy) for (int dx = 0; dx != 2; ++dx) {
        const auto sx = std::clamp(x0 + dx, 0, source.width - 1);
        const auto sy = std::clamp(y0 + dy, 0, source.height - 1);
        const auto color = source.pixels[std::size_t(sy) * source.width + sx];
        const auto weight = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
        const auto a = double(color >> 24) * weight;
        alpha += a; red += ((color >> 16) & 255) * a;
        green += ((color >> 8) & 255) * a; blue += (color & 255) * a;
    }
    const auto byte = [](double value) { return std::uint8_t(std::clamp(std::lround(value), 0L, 255L)); };
    if (alpha == 0) return {};
    return {byte(red / alpha), byte(green / alpha), byte(blue / alpha), byte(alpha)};
}

} // namespace

ii::core::Result<std::shared_ptr<const ii::gui::WidgetDrawing>> CanvasFramePresenter::execute(
    const FrameRenderResult &frame, CanvasRegion sampledRegion, const ii::gui::EditorCanvasQuery &query,
    RasterSampling sampling, std::uint64_t maximumPixels) const
{
    using ii::core::ErrorCode;
    const auto &transform = query.transform;
    const auto &source = frame.pixels;
    if (!frame.ok()) return ii::core::fail(ErrorCode::failed, frame.message.empty() ? "Canvas rendering failed" : frame.message);
    if (frame.origin != sampledRegion.origin || sampledRegion.extent.width <= 0 || sampledRegion.extent.height <= 0
        || source.width <= 0 || source.height <= 0
        || std::uint64_t(source.width) * std::uint64_t(source.height) != source.pixels.size()
        || !std::isfinite(query.surface.width) || !std::isfinite(query.surface.height)
        || query.surface.width < 0 || query.surface.height < 0
        || query.surface.width > ii::gui::maximum_image_dimension || query.surface.height > ii::gui::maximum_image_dimension
        || !std::isfinite(transform.scale) || transform.scale < .0001 || transform.scale > 8192
        || !std::isfinite(transform.offset_x) || std::abs(transform.offset_x) > 1e15
        || !std::isfinite(transform.offset_y) || std::abs(transform.offset_y) > 1e15
        || !std::isfinite(transform.rotation_radians) || std::abs(transform.rotation_radians) > 1e6
        || (sampling != RasterSampling::Nearest && sampling != RasterSampling::Smooth)
        || maximumPixels == 0 || maximumPixels > std::uint64_t(ii::gui::maximum_image_dimension) * ii::gui::maximum_image_dimension)
        return ii::core::fail(ErrorCode::invalid_argument, "Invalid canvas frame or viewport");
    const auto width = std::uint32_t(std::ceil(query.surface.width));
    const auto height = std::uint32_t(std::ceil(query.surface.height));
    if (std::uint64_t(width) * height > maximumPixels)
        return ii::core::fail(ErrorCode::invalid_argument, "Canvas presentation exceeds its pixel budget");
    try {
        auto drawing = std::make_shared<ii::gui::WidgetDrawing>();
        if (!width || !height) return std::shared_ptr<const ii::gui::WidgetDrawing>(std::move(drawing));
        std::vector<std::uint8_t> rgba(std::size_t(width) * height * 4, 0);
        const auto c = std::cos(transform.rotation_radians), s = std::sin(transform.rotation_radians);
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
            // Sample the center of each output pixel in the logical iiGUI surface.
            const auto px = (double(x) + .5) * query.surface.width / width - transform.offset_x;
            const auto py = (double(y) + .5) * query.surface.height / height - transform.offset_y;
            const auto wx = (c * px + s * py) / transform.scale * (transform.flip_x ? -1 : 1);
            const auto wy = (-s * px + c * py) / transform.scale * (transform.flip_y ? -1 : 1);
            const auto u = (wx - sampledRegion.origin.x) / sampledRegion.extent.width;
            const auto v = (wy - sampledRegion.origin.y) / sampledRegion.extent.height;
            if (!(u >= 0 && u < 1 && v >= 0 && v < 1)) continue;
            const auto offset = (std::size_t(y) * width + x) * 4;
            if (sampling == RasterSampling::Nearest) {
                const auto sx = std::min(int(u * source.width), source.width - 1);
                const auto sy = std::min(int(v * source.height), source.height - 1);
                const auto color = source.pixels[std::size_t(sy) * source.width + sx];
                rgba[offset] = std::uint8_t(color >> 16); rgba[offset + 1] = std::uint8_t(color >> 8);
                rgba[offset + 2] = std::uint8_t(color); rgba[offset + 3] = std::uint8_t(color >> 24);
            } else {
                const auto color = smoothPixel(source, u * source.width - .5, v * source.height - .5);
                std::copy(color.begin(), color.end(), rgba.begin() + std::ptrdiff_t(offset));
            }
        }
        auto image = ii::gui::rgba_image(width, height, std::move(rgba), ii::gui::ImageBudget{maximumPixels});
        if (!image) return std::unexpected(image.error());
        ii::gui::ImageRun run;
        run.bounds = {0, 0, float(query.surface.width), float(query.surface.height)};
        run.image = std::move(*image);
        drawing->images.push_back(std::move(run));
        return std::shared_ptr<const ii::gui::WidgetDrawing>(std::move(drawing));
    } catch (const std::bad_alloc &) {
        return ii::core::fail(ErrorCode::failed, "Cannot allocate the canvas presentation");
    } catch (const std::length_error &) {
        return ii::core::fail(ErrorCode::failed, "Canvas presentation is too large");
    }
}

} // namespace iiSharedCanvas
