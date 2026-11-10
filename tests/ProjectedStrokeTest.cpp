#include <iiSharedCanvas.h>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace iiSharedCanvas;
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Editor>
void exercise(Editor& editor) {
    check(editor.setDeferredStrokeHistory(true), "history setup failed");
    check(editor.beginProjectedStroke(), "projected stroke begin failed");
    const std::vector<RasterSample> first{{{2, 2}, 0x804080ffU, 255, RasterBlendMode::SourceOver}};
    const std::vector<RasterSample> oldTail{{{3, 3}, 0x804080ffU, 255, RasterBlendMode::SourceOver}};
    const std::vector<RasterSample> newTail{{{4, 4}, 0x804080ffU, 255, RasterBlendMode::SourceOver}};
    check(editor.updateProjectedStroke(first, oldTail), "first update failed");
    const auto stablePixel = editor.pixelAt(2, 2);
    check(editor.pixelAt(2, 2).value_or(0) != 0 && editor.pixelAt(3, 3).value_or(0) != 0,
          "stable or transient pixels were not published");
    check(editor.strokeSourceAt(3, 3).value_or(1) == 0, "transient preview changed the source");
    const std::vector<RasterSample> overlap{{{2, 2}, 0x8033ff00U, 255, RasterBlendMode::SourceOver}};
    check(editor.updateProjectedStroke({}, overlap) && editor.pixelAt(2, 2) != stablePixel,
          "transient overlap failed");
    check(editor.updateProjectedStroke({}, newTail), "replacement update failed");
    check(editor.pixelAt(2, 2) == stablePixel && editor.pixelAt(3, 3).value_or(1) == 0 &&
          editor.pixelAt(4, 4).value_or(0) != 0, "replacement failed to restore old transient pixels");
    const std::vector<RasterSample> invalid{{{-1, 0}, 0xffffffffU, 255, RasterBlendMode::SourceOver}};
    check(!editor.updateProjectedStroke(invalid, {}) && editor.pixelAt(4, 4).value_or(0) != 0,
          "invalid samples changed a live preview");
    const auto displayed = editor.pixelAt(4, 4);
    check(editor.endProjectedStroke(), "projected stroke finish failed");
    check(editor.pixelAt(4, 4) == displayed, "finish changed the transient pixels");
    check(editor.undo() && editor.pixelAt(2, 2).value_or(1) == 0 &&
          editor.pixelAt(4, 4).value_or(1) == 0, "projected stroke undo failed");
    check(editor.redo() && editor.pixelAt(4, 4) == displayed, "projected stroke redo failed");
    check(editor.beginProjectedStroke() && editor.updateProjectedStroke({}, oldTail), "cancel setup failed");
    editor.cancelStroke();
    check(editor.pixelAt(3, 3).value_or(1) == 0 && editor.pixelAt(4, 4) == displayed,
          "cancel left transient pixels behind");
    check(editor.undo() && editor.canRedo(), "no-op redo fixture failed");
    check(editor.beginProjectedStroke() && editor.updateProjectedStroke({}, {}) &&
          editor.endProjectedStroke() && editor.canRedo(), "no-op projected stroke discarded redo");
    check(editor.beginProjectedStroke() && editor.updateProjectedStroke({}, oldTail) &&
          editor.updateProjectedStroke({}, newTail) && editor.pixelAt(3, 3).value_or(1) == 0,
          "transient-only replacement left the previous footprint");
    editor.cancelStroke();
    check(editor.pixelAt(4, 4).value_or(1) == 0 && editor.canRedo(),
          "transient-only cancel changed pixels or redo");
}
}
int main() try {
    Document raster; raster.extent = {16, 16};
    raster.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(16, 16)});
    BitmapEditor bitmap(raster, "paint"); exercise(bitmap);
    Document chunked; chunked.extent = {16, 16};
    chunked.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    ChunkedBitmapEditor chunks(chunked, "paint"); exercise(chunks);
    std::cout << "PROJECTED_STROKE_OK\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
