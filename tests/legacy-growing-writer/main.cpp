#include <iiSharedCanvasNative.h>
#include <filesystem>
#include <iostream>

// Reproduce the frozen fixture with the installed Native 0.34.0 writer.
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    using namespace iiSharedCanvas;
    Document document;
    document.extent = {128, 128};
    document.canvasMode = CanvasMode::Infinite;
    document.infiniteCanvas = {{-64, -64}, 32};
    auto hidden = makeRasterLayer(32, 32);
    hidden.pixels[3] = 0xff123456;
    document.assets.emplace_back(ChunkedRasterAsset{"paint", {{-4, -4, std::move(hidden)}}});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentFile file;
    const auto result = file.create(argv[1], document);
    if (!result.ok()) { std::cerr << result.message << '\n'; return 1; }
    file.close();
    std::cout << "LEGACY_GROWING_FIXTURE_CREATED\n";
}
