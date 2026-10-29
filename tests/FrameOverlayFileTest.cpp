#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>

#include <array>
#include <filesystem>
#include <iostream>
#include <string_view>

using namespace iiSharedCanvas;
int main() {
    int failures = 0;
    const auto expect = [&](bool value, std::string_view message) {
        if (!value) { ++failures; std::cerr << message << '\n'; }
    };
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { std::cerr << "file overlay: temporary directory\n"; return 1; }
    const auto path = (temporary.path() / "overlay.iisc").string();
    Document initial; initial.extent = {8, 8};
    initial.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(8, 8, 0xffcc2244U)});
    initial.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentFile owner;
    if (!owner.create(path, initial).ok()) { std::cerr << "file overlay: create\n"; return 1; }
    const auto revision = owner.revision(); const auto before = encodeIisc(*owner.document()).bytes;
    DocumentHistory history(owner);
    const std::array<std::uint32_t, 2> colors{0x00000000U, 0xff00cc44U};
    const std::array patches{BitmapPatch{{{3, 3}, 2, 1}, colors}};
    expect(history.beginStagedPixelPreview("paint").ok() && history.replacePixelPreview(patches).changed,
           "file overlay: stage final pixels");
    auto tiles = history.stagedPixelPreviewTiles();
    const std::array overlays{FrameBitmapOverlay{"paint", tiles}};
    const auto preview = renderFrame(*owner.document(), 0, overlays);
    Document snapshot;
    expect(preview.ok() && preview.pixels.pixels[3 * 8 + 3] == 0 &&
           preview.pixels.pixels[3 * 8 + 4] == colors[1] &&
           owner.revision() == revision && DocumentFile::readSnapshot(path, snapshot).ok() &&
           encodeIisc(snapshot).bytes == before,
           "file overlay: rendered pixels differ while another reader sees unchanged durable document");
    expect(history.commitPixelPreview().changed && owner.revision() == revision + 1,
           "file overlay: one commit advances the file revision once");
    const auto committed = renderFrame(*owner.document(), 0);
    expect(committed.ok() && preview.ok() && committed.pixels.pixels == preview.pixels.pixels &&
           DocumentFile::readSnapshot(path, snapshot).ok() &&
           findRasterAsset(snapshot, "paint")->pixels.pixels[3 * 8 + 4] == colors[1],
           "file overlay: committed output equals preview and is durable");
    expect(history.undo().changed && DocumentFile::readSnapshot(path, snapshot).ok() &&
           findRasterAsset(snapshot, "paint")->pixels.pixels[3 * 8 + 3] == 0xffcc2244U &&
           history.redo().changed && DocumentFile::readSnapshot(path, snapshot).ok() &&
           findRasterAsset(snapshot, "paint")->pixels.pixels[3 * 8 + 3] == 0,
           "file overlay: one undo and redo restore the expected durable pixels");
    return failures ? 1 : 0;
}
