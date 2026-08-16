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

    DocumentEditor structure(document);
    const DocumentEditResult renamed = structure.renameAsset(
        "installed-raster", "installed-pixels");
    const DocumentEditResult layerName = structure.setLayerName(
        "installed-layer", "Edited installed package");
    const Asset *asset = resolveAssetAt(document, document.layers.front(), 0);
    BitmapEditor editor(document, "installed-pixels");
    const bool edited = editor.setPixel(2, 1, 0xffaabbccU);
    const FrameRenderResult rendered = renderFrame(document, 0);
    const IiscEncodeResult encoded = encodeIisc(document);
    const IiscDecodeResult decoded = encoded.ok()
        ? decodeIisc(encoded.bytes)
        : IiscDecodeResult{};
    return validate(document).ok()
        && asset
        && renamed.ok()
        && renamed.changed
        && layerName.ok()
        && document.layers.front().name == "Edited installed package"
        && assetId(*asset) == "installed-pixels"
        && edited
        && editor.pixelAt(2, 1) == std::optional<std::uint32_t>{0xffaabbccU}
        && rendered.ok()
        && rasterLayerPixelAt(rendered.pixels, {2, 1}) == 0xffaabbccU
        && decoded.ok()
        && encodeIisc(decoded.document).bytes == encoded.bytes
        ? 0
        : 1;
}
