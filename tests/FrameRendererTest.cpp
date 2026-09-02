#include <iiSharedCanvas.h>

#include <iostream>
#include <string>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

std::uint32_t pixelAt(const RasterLayer &layer, int x, int y)
{
    return rasterLayerPixelAt(layer, {x, y});
}

iiSharedCanvas::Document makeDocument(int width, int height, iiSharedCanvas::FrameIndex frameCount = 1)
{
    iiSharedCanvas::Document document;
    document.extent = {width, height};
    document.timeline = {{24, 1}, frameCount};
    return document;
}

iiSharedCanvas::VectorAsset makeFilledRectangle(std::string id,
                                                int width,
                                                int height,
                                                double left,
                                                double top,
                                                double right,
                                                double bottom,
                                                std::uint32_t argb)
{
    using namespace iiSharedCanvas;

    VectorPath path;
    path.commands = {
        MoveTo{{left, top}},
        LineTo{{right, top}},
        LineTo{{right, bottom}},
        LineTo{{left, bottom}},
        ClosePath{},
    };
    path.fill = SolidPaint{argb};
    return {std::move(id), {width, height}, {std::move(path)}};
}

iiSharedCanvas::Layer staticBitmapLayer(std::string id, std::string assetId)
{
    return iiSharedCanvas::BitmapLayer{
        {std::move(id), "Layer", true, 1.0, {}, RasterBlendMode::SourceOver},
        iiSharedCanvas::StaticSource{std::move(assetId)},
    };
}

iiSharedCanvas::Layer staticVectorLayer(std::string id, std::string assetId)
{
    return iiSharedCanvas::VectorLayer{
        {std::move(id), "Layer", true, 1.0, {}, RasterBlendMode::SourceOver},
        iiSharedCanvas::StaticSource{std::move(assetId)},
    };
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    {
        Document document = makeDocument(4, 4);
        document.assets.emplace_back(RasterAsset{"background", makeRasterLayer(4, 4, 0xff0000ffU)});
        document.assets.emplace_back(makeFilledRectangle("vector", 4, 4, 1.0, 1.0, 3.0, 3.0,
                                                         0xffff0000U));
        document.layers.push_back(staticBitmapLayer("background-layer", "background"));
        document.layers.push_back(staticVectorLayer("vector-layer", "vector"));

        const FrameRenderTileRequest fullRequest{canvasRegion(document), document.extent};
        const FrameLayerBatchRenderResult layers = renderFrameLayers(
            document, 0, {fullRequest});
        expect(layers.ok() && layers.layers.size() == 2,
               "a mixed frame must expose one render result per document layer");
        expect(layers.layers[0].layerIndex == 0
                   && layers.layers[0].layerId == "background-layer"
                   && layers.layers[0].tiles.size() == 1
                   && pixelAt(layers.layers[0].tiles.front().pixels, 1, 1)
                       == 0xff0000ffU,
               "the bottom bitmap must render into its own layer tile");
        expect(layers.layers[1].layerIndex == 1
                   && layers.layers[1].layerId == "vector-layer"
                   && layers.layers[1].tiles.size() == 1
                   && pixelAt(layers.layers[1].tiles.front().pixels, 0, 0)
                       == 0x00000000U
                   && pixelAt(layers.layers[1].tiles.front().pixels, 1, 1)
                       == 0xffff0000U,
               "the top vector must remain isolated from lower pixels in its layer tile");
        const FrameTileRenderResult composedLayers = composeFrameLayers(layers);
        expect(composedLayers.ok()
                   && composedLayers.tiles.size() == 1
                   && pixelAt(composedLayers.tiles.front().pixels, 1, 1)
                       == 0xffff0000U,
               "layer tiles must compose in stable bottom-to-top order");

        const FrameLayerTileRenderResult vectorOnly = renderFrameLayerTiles(
            document, 0, 1, {fullRequest});
        expect(vectorOnly.ok()
                   && vectorOnly.layerId == "vector-layer"
                   && vectorOnly.tiles.size() == 1
                   && pixelAt(vectorOnly.tiles.front().pixels, 0, 0) == 0x00000000U,
               "one document layer must be independently renderable without lower layers");
        expect(renderFrameLayerTiles(document, 0, 2, {fullRequest}).status
                   == FrameRenderStatus::LayerOutOfRange,
               "an invalid layer index must fail closed");

        const FrameRenderResult result = renderFrame(document, 0);
        expect(result.ok(), "static raster and vector layers must render into one frame");
        expect(result.pixels.width == 4 && result.pixels.height == 4,
               "the rendered frame must use the document extent");
        expect(pixelAt(result.pixels, 0, 0) == 0xff0000ffU,
               "the bottom raster must remain visible outside vector geometry");
        expect(pixelAt(result.pixels, 1, 1) == 0xffff0000U
                   && pixelAt(result.pixels, 2, 2) == 0xffff0000U,
               "the top vector fill must cover its exact interior pixels");
        expect(pixelAt(result.pixels, 3, 3) == 0xff0000ffU,
               "vector geometry must clip to its declared bounds");
    }

    {
        Document document = makeDocument(3, 1);
        document.assets.emplace_back(RasterAsset{"translated", makeRasterLayer(1, 1, 0xffff0000U)});
        Layer layer = staticBitmapLayer("translated-layer", "translated");
        layerProperties(layer).transform.translationX = 1.0;
        document.layers.push_back(layer);

        const FrameRenderResult result = renderFrame(document, 0);
        expect(result.ok(), "a translated raster layer must render");
        expect(pixelAt(result.pixels, 0, 0) == 0x00000000U
                   && pixelAt(result.pixels, 1, 0) == 0xffff0000U
                   && pixelAt(result.pixels, 2, 0) == 0x00000000U,
               "affine translation must use document pixel coordinates and canvas clipping");
    }

    {
        Document document = makeDocument(1, 1);
        document.assets.emplace_back(RasterAsset{"blue", makeRasterLayer(1, 1, 0xff0000ffU)});
        document.assets.emplace_back(RasterAsset{"red", makeRasterLayer(1, 1, 0xffff0000U)});
        document.layers.push_back(staticBitmapLayer("blue-layer", "blue"));
        Layer red = staticBitmapLayer("red-layer", "red");
        layerProperties(red).opacity = 0.5;
        document.layers.push_back(red);

        const FrameLayerBatchRenderResult opacityLayers = renderFrameLayers(
            document, 0, {{{{0, 0}, {1, 1}}, {1, 1}}});
        expect(opacityLayers.ok()
                   && opacityLayers.layers[1].opacity == 0.5
                   && pixelAt(opacityLayers.layers[1].tiles.front().pixels, 0, 0)
                       == 0xffff0000U,
               "layer rendering must preserve unmodified pixels and carry opacity as metadata");

        layerProperties(document.layers.back()).visible = false;
        const FrameLayerBatchRenderResult hiddenLayers = renderFrameLayers(
            document, 0, {{{{0, 0}, {1, 1}}, {1, 1}}});
        const FrameTileRenderResult hiddenComposite = composeFrameLayers(hiddenLayers);
        expect(hiddenLayers.ok()
                   && hiddenLayers.layers.size() == 2
                   && !hiddenLayers.layers.back().visible
                   && hiddenLayers.layers.back().tiles.empty()
                   && hiddenComposite.ok()
                   && pixelAt(hiddenComposite.tiles.front().pixels, 0, 0)
                       == 0xff0000ffU,
               "a hidden layer must retain ordered metadata without allocating layer tiles");
        layerProperties(document.layers.back()).visible = true;

        const FrameRenderResult result = renderFrame(document, 0);
        expect(result.ok() && pixelAt(result.pixels, 0, 0) == 0xff800080U,
               "layer opacity must be applied by iiPaintEngine composition");

        layerProperties(document.layers.back()).opacity = 1.0;
        layerProperties(document.layers.back()).blendMode = RasterBlendMode::Multiply;
        const FrameRenderResult multiplied = renderFrame(document, 0);
        expect(multiplied.ok() && pixelAt(multiplied.pixels, 0, 0) == 0xff000000U,
               "supported iiPaintEngine layer blend modes must remain exact");

        layerProperties(document.layers.back()).blendMode = RasterBlendMode::Screen;
        const FrameRenderResult screened = renderFrame(document, 0);
        expect(screened.ok() && pixelAt(screened.pixels, 0, 0) == 0xffff00ffU,
               "screen layer blending must remain owned by iiPaintEngine");

        std::get<RasterAsset>(document.assets[0]).pixels.pixels[0] = 0xff202020U;
        std::get<RasterAsset>(document.assets[1]).pixels.pixels[0] = 0xffffffffU;
        layerProperties(document.layers.back()).blendMode = RasterBlendMode::Overlay;
        const FrameRenderResult overlaid = renderFrame(document, 0);
        expect(overlaid.ok() && pixelAt(overlaid.pixels, 0, 0) == 0xff404040U,
               "overlay layer blending must remain owned by iiPaintEngine");
    }

    {
        Document document = makeDocument(2, 2);
        RasterLayer source = makeRasterLayer(2, 1, 0xffff0000U);
        source.pixels[1] = 0xff00ff00U;
        document.assets.emplace_back(RasterAsset{"rotated", std::move(source)});
        Layer layer = staticBitmapLayer("rotated-layer", "rotated");
        LayerProperties &properties = layerProperties(layer);
        properties.transform.m11 = 0.0;
        properties.transform.m12 = 1.0;
        properties.transform.m21 = -1.0;
        properties.transform.m22 = 0.0;
        properties.transform.translationX = 2.0;
        document.layers.push_back(layer);

        const FrameRenderResult result = renderFrame(document, 0);
        expect(result.ok()
                   && pixelAt(result.pixels, 1, 0) == 0xffff0000U
                   && pixelAt(result.pixels, 1, 1) == 0xff00ff00U,
               "the complete iiPaintEngine affine matrix must drive raster sampling");
        expect(pixelAt(result.pixels, 0, 0) == 0x00000000U,
               "rotated content must remain clipped to its transformed footprint");
    }

    {
        Document document = makeDocument(1, 1, 2);
        document.assets.emplace_back(RasterAsset{"raster-0", makeRasterLayer(1, 1, 0xffff0000U)});
        document.assets.emplace_back(RasterAsset{"raster-1", makeRasterLayer(1, 1, 0xff00ff00U)});
        document.layers.emplace_back(BitmapLayer{
            {"animated-raster", "Animated raster", true, 1.0, {},
             RasterBlendMode::SourceOver},
            KeyframedSource{{{0, "raster-0"}, {1, "raster-1"}}},
        });

        expect(pixelAt(renderFrame(document, 0).pixels, 0, 0) == 0xffff0000U
                   && pixelAt(renderFrame(document, 1).pixels, 0, 0) == 0xff00ff00U,
               "raster keyframes must switch on their exact hold-sampling boundary");
    }

    {
        Document document = makeDocument(1, 1, 2);
        document.assets.emplace_back(makeFilledRectangle("vector-0", 1, 1, 0.0, 0.0, 1.0, 1.0,
                                                         0xff0000ffU));
        document.assets.emplace_back(makeFilledRectangle("vector-1", 1, 1, 0.0, 0.0, 1.0, 1.0,
                                                         0xffffffffU));
        document.layers.emplace_back(VectorLayer{
            {"animated-vector", "Animated vector", true, 1.0, {},
             RasterBlendMode::SourceOver},
            KeyframedSource{{{0, "vector-0"}, {1, "vector-1"}}},
        });

        expect(pixelAt(renderFrame(document, 0).pixels, 0, 0) == 0xff0000ffU
                   && pixelAt(renderFrame(document, 1).pixels, 0, 0) == 0xffffffffU,
               "vector keyframes must render on their exact hold-sampling boundary");
    }

    {
        Document document = makeDocument(5, 5);
        VectorPath path;
        path.commands = {
            MoveTo{{0.0, 2.0}},
            QuadraticTo{{2.0, 0.0}, {4.0, 2.0}},
            CubicTo{{4.0, 3.0}, {3.0, 4.0}, {2.0, 4.0}},
        };
        path.stroke = StrokeStyle{SolidPaint{0xffffffffU}, 1.0};
        document.assets.emplace_back(VectorAsset{"curves", {5, 5}, {path}});
        document.layers.push_back(staticVectorLayer("curve-layer", "curves"));

        const FrameRenderResult result = renderFrame(document, 0);
        expect(result.ok() && ((pixelAt(result.pixels, 2, 1) >> 24U) & 0xffU) > 0,
               "quadratic and cubic vector commands must produce visible deterministic strokes");
    }

    {
        Document invalid = makeDocument(1, 1);
        const FrameRenderResult invalidResult = renderFrame(invalid, 0);
        expect(invalidResult.status == FrameRenderStatus::Success,
               "an empty but valid document must render as transparent pixels");
        expect(pixelAt(invalidResult.pixels, 0, 0) == 0x00000000U,
               "an empty document must have a transparent frame");

        invalid.extent.width = 0;
        expect(renderFrame(invalid, 0).status == FrameRenderStatus::InvalidDocument,
               "rendering must fail closed when document validation fails");

        Document outOfRange = makeDocument(1, 1);
        expect(renderFrame(outOfRange, 1).status == FrameRenderStatus::FrameOutOfRange,
               "rendering outside the timeline must fail closed");
    }

    return failures == 0 ? 0 : 1;
}
