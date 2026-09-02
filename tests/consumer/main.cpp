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
    document.layers.emplace_back(BitmapLayer{
        {"installed-layer", "Installed package", true, 1.0, {},
         RasterBlendMode::SourceOver},
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
    document.layers.emplace_back(VectorLayer{
        {"installed-shape-layer", "Detailed shape", false, 0.625, shapeTransform,
         RasterBlendMode::Multiply},
        StaticSource{"installed-shape"},
    });

    DocumentEditor structure(document);
    const DocumentEditResult renamed = structure.renameAsset(
        "installed-raster", "installed-pixels");
    const DocumentEditResult layerName = structure.setLayerName(
        "installed-layer", "Edited installed package");
    const Automatic1111ParseResult automatic1111 =
        parseAutomatic1111Infotext(
            "installed package prompt\n"
            "Steps: 20, Sampler: Euler, CFG scale: 7, Seed: 77, "
            "Size: 4x4, Model hash: 0123456789, Model: installed-model");
    StableDiffusionMetadata generation;
    generation.positivePrompt = "installed package test image";
    generation.negativePrompt = "watermark";
    generation.outputExtent = StableDiffusionImageExtent{4, 4};
    generation.samplingPasses.push_back({
        "3", 77, 20, 7.0, "euler", "normal", 1.0,
        std::nullopt, std::nullopt,
    });
    generation.software = "ComfyUI";
    generation.comfyUi.promptJson =
        R"json({"3":{"class_type":"KSampler","inputs":{"seed":77,"steps":20,"cfg":7.0}}})json";
    generation.comfyUi.workflowJson =
        R"json({"version":1,"state":{},"nodes":[]})json";
    const DocumentEditResult generationEdit =
        structure.setStableDiffusionMetadata(generation);
    const Asset *asset = resolveAssetAt(document, document.layers.front(), 0);
    BitmapEditor editor(document, "installed-pixels");
    const bool edited = editor.setPixel(2, 1, 0xffaabbccU);
    const FrameRenderResult rendered = renderFrame(document, 0);
    const FrameRenderTileRequest installedTileRequest{
        {{0, 0}, {4, 4}}, {2, 2}};
    const FrameTileRenderResult renderedTile = renderFrameTiles(
        document, 0, {installedTileRequest});
    const FrameLayerBatchRenderResult renderedLayers = renderFrameLayers(
        document, 0, {installedTileRequest});
    const FrameTileRenderResult recomposedLayers = composeFrameLayers(renderedLayers);
    AsyncFrameRenderer asyncRenderer;
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
        ? std::get_if<StaticSource>(&layerSource(*decodedShapeLayer))
        : nullptr;
    const LayerProperties *decodedShapeProperties = decodedShapeLayer
        ? &layerProperties(*decodedShapeLayer)
        : nullptr;

    CameraRawData cameraRaw;
    cameraRaw.image.kind = CameraRawImageKind::Monochrome;
    cameraRaw.image.extent = {2, 1};
    cameraRaw.image.bitsPerSample = 12;
    cameraRaw.image.samplesPerPixel = 1;
    cameraRaw.image.samples = {64, 2048};
    cameraRaw.image.colorChannels = {
        {CameraRawChannelRole::Luminance, "luminance"},
    };
    return validate(document).ok()
        && validateCameraRaw(cameraRaw).ok()
        && cameraRawSampleAt(cameraRaw.image, 1, 0)
            == std::optional<std::uint32_t>{2048}
        && asset
        && renamed.ok()
        && renamed.changed
        && layerName.ok()
        && automatic1111.ok()
        && automatic1111.metadata.outputExtent
            == std::optional<StableDiffusionImageExtent>{{4, 4}}
        && automatic1111.metadata.models.size() == 1
        && automatic1111.metadata.models.front().hashType
            == "sha256-prefix-10"
        && generationEdit.ok()
        && generationEdit.changed
        && layerProperties(document.layers.front()).name == "Edited installed package"
        && assetId(*asset) == "installed-pixels"
        && edited
        && editor.pixelAt(2, 1) == std::optional<std::uint32_t>{0xffaabbccU}
        && rendered.ok()
        && rasterLayerPixelAt(rendered.pixels, {2, 1}) == 0xffaabbccU
        && renderedTile.ok()
        && renderedTile.tiles.size() == 1
        && renderedTile.tiles.front().pixels.width == 2
        && renderedLayers.ok()
        && renderedLayers.layers.size() == 2
        && renderedLayers.layers.front().layerId == "installed-layer"
        && renderedLayers.layers.back().layerId == "installed-shape-layer"
        && !renderedLayers.layers.back().visible
        && renderedLayers.layers.back().tiles.empty()
        && recomposedLayers.ok()
        && recomposedLayers.tiles.front().pixels.pixels
            == renderedTile.tiles.front().pixels.pixels
        && !asyncRenderer.busy()
        && decoded.ok()
        && decoded.document.stableDiffusionMetadata == generation
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
        && std::holds_alternative<VectorLayer>(*decodedShapeLayer)
        && decodedShapeProperties
        && decodedShapeProperties->name == "Detailed shape"
        && !decodedShapeProperties->visible
        && decodedShapeProperties->opacity == 0.625
        && decodedShapeProperties->transform.translationX == 0.5
        && decodedShapeProperties->transform.translationY == 1.0
        && decodedShapeProperties->blendMode == RasterBlendMode::Multiply
        && decodedShapeSource
        && decodedShapeSource->assetId == "installed-shape"
        && encodeIisc(decoded.document).bytes == encoded.bytes
        ? 0
        : 1;
}
