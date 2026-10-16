#include <iiSharedCanvasCore.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    const auto path = fs::path(argv[1]) / ("core-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".iisc");
    struct Cleanup { fs::path path; ~Cleanup() {
        std::error_code error;
        for (const auto &suffix : {"", "-wal", "-shm"}) fs::remove(path.string() + suffix, error);
    } } cleanup{path};
    try {
        using namespace iiSharedCanvas;
        Document document;
        document.extent = {4, 4};
        document.assets.emplace_back(RasterAsset{"pixels", makeRasterLayer(4, 4, 0xff428542U)});
        document.layers.emplace_back(StaticBitmapLayer{{"layer", "Native pixels"}, StaticSource{"pixels"}});
        StableDiffusionMetadata metadata; metadata.software = "Core consumer";
        document.stableDiffusionMetadata = metadata;
        const auto bytes = encodeIisc(document);
        require(bytes.ok(), "encode IISC");
        const auto decoded = decodeIisc(bytes.bytes);
        require(decoded.ok(), "decode IISC");
        require(decoded.document.stableDiffusionMetadata->software == metadata.software, "preserve metadata");
        {
            DocumentFile file;
            require(file.create(path.string(), decoded.document).ok(), "create working document");
        }
        DocumentFile reopened;
        require(reopened.open(path.string()).ok(), "reopen working document");
        const auto rendered = renderFrame(*reopened.document(), 0);
        require(rendered.ok() && rendered.pixels.width == 4 && rendered.pixels.height == 4,
                "render restored document");
        require(rendered.pixels.pixels.front() == 0xff428542U, "preserve restored pixel color");
        std::cout << "IISC metadata, working-file reopen, and CPU render passed without QML or Quick\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
