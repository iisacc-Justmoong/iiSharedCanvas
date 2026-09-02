#include <iiSharedCanvas.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

iiSharedCanvas::Document makeLargeDocument()
{
    using namespace iiSharedCanvas;

    Document document;
    document.canvasMode = CanvasMode::Infinite;
    document.infiniteCanvas = {{-32768, -16384}, 256};
    document.extent = {65536, 49152};
    document.timeline = {{24, 1}, 1};

    RasterChunk chunk;
    chunk.column = 63;
    chunk.row = 31;
    chunk.pixels = makeRasterLayer(256, 256, 0x00000000U);
    chunk.pixels.pixels[static_cast<std::size_t>(18) * 256U + 22U] = 0xff22d3eeU;
    document.assets.emplace_back(ChunkedRasterAsset{"paint", {std::move(chunk)}});
    document.layers.emplace_back(BitmapLayer{
        {"paint-layer", "Paint", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"paint"},
    });
    return document;
}

iiSharedCanvas::Document makeLargeVectorDocument()
{
    using namespace iiSharedCanvas;

    Document document;
    document.extent = {65536, 49152};
    document.timeline = {{24, 1}, 1};
    VectorPath path;
    path.commands = {
        MoveTo{{16128.0, 7936.0}},
        LineTo{{16640.0, 7936.0}},
        LineTo{{16640.0, 8448.0}},
        LineTo{{16128.0, 8448.0}},
        ClosePath{},
    };
    path.fill = SolidPaint{0xffffcc00U};
    document.assets.emplace_back(
        VectorAsset{"shape", {65536, 49152}, {std::move(path)}});
    document.layers.emplace_back(VectorLayer{
        {"shape-layer", "Shape", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"shape"},
    });
    return document;
}

iiSharedCanvas::Document makeTwoLayerDocument()
{
    using namespace iiSharedCanvas;

    Document document;
    document.extent = {128, 128};
    document.timeline = {{24, 1}, 1};
    document.assets.emplace_back(
        RasterAsset{"background", makeRasterLayer(128, 128, 0xff102030U)});

    VectorPath path;
    path.commands = {
        MoveTo{{32.0, 32.0}},
        LineTo{{96.0, 32.0}},
        LineTo{{96.0, 96.0}},
        LineTo{{32.0, 96.0}},
        ClosePath{},
    };
    path.fill = SolidPaint{0xffffcc00U};
    document.assets.emplace_back(
        VectorAsset{"shape", {128, 128}, {std::move(path)}});
    document.layers.emplace_back(BitmapLayer{
        {"background-layer", "Background", true, 1.0, {},
         RasterBlendMode::SourceOver},
        StaticSource{"background"},
    });
    document.layers.emplace_back(VectorLayer{
        {"shape-layer", "Shape", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"shape"},
    });
    return document;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    using namespace iiSharedCanvas;

    Document document = makeLargeDocument();
    const CanvasRegion sourceRegion{{16128, 7936}, {512, 512}};
    const FrameRenderTileRequest tileRequest{sourceRegion, {128, 128}};

    const FrameTileRenderResult immediate = renderFrameTiles(document, 0, {tileRequest});
    expect(immediate.ok() && immediate.tiles.size() == 1,
           "a bounded large-canvas tile request must render without allocating the full frame");
    expect(immediate.tiles.front().pixels.width == 128
               && immediate.tiles.front().pixels.height == 128
               && immediate.tiles.front().pixels.pixels.size() == 128U * 128U,
           "LOD tile output must be bounded by its requested texture extent");
    expect(rasterLayerPixelAt(immediate.tiles.front().pixels, {5, 4}) == 0xff22d3eeU,
           "LOD sampling must preserve the addressed sparse world pixel");
    expect(renderFrameRegion(document, 0, {{32760, 32760}, {32, 32}}, {32, 32}).status
               == FrameRenderStatus::InvalidRegion,
           "a region outside the allocated world bounds must fail closed");

    const FrameTileRenderResult vectorTile = renderFrameTiles(
        makeLargeVectorDocument(), 0, {tileRequest});
    expect(vectorTile.ok()
               && vectorTile.tiles.size() == 1
               && vectorTile.tiles.front().pixels.pixels.size() == 128U * 128U
               && rasterLayerPixelAt(vectorTile.tiles.front().pixels, {64, 64})
                   == 0xffffcc00U,
           "large native vector viewports must rasterize directly into the bounded LOD tile");

    AsyncFrameRenderer renderer;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(5000);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    qulonglong completedRequest = 0;
    int completionCount = 0;
    QObject::connect(&renderer, &AsyncFrameRenderer::finished,
                     &loop, [&](qulonglong requestId) {
                         completedRequest = requestId;
                         ++completionCount;
                         loop.quit();
                     });
    const qulonglong requestId = renderer.request(document, 0, {tileRequest});
    expect(requestId != 0 && renderer.busy(),
           "an asynchronous render request must return immediately in a busy state");

    std::get<ChunkedRasterAsset>(document.assets.front()).chunks.front().pixels.pixels.clear();
    timeout.start();
    loop.exec();

    const FrameTileRenderResult &completed = renderer.lastResult();
    expect(completedRequest == requestId && !renderer.busy() && completed.ok(),
           "the asynchronous renderer must finish the accepted request on its owner thread");
    expect(completed.tiles.size() == 1
               && rasterLayerPixelAt(completed.tiles.front().pixels, {5, 4}) == 0xff22d3eeU,
           "the worker must render an immutable request snapshot, not concurrently read caller mutations");
    expect(renderer.lastLayerResult().ok()
               && renderer.lastLayerResult().layers.size() == 1
               && renderer.lastLayerResult().layers.front().layerId == "paint-layer",
           "the asynchronous result must retain its independently rendered layer batch");

    Document twoLayers = makeTwoLayerDocument();
    const FrameRenderTileRequest fullTwoLayerTile{
        {{0, 0}, {128, 128}}, {128, 128}};
    completedRequest = 0;
    const qulonglong twoLayerRequest = renderer.request(
        twoLayers, 0, {fullTwoLayerTile});
    timeout.start();
    loop.exec();
    const FrameLayerBatchRenderResult &parallelLayers = renderer.lastLayerResult();
    expect(completedRequest == twoLayerRequest
               && parallelLayers.ok()
               && parallelLayers.layers.size() == 2
               && parallelLayers.layers[0].layerId == "background-layer"
               && parallelLayers.layers[1].layerId == "shape-layer",
           "parallel asynchronous work must return separate layers in document order");
    expect(rasterLayerPixelAt(parallelLayers.layers[0].tiles.front().pixels,
                              {64, 64}) == 0xff102030U
               && rasterLayerPixelAt(parallelLayers.layers[1].tiles.front().pixels,
                                     {0, 0}) == 0x00000000U
               && rasterLayerPixelAt(parallelLayers.layers[1].tiles.front().pixels,
                                     {64, 64}) == 0xffffcc00U
               && rasterLayerPixelAt(renderer.lastResult().tiles.front().pixels,
                                     {64, 64}) == 0xffffcc00U,
           "each asynchronous layer tile must stay isolated before final tile composition");

    document = makeLargeDocument();
    completedRequest = 0;
    const int completionsBeforeCoalescing = completionCount;
    renderer.request(document, 0, {tileRequest});
    const FrameRenderTileRequest latestTile{
        {{-32768, -16384}, {512, 512}}, {128, 128}};
    const qulonglong latestRequestId = renderer.request(
        document, 0, {latestTile});
    timeout.start();
    loop.exec();
    expect(completedRequest == latestRequestId
               && completionCount == completionsBeforeCoalescing + 1
               && renderer.lastResult().tiles.size() == 1
               && renderer.lastResult().tiles.front().region.origin.x == -32768,
           "queued asynchronous work must coalesce to one completion for the newest request");

    return failures == 0 ? 0 : 1;
}
