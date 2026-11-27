#include "CanvasGuiBridge.h"
#include "Gui/CanvasInteraction.h"
#include "Gui/CanvasPresentation.h"

#include <algorithm>
#include <limits>

namespace {

// Borrowed C input is copied into iiGUI values. No state escapes this helper.
ii::gui::EditorCanvasInput guiInput(const IiscGuiInput &input)
{
    ii::gui::EditorCanvasInput value;
    value.phase = static_cast<ii::gui::EditorCanvasPhase>(input.phase);
    value.document = {input.x, input.y};
    value.native.device = static_cast<ii::gui::PointerDevice>(input.device);
    value.native.pressure = input.pressure;
    value.native.tilt_x = input.tilt_x; value.native.tilt_y = input.tilt_y;
    value.native.rotation = input.rotation; value.native.timestamp = input.timestamp;
    value.native.device_id = input.device_id; value.native.buttons = input.buttons;
    value.native.scroll_x = input.scroll_x; value.native.scroll_y = input.scroll_y;
    value.native.control = input.control != 0; value.native.shift = input.shift != 0;
    value.native.alt = input.alt != 0; value.native.meta = input.meta != 0;
    value.native.precise_scroll = input.precise_scroll != 0;
    return value;
}

// Map a detached pointer back to the C ABI; the caller provides the phase and
// source device because these use iiGUI's public enumeration order.
IiscGuiInputOutput guiOutput(const iiSharedCanvas::CanvasInputEvent &value, int phase, int device,
                             const PointerEvent &pointer)
{
    return {value.kind == iiSharedCanvas::CanvasInputKind::Scroll, phase, device, int(pointer.button),
        pointer.hovering, pointer.eraserActive, pointer.primaryButtonDown,
        pointer.documentPosition.x, pointer.documentPosition.y, pointer.pressure,
        pointer.tiltX, pointer.tiltY, pointer.rotationRadians, pointer.time,
        value.deviceId, value.nativeButtons, pointer.deviceState, value.scrollX, value.scrollY,
        value.control, value.shift, value.alt, value.meta, value.preciseScroll};
}

} // namespace

extern "C" {

int iisc_gui_translate(const IiscGuiInput *input, IiscGuiInputOutput *output)
{
    if (!input || !output || input->phase < 0 || input->phase > 5 || input->device < 0 || input->device > 3)
        return 0;
    try {
        auto value = guiInput(*input);
        const auto converted = iiSharedCanvas::CanvasInputTranslator{}.execute(value);
        if (!converted) return 0;
        *output = guiOutput(*converted, input->phase, input->device, converted->pointer);
        return 1;
    } catch (...) { return 0; }
}

int iisc_gui_route(const IiscGuiInput *input, double screen_x, double screen_y,
    const IiscGuiGestureState *state, const IiscGuiViewport *viewport,
    const IiscGuiInteractionOptions *options, IiscGuiInteractionOutput *output)
{
    if (!input || !state || !viewport || !options || !output || input->phase < 0 || input->phase > 5
        || input->device < 0 || input->device > 3 || state->mode < 0 || state->mode > 2
        || state->device_kind < 0 || state->device_kind > 2 || options->tool < 0 || options->tool > 3) return 0;
    try {
        auto raw = guiInput(*input);
        raw.native.local_position = ii::gui::WidgetPoint{screen_x, screen_y};
        const auto event = iiSharedCanvas::CanvasInputTranslator{}.execute(raw);
        if (!event) return 0;
        const iiSharedCanvas::CanvasInteractionState prior{
            static_cast<iiSharedCanvas::CanvasGestureMode>(state->mode), state->device_id,
            state->last_x, state->last_y, state->eraser != 0, static_cast<PointerDeviceKind>(state->device_kind)};
        const ii::gui::EditorCanvasViewportConfiguration view{viewport->scale, viewport->offset_x,
            viewport->offset_y, viewport->rotation_radians, viewport->flip_x != 0, viewport->flip_y != 0};
        const iiSharedCanvas::CanvasInteractionOptions policy{static_cast<iiSharedCanvas::CanvasTool>(options->tool),
            options->raster_selected != 0, options->minimum_zoom, options->maximum_zoom, options->scroll_sensitivity};
        const auto result = iiSharedCanvas::CanvasInteractionRouter{}.execute(prior, view, policy, *event);
        if (!result) return 0;
        IiscGuiInteractionOutput detached{};
        detached.state = {int(result->state.mode), result->state.deviceId,
            result->state.lastScreenX, result->state.lastScreenY, result->state.eraser, int(result->state.device)};
        detached.viewport = {result->viewport.scale, result->viewport.offset_x, result->viewport.offset_y,
            result->viewport.rotation_radians, result->viewport.flip_x, result->viewport.flip_y};
        detached.has_stroke = result->stroke.has_value(); detached.viewport_changed = result->viewportChanged;
        detached.eraser = result->eraser;
        if (result->stroke)
            detached.stroke = guiOutput(*event, int(result->stroke->phase), input->device, *result->stroke);
        *output = detached;
        return 1;
    } catch (...) { return 0; }
}

int iisc_gui_present(const uint32_t *pixels, size_t count, int32_t source_width, int32_t source_height,
    int32_t x, int32_t y, int32_t region_width, int32_t region_height,
    double surface_width, double surface_height, double scale, double offset_x, double offset_y,
    double rotation_radians, int flip_x, int flip_y, int smooth, uint64_t maximum_pixels,
    uint8_t *output, size_t output_bytes, uint32_t *output_width, uint32_t *output_height)
{
    if (!pixels || !output_width || !output_height || source_width <= 0 || source_height <= 0
        || std::uint64_t(source_width) * std::uint64_t(source_height) != count
        || count > maximum_pixels || (smooth != 0 && smooth != 1)) return 0;
    try {
        iiSharedCanvas::FrameRenderResult frame;
        frame.origin = {x, y}; frame.pixels.width = source_width; frame.pixels.height = source_height;
        frame.pixels.pixels.assign(pixels, pixels + count);
        ii::gui::EditorCanvasQuery query;
        query.surface = {surface_width, surface_height};
        query.transform = {scale, offset_x, offset_y, rotation_radians, flip_x != 0, flip_y != 0};
        auto drawing = iiSharedCanvas::CanvasFramePresenter{}.execute(frame, {{x, y}, {region_width, region_height}},
            query, smooth ? iiSharedCanvas::RasterSampling::Smooth : iiSharedCanvas::RasterSampling::Nearest,
            maximum_pixels);
        if (!drawing) return 0;
        if ((*drawing)->images.empty()) { *output_width = 0; *output_height = 0; return 1; }
        const auto &image = (*drawing)->images.front().image;
        const auto rgba = image->rgba();
        if (!output || output_bytes < rgba.size()) return 0;
        std::copy(rgba.begin(), rgba.end(), output);
        *output_width = image->width(); *output_height = image->height();
        return 1;
    } catch (...) { return 0; }
}

} // extern "C"
