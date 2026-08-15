#include <iiSharedCanvas.h>

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
    return validate(document).ok()
        && asset
        && assetId(*asset) == "installed-raster"
        ? 0
        : 1;
}
