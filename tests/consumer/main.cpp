#include <iiSharedCanvas.h>

#include <cstdint>
#include <optional>
#include <variant>

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

    VectorPath shape;
    shape.commands = {
        MoveTo{{0.0, 0.0}},
        LineTo{{3.0, 0.0}},
        LineTo{{3.0, 3.0}},
        ClosePath{},
    };
    shape.fill = SolidPaint{0xff00cc88U};
    shape.stroke = StrokeStyle{SolidPaint{0xff102030U}, 0.75};
    document.assets.emplace_back(VectorAsset{"installed-shape", {4, 4}, {shape}});

    AffineTransform shapeTransform;
    shapeTransform.translationX = 0.5;
    shapeTransform.translationY = 1.0;
    document.layers.push_back({
        "installed-shape-layer",
        "Detailed shape",
        false,
        0.625,
        shapeTransform,
        RasterBlendMode::Multiply,
        StaticSource{"installed-shape"},
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
    const RasterAsset *decodedImage = decoded.ok()
        ? findRasterAsset(decoded.document, "installed-pixels")
        : nullptr;
    const VectorAsset *decodedShape = decoded.ok()
        ? findVectorAsset(decoded.document, "installed-shape")
        : nullptr;
    const Layer *decodedShapeLayer = decoded.ok()
        ? findLayer(decoded.document, "installed-shape-layer")
        : nullptr;
    const StaticSource *decodedShapeSource = decodedShapeLayer
        ? std::get_if<StaticSource>(&decodedShapeLayer->source)
        : nullptr;
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
        && decodedImage
        && decodedImage->pixels.width == 4
        && decodedImage->pixels.height == 4
        && rasterLayerPixelAt(decodedImage->pixels, {2, 1}) == 0xffaabbccU
        && decodedShape
        && decodedShape->viewport.width == 4
        && decodedShape->paths.size() == 1
        && decodedShape->paths.front().commands.size() == 4
        && decodedShape->paths.front().fill
        && decodedShape->paths.front().fill->argb == 0xff00cc88U
        && decodedShape->paths.front().stroke
        && decodedShape->paths.front().stroke->width == 0.75
        && decodedShapeLayer
        && decodedShapeLayer->name == "Detailed shape"
        && !decodedShapeLayer->visible
        && decodedShapeLayer->opacity == 0.625
        && decodedShapeLayer->transform.translationX == 0.5
        && decodedShapeLayer->transform.translationY == 1.0
        && decodedShapeLayer->blendMode == RasterBlendMode::Multiply
        && decodedShapeSource
        && decodedShapeSource->assetId == "installed-shape"
        && encodeIisc(decoded.document).bytes == encoded.bytes
        ? 0
        : 1;
}
