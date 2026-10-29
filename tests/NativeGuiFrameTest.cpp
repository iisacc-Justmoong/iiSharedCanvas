#include "GuiFramePresenter.h"

#include <iiSharedCanvas.h>

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, std::string_view message)
{
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
}

int main()
{
    using namespace iiSharedCanvas;
    FrameRenderResult frame;
    frame.origin = {-3, 4};
    frame.pixels = makeRasterLayer(2, 2);
    frame.pixels.pixels = {0xffff0000U, 0x8000ff00U, 0x400000ffU, 0x00112233U};
    const auto image = example::toGuiCanvasFrame(frame);
    expect(image.has_value(), "convert a detached canvas frame into an immutable GUI image");
    if (image) {
        expect(image->origin.x == -3 && image->origin.y == 4
               && image->image && image->image->width() == 2 && image->image->height() == 2,
               "preserve the frame origin and dimensions");
        const std::vector<std::uint8_t> expected = {
            255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 64, 17, 34, 51, 0};
        const auto rgba = image->image->rgba();
        expect(std::vector<std::uint8_t>(rgba.begin(), rgba.end()) == expected,
               "preserve straight alpha and RGBA channels including transparent RGB");
        frame.pixels.pixels[0] = 0;
        expect(image->image->rgba()[0] == 255 && image->image->rgba()[3] == 255,
               "GUI image owns an immutable copy independent of the source frame");
        frame.pixels.pixels[0] = 0xffff0000U;
    }
    expect(frame.pixels.pixels == std::vector<std::uint32_t>({0xffff0000U, 0x8000ff00U, 0x400000ffU, 0x00112233U}),
           "presentation does not mutate source pixels");

    Document document;
    document.extent = {2, 2};
    document.assets.emplace_back(RasterAsset{"raster", frame.pixels});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Layer"}, StaticSource{"raster"}});
    const auto rendered = renderFrame(document, 0);
    const auto renderedImage = example::toGuiCanvasFrame(rendered);
    expect(rendered.ok() && renderedImage.has_value(), "present an actual canvas-rendered frame");
    if (rendered.ok() && renderedImage) {
        const auto rgba = renderedImage->image->rgba();
        const auto pixel = rendered.pixels.pixels[1];
        expect(rgba[4] == std::uint8_t(pixel >> 16) && rgba[5] == std::uint8_t(pixel >> 8)
               && rgba[6] == std::uint8_t(pixel) && rgba[7] == std::uint8_t(pixel >> 24),
               "rendered frame and GUI image contain the same pixel channels");
    }
    auto failed = frame;
    failed.status = FrameRenderStatus::InvalidDocument;
    expect(!example::toGuiCanvasFrame(failed), "reject failed frame renders");
    auto invalid = frame;
    invalid.pixels.pixels.pop_back();
    expect(!example::toGuiCanvasFrame(invalid), "reject incomplete pixel buffers");
    invalid = frame;
    invalid.pixels.width = 0;
    expect(!example::toGuiCanvasFrame(invalid), "reject empty image dimensions");
    invalid = frame;
    invalid.pixels.width = 8193;
    expect(!example::toGuiCanvasFrame(invalid), "reject dimensions beyond the GUI limit");
    expect(example::toGuiCanvasFrame(frame, 4).has_value(), "accept the exact caller pixel budget");
    expect(!example::toGuiCanvasFrame(frame, 3), "enforce the caller's pixel budget");
    expect(!example::toGuiCanvasFrame(frame, 0), "reject a zero pixel budget");
    expect(!example::toGuiCanvasFrame(frame, 64ULL * 1024 * 1024 + 1),
           "reject a budget beyond the GUI's hard limit");
    return failures ? 1 : 0;
}
