#include <iiSharedCanvas.h>

#include <cstdint>
#include <optional>

int main()
{
    using namespace iiSharedCanvas;

    Document document;
    document.extent = {4, 4};
    document.timeline = {{24, 1}, 1};
    document.assets.emplace_back(
        RasterAsset{"installed-raster", makeRasterLayer(4, 4, 0xff112233U)});
    document.layers.push_back({
        "installed-layer",
        "Installed package",
        true,
        1.0,
        {},
        RasterBlendMode::SourceOver,
        StaticSource{"installed-raster"},
    });

    const Asset *asset = resolveAssetAt(document, document.layers.front(), 0);
    BitmapEditor editor(document, "installed-raster");
    const bool edited = editor.setPixel(2, 1, 0xffaabbccU);
    return validate(document).ok()
        && asset
        && assetId(*asset) == "installed-raster"
        && edited
        && editor.pixelAt(2, 1) == std::optional<std::uint32_t>{0xffaabbccU}
        ? 0
        : 1;
}
