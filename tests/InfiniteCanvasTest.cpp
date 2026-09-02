#include <iiSharedCanvas.h>

#include <QGuiApplication>

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

iiSharedCanvas::Document makeInfiniteDocument()
{
    using namespace iiSharedCanvas;

    Document document;
    document.canvasMode = CanvasMode::Infinite;
    document.infiniteCanvas = {{0, 0}, 64};
    document.extent = {128, 96};
    document.timeline = {{24, 1}, 1};
    document.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    document.layers.emplace_back(BitmapLayer{
        {"paint-layer", "Paint", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"paint"},
    });
    return document;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);

    using namespace iiSharedCanvas;

    Document document = makeInfiniteDocument();
    expect(validate(document).ok(),
           "an infinite document with one sparse raster layer must validate");
    expect(contentKind(document.assets.front()) == ContentKind::Raster
               && findChunkedRasterAsset(document, "paint") != nullptr,
           "chunked raster content must participate in the normal asset lookup contract");

    Document invalidChunkSize = document;
    invalidChunkSize.infiniteCanvas.chunkSize = 48;
    expect(!validate(invalidChunkSize).ok(),
           "an infinite canvas must reject non-power-of-two chunk sizes");
    Document finiteChunked = document;
    finiteChunked.canvasMode = CanvasMode::Finite;
    expect(!validate(finiteChunked).ok(),
           "a finite canvas must reject chunked raster assets");

    DocumentEditor structure(document);
    const DocumentEditResult expanded = structure.ensureInfiniteCanvasRegion(
        {{-70, -10}, {300, 180}});
    expect(expanded.ok() && expanded.changed
               && document.infiniteCanvas.origin.x == -128
               && document.infiniteCanvas.origin.y == -64
               && document.extent.width == 384
               && document.extent.height == 256,
           "camera demand must expand the allocated region outwards on chunk boundaries");
    const std::uint64_t expandedRevision = structure.revision();
    const DocumentEditResult contained = structure.ensureInfiniteCanvasRegion(
        {{-64, 0}, {64, 64}});
    expect(contained.ok() && !contained.changed && structure.revision() == expandedRevision,
           "a viewport already covered by allocated chunks must not mutate the document");

    ChunkedBitmapEditor editor(document, "paint");
    BitmapBrush brush;
    brush.argb = 0xff22d3eeU;
    brush.size = 1.0;
    brush.hardness = 1.0;
    expect(editor.setBrush(brush)
               && editor.beginStroke({-1.0, -1.0}, 1.0)
               && editor.endStroke({-1.0, -1.0}, 1.0),
           "a brush stroke must author sparse pixels in negative world coordinates");
    const ChunkedRasterAsset *paint = findChunkedRasterAsset(document, "paint");
    const RasterChunk *negativeChunk = paint ? findRasterChunk(*paint, -1, -1) : nullptr;
    expect(negativeChunk
               && rasterLayerPixelAt(negativeChunk->pixels, {63, 63}) == 0xff22d3eeU,
           "the brush result must be stored in the exact addressed chunk rather than a monolithic bitmap");
    Document duplicateChunk = document;
    if (negativeChunk) {
        std::get<ChunkedRasterAsset>(duplicateChunk.assets.front()).chunks.push_back(
            *negativeChunk);
    }
    expect(negativeChunk && !validate(duplicateChunk).ok(),
           "duplicate or non-canonical sparse chunk coordinates must fail validation");

    const FrameRenderResult rendered = renderFrame(document, 0);
    expect(rendered.ok()
               && rendered.origin.x == -128
               && rendered.origin.y == -64
               && rasterLayerPixelAt(rendered.pixels, {127, 63}) == 0xff22d3eeU,
           "frame rendering must place sparse chunks relative to the allocated world origin");
    expect(editor.canUndo() && editor.undo()
               && rasterLayerPixelAt(renderFrame(document, 0).pixels, {127, 63}) == 0x00000000U
               && editor.redo(),
           "one chunked stroke must undo and redo atomically across the sparse asset");

    const IiscEncodeResult encoded = encodeIisc(document);
    const IiscDecodeResult decoded = encoded.ok() ? decodeIisc(encoded.bytes) : IiscDecodeResult{};
    const ChunkedRasterAsset *decodedPaint = decoded.ok()
        ? findChunkedRasterAsset(decoded.document, "paint")
        : nullptr;
    expect(encoded.ok() && decoded.ok()
               && decoded.document.canvasMode == CanvasMode::Infinite
               && decoded.document.infiniteCanvas.origin.x == -128
               && decoded.document.infiniteCanvas.chunkSize == 64
               && decodedPaint
               && findRasterChunk(*decodedPaint, -1, -1)
               && encodeIisc(decoded.document).bytes == encoded.bytes,
           "the 1.1 codec must preserve infinite geometry and canonical sparse chunks");

    Document legacy;
    legacy.formatVersion = {1, 0};
    legacy.extent = {2, 2};
    legacy.timeline = {{24, 1}, 1};
    const IiscEncodeResult legacyBytes = encodeIisc(legacy);
    const IiscDecodeResult legacyDecoded = legacyBytes.ok()
        ? decodeIisc(legacyBytes.bytes)
        : IiscDecodeResult{};
    expect(legacyDecoded.ok()
               && legacyDecoded.document.canvasMode == CanvasMode::Finite
               && legacyDecoded.document.infiniteCanvas.origin.x == 0,
           "the 1.1 reader must migrate a canonical 1.0 document to finite canvas defaults");

    CanvasItem item;
    expect(item.createInfiniteRasterDocument(96, 64, 32)
               && item.infiniteCanvas()
               && item.canvasOriginX() == 0
               && item.canvasOriginY() == 0
               && item.canvasChunkSize() == 32,
           "Qt Quick hosts must be able to create an editable infinite raster document");
    const QVariantMap growth = item.ensureInfiniteCanvasRegion(-40, -1, 200, 90);
    expect(growth.value(QStringLiteral("changed")).toBool()
               && growth.value(QStringLiteral("left")).toInt() == 64
               && growth.value(QStringLiteral("top")).toInt() == 32
               && growth.value(QStringLiteral("right")).toInt() == 64
               && item.canvasWidth() == 224
               && item.canvasHeight() == 128,
           "CanvasItem must report exact margins so a consumer can preserve camera and overlay positions");

    return failures == 0 ? 0 : 1;
}
