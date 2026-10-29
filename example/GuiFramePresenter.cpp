#include "GuiFramePresenter.h"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iiSharedCanvas::example {
ii::core::Result<GuiCanvasFrame> toGuiCanvasFrame(const FrameRenderResult &frame, std::uint64_t maxPixels)
{
    using ii::core::ErrorCode;
    try {
        if (!frame.ok())
            return ii::core::fail(ErrorCode::failed,
                                  frame.message.empty() ? "Canvas frame rendering failed" : frame.message);
        const auto width = frame.pixels.width, height = frame.pixels.height;
        constexpr auto maximumDimension = ii::gui::maximum_image_dimension;
        constexpr std::uint64_t maximumPixels = std::uint64_t(maximumDimension) * maximumDimension;
        if (width <= 0 || height <= 0 || width > int(maximumDimension) || height > int(maximumDimension)
            || !maxPixels || maxPixels > maximumPixels)
            return ii::core::fail(ErrorCode::invalid_argument, "Canvas frame exceeds GUI dimensions or pixel budget");
        const auto pixels = std::uint64_t(width) * std::uint64_t(height);
        if (pixels > maxPixels || pixels > std::numeric_limits<std::size_t>::max() / 4
            || frame.pixels.pixels.size() != pixels)
            return ii::core::fail(ErrorCode::invalid_argument, "Canvas frame has invalid pixels or exceeds its budget");
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(pixels) * 4);
        for (std::size_t i = 0; i < frame.pixels.pixels.size(); ++i) {
            const auto argb = frame.pixels.pixels[i];
            rgba[4 * i] = std::uint8_t(argb >> 16);
            rgba[4 * i + 1] = std::uint8_t(argb >> 8);
            rgba[4 * i + 2] = std::uint8_t(argb);
            rgba[4 * i + 3] = std::uint8_t(argb >> 24);
        }
        auto image = ii::gui::rgba_image(std::uint32_t(width), std::uint32_t(height), std::move(rgba),
                                         ii::gui::ImageBudget{maxPixels});
        if (!image) return std::unexpected(image.error());
        return GuiCanvasFrame{frame.origin, std::move(*image)};
    } catch (const std::bad_alloc &) {
        return ii::core::fail(ErrorCode::failed, "Cannot allocate a GUI frame image");
    } catch (const std::length_error &) {
        return ii::core::fail(ErrorCode::failed, "GUI frame image is too large");
    }
}
} // namespace iiSharedCanvas::example
