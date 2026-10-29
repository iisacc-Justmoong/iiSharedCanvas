#pragma once

#include <Render/FrameRenderer.h>
#include <iiGUI/Image.h>

#include <cstdint>
#include <memory>

namespace iiSharedCanvas::example {

struct GuiCanvasFrame {
    CanvasOrigin origin;
    std::shared_ptr<const ii::gui::RasterImage> image;
};

// The application composes the canvas renderer and iiGUI. A successful frame
// remains detached; setting an image on a widget is the application's job.
[[nodiscard]] ii::core::Result<GuiCanvasFrame> toGuiCanvasFrame(
    const FrameRenderResult &frame,
    std::uint64_t maxPixels = ii::gui::maximum_image_pixels);

} // namespace iiSharedCanvas::example
