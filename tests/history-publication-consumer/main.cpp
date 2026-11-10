#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>
#include <filesystem>
#include <iostream>

int main() {
    using namespace iiSharedCanvas;
    iiFileProvider::native::TemporaryDirectory directory(std::filesystem::temp_directory_path());
    if (!directory.isValid()) return 1;
    Document document; document.extent = {64, 64};
    document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(64, 64)});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentFile file;
    if (!file.create((directory.path() / "consumer.iisc").string(), document).ok()) return 2;
    const auto prior = document.authorship;
    const auto version = document.formatVersion;
    BitmapEditor editor(document, "paint");
    if (!editor.setPixel(4, 5, 0xffab12cd) ||
        !editor.publishHistoryState(file, BitmapHistoryState::Applied, prior, version, file.revision()) ||
        !editor.canUndo()) return 3;
    Document reopened;
    if (!DocumentFile::readSnapshot(file.filePath(), reopened).ok()) return 4;
    const auto *asset = findRasterAsset(reopened, "paint");
    if (!asset || asset->pixels.pixels[5 * 64 + 4] != 0xffab12cd) return 5;
    std::cout << "INSTALLED_HISTORY_PUBLICATION_OK\n";
    return 0;
}
