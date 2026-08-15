#include <iiSharedCanvas.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

iiSharedCanvas::Document makeDocument()
{
    using namespace iiSharedCanvas;

    Document document;
    document.extent = {16, 16};
    document.timeline = {{24, 1}, 1};
    document.assets.emplace_back(
        RasterAsset{"paint", makeRasterLayer(16, 16, 0x00000000U)});
    document.assets.emplace_back(
        VectorAsset{"vector", {16, 16}, {}});
    document.layers.push_back({
        "paint-layer",
        "Paint",
        true,
        1.0,
        {},
        RasterBlendMode::SourceOver,
        StaticSource{"paint"},
    });
    return document;
}

std::size_t countVisiblePixels(const RasterLayer &layer)
{
    return static_cast<std::size_t>(std::count_if(
        layer.pixels.begin(), layer.pixels.end(), [](std::uint32_t pixel) {
            return (pixel >> 24U) != 0U;
        }));
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    Document document = makeDocument();
    BitmapEditor editor;

    expect(!editor.bind(document, "missing"),
           "binding a missing asset must fail closed");
    expect(!editor.bind(document, "vector"),
           "binding a vector asset as a bitmap must fail closed");
    expect(editor.bind(document, "paint"),
           "a valid raster asset must be editable");
    expect(editor.isBound() && editor.width() == 16 && editor.height() == 16,
           "binding must expose the raster dimensions");

    expect(editor.setPixel(3, 4, 0xff112233U),
           "setPixel must mutate a valid bitmap coordinate");
    expect(editor.pixelAt(3, 4) == std::optional<std::uint32_t>{0xff112233U},
           "setPixel must update the selected RasterAsset itself");
    expect(!editor.setPixel(-1, 4, 0xffffffffU),
           "setPixel must reject an out-of-bounds coordinate");
    expect(editor.canUndo(), "a pixel mutation must create undo history");
    expect(editor.undo(), "pixel mutation must be undoable");
    expect(editor.pixelAt(3, 4) == std::optional<std::uint32_t>{0x00000000U},
           "undo must restore the prior pixel value");
    expect(editor.redo(), "pixel mutation must be redoable");
    expect(editor.pixelAt(3, 4) == std::optional<std::uint32_t>{0xff112233U},
           "redo must restore the edited pixel value");

    const DevicePixelRect patchBounds{{1, 1}, 2, 2};
    const std::vector<std::uint32_t> patch{
        0xffff0000U, 0xff00ff00U,
        0xff0000ffU, 0xffffffffU,
    };
    expect(editor.replacePatch(patchBounds, patch),
           "a complete in-bounds ARGB patch must be accepted");
    expect(editor.pixelAt(1, 1) == std::optional<std::uint32_t>{0xffff0000U}
               && editor.pixelAt(2, 2) == std::optional<std::uint32_t>{0xffffffffU},
           "replacePatch must preserve row-major pixel order");
    expect(!editor.replacePatch({{15, 15}, 2, 2}, patch),
           "a patch extending outside the raster must fail closed");

    expect(editor.clear(0xff204060U),
           "clear must replace the entire selected raster");
    expect(editor.pixelAt(0, 0) == std::optional<std::uint32_t>{0xff204060U}
               && editor.pixelAt(15, 15) == std::optional<std::uint32_t>{0xff204060U},
           "clear must cover every bitmap pixel");

    BitmapBrush brush;
    brush.argb = 0xffffcc00U;
    brush.size = 3.0;
    brush.flow = 1.0;
    brush.opacity = 1.0;
    brush.hardness = 1.0;
    brush.spacingRatio = 0.15;
    expect(editor.setBrush(brush), "a finite brush configuration must be accepted");
    expect(editor.beginStroke({2.0, 8.0}, 1.0),
           "a bitmap brush stroke must begin on the selected raster");
    expect(editor.continueStroke({13.0, 8.0}, 1.0),
           "a bitmap brush stroke must accept streaming pointer positions");
    expect(editor.endStroke({13.0, 8.0}, 1.0),
           "a bitmap brush stroke must commit its final position");

    const RasterLayer *painted = editor.pixels();
    expect(painted && countVisiblePixels(*painted) == 256,
           "painting over an opaque bitmap must keep the bitmap fully visible");
    expect(editor.pixelAt(8, 8).has_value()
               && (editor.pixelAt(8, 8).value() & 0x00ffffffU) != 0x00204060U,
           "the iiPaintEngine rasterizer must commit brush color into RasterAsset pixels");

    const std::uint32_t beforeErase = editor.pixelAt(8, 8).value_or(0U);
    brush.eraser = true;
    brush.size = 5.0;
    expect(editor.setBrush(brush), "eraser mode must be a brush configuration");
    expect(editor.beginStroke({8.0, 8.0}, 1.0)
               && editor.endStroke({8.0, 8.0}, 1.0),
           "an eraser stroke must commit through the same raster path");
    const std::uint32_t afterErase = editor.pixelAt(8, 8).value_or(0U);
    expect((afterErase >> 24U) < (beforeErase >> 24U),
           "DestinationOut brush compositing must reduce pixel alpha");
    expect(editor.undo(), "an entire streamed brush stroke must be one undo operation");
    expect(editor.pixelAt(8, 8) == std::optional<std::uint32_t>{beforeErase},
           "undo must restore the pre-stroke bitmap snapshot");
    expect(editor.canRedo(), "undoing a stroke must make that stroke redoable");

    expect(editor.beginStroke({-100.0, -100.0}, 1.0),
           "an off-canvas pointer sequence must remain a valid streaming input");
    expect(!editor.setBrush(brush),
           "brush settings must remain stable for the duration of one stroke");
    expect(editor.endStroke({-100.0, -100.0}, 1.0),
           "an off-canvas no-op stroke must terminate normally");
    expect(editor.canRedo(),
           "a no-op stroke must not discard redo history");

    const RasterLayer beforeCancelledStroke = *editor.pixels();
    brush.eraser = false;
    brush.argb = 0xff00ffffU;
    expect(editor.setBrush(brush), "brush mode must be restorable after erasing");
    expect(editor.beginStroke({4.0, 4.0}, 1.0),
           "a cancellable stroke must begin");
    editor.cancelStroke();
    expect(editor.pixels() && editor.pixels()->pixels == beforeCancelledStroke.pixels,
           "cancelling a stroke must restore pixels without replaying input");

    RasterLayer replacement = makeRasterLayer(8, 6, 0xffabcdefU);
    expect(editor.replacePixels(replacement),
           "a valid iiPaintEngine RasterLayer must replace the selected bitmap");
    expect(editor.width() == 8 && editor.height() == 6
               && editor.pixelAt(7, 5) == std::optional<std::uint32_t>{0xffabcdefU},
           "whole-bitmap replacement must update dimensions and pixels together");

    editor.unbind();
    expect(!editor.isBound() && !editor.setPixel(0, 0, 0xffffffffU),
           "an unbound editor must reject mutations");

    return failures == 0 ? 0 : 1;
}
