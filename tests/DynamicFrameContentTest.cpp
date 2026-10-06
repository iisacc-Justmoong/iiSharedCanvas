#include <iiSharedCanvas.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#include <filesystem>
#include <limits>
#include <memory>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
constexpr FrameIndex frameCount = 24;
void expect(bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
std::uint32_t color(FrameIndex frame) {
    return 0xff000000U | ((frame + 1) << 16) | ((frame * 7 + 3) << 8) | (frame * 9 + 1);
}
VectorAsset vectorContent(std::string id, FrameIndex frame) {
    const double width = 1 + frame % 4;
    VectorPath path;
    path.commands = {MoveTo{{0, 0}}, LineTo{{width, 0}}, LineTo{{width, 4}}, LineTo{{0, 4}}, ClosePath{}};
    path.fill = SolidPaint{color(frame)};
    return {std::move(id), {4, 4}, {std::move(path)}};
}
Document fixture() {
    Document d;
    d.extent = {8, 4};
    d.timeline = {{24, 1}, frameCount};
    DocumentEditor editor(d);
    expect(editor.insertRasterAsset("seed-bitmap", makeRasterLayer(4, 4, color(0))).ok(), "bitmap seed");
    const auto vector = vectorContent("seed-vector", 0);
    expect(editor.insertVectorAsset(vector.id, vector.viewport, vector.paths).ok(), "vector seed");
    expect(editor.insertDynamicLayer({"bitmap", "Per-frame bitmap"}, LayerRepresentation::Bitmap,
                                     {{0, "seed-bitmap"}}).ok(), "dynamic bitmap seed");
    LayerProperties properties; properties.id = "vector"; properties.transform.translationX = 4;
    expect(editor.insertDynamicLayer(properties, LayerRepresentation::Vector,
                                     {{0, "seed-vector"}}).ok(), "dynamic vector seed");
    return d;
}
void verifyFrames(const Document &d) {
    expect(validate(d).ok(), "every-frame content document validates");
    const auto *bitmap = findLayer(d, "bitmap"), *vector = findLayer(d, "vector");
    if (!bitmap || !vector) { expect(false, "dynamic layers exist"); return; }
    expect(layerKind(*bitmap) == LayerKind::DynamicBitmap && layerKind(*vector) == LayerKind::DynamicVector,
           "dynamic content keeps its bitmap/vector identity");
    for (FrameIndex frame = 0; frame < frameCount; ++frame) {
        const auto bitmapId = "bitmap-" + std::to_string(frame);
        const auto vectorId = "vector-" + std::to_string(frame);
        const auto *b = resolveAssetAt(d, *bitmap, frame), *v = resolveAssetAt(d, *vector, frame);
        expect(b && assetId(*b) == bitmapId && v && assetId(*v) == vectorId,
               "every integer frame resolves its own bitmap and vector state");
        const auto *geometry = v ? std::get_if<VectorAsset>(v) : nullptr;
        expect(geometry && std::get<LineTo>(geometry->paths[0].commands[1]).point.x == 1 + frame % 4,
               "vector geometry changes per frame, not only its transform");
        const auto rendered = renderFrame(d, frame);
        expect(rendered.ok() && rendered.pixels.width == 8 && rendered.pixels.height == 4,
               "render complete per-frame content");
        if (!rendered.ok()) continue;
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x) {
            const auto expected = x < 4 || x - 4 < 1 + int(frame % 4) ? color(frame) : 0;
            expect(rasterLayerPixelAt(rendered.pixels, {x, y}) == expected,
                   "all rendered pixels match each frame's independent bitmap and vector values");
        }
    }
    expect(!resolveAssetAt(d, *bitmap, frameCount), "no content beyond the timeline");
}
void fillFrames(DocumentEditor &editor) {
    for (FrameIndex frame = 0; frame < frameCount; ++frame) {
        const auto revision = editor.revision();
        expect(editor.setDynamicFrameContent("bitmap", frame,
            RasterAsset{"bitmap-" + std::to_string(frame), makeRasterLayer(4, 4, color(frame))}).changed,
            "atomically write bitmap content and insert or replace its exact-frame key");
        expect(editor.revision() == revision + 1, "bitmap content is one editor change");
        expect(editor.setDynamicFrameContent("vector", frame,
            vectorContent("vector-" + std::to_string(frame), frame)).changed,
            "atomically write vector geometry and its exact-frame key");
        expect(editor.revision() == revision + 2, "vector content is one editor change");
    }
}
void rejectedEdits(DocumentEditor &editor, const Document &d) {
    const auto before = encodeIisc(d);
    const auto revision = editor.revision();
    expect(editor.setDynamicFrameContent("missing", 2, RasterAsset{"missing", makeRasterLayer(1, 1)}).code
           == DocumentEditCode::LayerNotFound, "missing layer is explicit");
    expect(editor.setDynamicFrameContent("vector", 2, RasterAsset{"wrong-kind", makeRasterLayer(1, 1)}).code
           == DocumentEditCode::AssetKindMismatch, "bitmap payload cannot replace vector content");
    expect(editor.setDynamicFrameContent("bitmap", frameCount,
            RasterAsset{"out-of-range", makeRasterLayer(1, 1)}).code == DocumentEditCode::IndexOutOfRange,
           "out-of-range frame cannot leave an orphan asset");
    expect(editor.setDynamicFrameContent("bitmap", 3, RasterAsset{"bitmap-2", makeRasterLayer(1, 1)}).code
           == DocumentEditCode::DuplicateAssetId, "existing shared assets cannot be overwritten by a frame-local write");
    expect(!editor.setDynamicFrameContent("bitmap", 3, RasterAsset{"invalid", {}}).ok(),
           "malformed bitmap rolls back its asset and key");
    auto invalid = vectorContent("invalid-vector", 3);
    std::get<LineTo>(invalid.paths[0].commands[1]).point.x = std::numeric_limits<double>::quiet_NaN();
    expect(!editor.setDynamicFrameContent("vector", 3, std::move(invalid)).ok(),
           "malformed vector rolls back its asset and key");
    expect(editor.revision() == revision && encodeIisc(d).bytes == before.bytes,
           "all rejected content edits preserve complete bytes, metadata and revision");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    auto d = fixture();
    DocumentEditor editor(d);
    fillFrames(editor);
    verifyFrames(d);
    rejectedEdits(editor, d);
    expect(d.frames.size() == frameCount && d.frames[0].keyframes.size() == 2,
           "both dynamic tracks share frame owners without merging their content");
    const auto encoded = encodeIisc(d);
    const auto decoded = decodeIisc(encoded.bytes);
    expect(encoded.ok() && decoded.ok() && encodeIisc(decoded.document).bytes == encoded.bytes,
           "per-frame native snapshot preserves canonical content");
    if (decoded.ok()) verifyFrames(decoded.document);
    {
        AsyncFrameRenderer renderer;
        const auto snapshot = std::make_shared<const Document>(d);
        const FrameRenderTileRequest request{{{0, 0}, d.extent}, d.extent};
        for (FrameIndex offset = 0; offset < frameCount; ++offset) {
            const FrameIndex frame = frameCount - offset - 1;
            const auto id = renderer.request(snapshot, frame, {request});
            QElapsedTimer timeout; timeout.start();
            while (renderer.lastCompletedRequest() != id && timeout.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(1);
            }
            const auto &result = renderer.lastResult();
            expect(renderer.lastCompletedRequest() == id && result.ok() && result.tiles.size() == 1
                && result.tiles[0].pixels.pixels == renderFrame(d, frame).pixels.pixels,
                "asynchronous rendering selects each frame's content even when scrubbing backwards");
        }
    }
    {
        auto independent = d;
        BitmapEditor pixels(independent, "bitmap-9");
        expect(pixels.setPixel(0, 0, 0xffabcdefU), "edit one bitmap frame's pixels");
        DocumentEditor geometry(independent);
        auto replacement = vectorContent("unused", 7);
        expect(geometry.replaceVectorData("vector-9", replacement.viewport, replacement.paths).changed,
               "edit one vector frame's paths");
        for (FrameIndex frame = 0; frame < frameCount; ++frame) {
            if (frame == 9) continue;
            expect(renderFrame(independent, frame).pixels.pixels == renderFrame(d, frame).pixels.pixels,
                   "editing one frame leaves every other frame unchanged");
        }
        expect(renderFrame(independent, 9).pixels.pixels != renderFrame(d, 9).pixels.pixels,
               "the edited frame changes actual rendered content");
    }
    {
        auto held = fixture();
        DocumentEditor heldEditor(held);
        expect(heldEditor.setDynamicFrameContent("bitmap", 7, RasterAsset{"held-7", makeRasterLayer(4, 4, color(7))}).changed,
               "a sparse content key can be inserted");
        const auto *layer = findLayer(held, "bitmap");
        expect(assetId(*resolveAssetAt(held, *layer, 6)) == "seed-bitmap"
            && assetId(*resolveAssetAt(held, *layer, 7)) == "held-7"
            && assetId(*resolveAssetAt(held, *layer, 23)) == "held-7",
            "missing frame keys hold the prior content and switch on the exact key boundary");
        expect(heldEditor.setStaticSource("bitmap", "seed-bitmap").ok(), "static conversion fixture");
        const auto prior = encodeIisc(held).bytes;
        expect(heldEditor.setDynamicFrameContent("bitmap", 3, RasterAsset{"static-edit", makeRasterLayer(1, 1)}).code
            == DocumentEditCode::SourceNotKeyframed && encodeIisc(held).bytes == prior,
            "frame-local authoring never silently converts a static layer");
    }
    const auto path = std::string(IISHAREDCANVAS_TEST_OUTPUT_DIR "/dynamic-frame-content.iisc");
    std::filesystem::remove(path);
    DocumentFile file;
    expect(file.create(path, fixture()).ok(), "create per-frame working document");
    DocumentEditor bound(file);
    fillFrames(bound);
    if (file.document()) {
        verifyFrames(*file.document());
        rejectedEdits(bound, *file.document());
        expect(file.revision() == frameCount * 2, "each content write is one durable transaction");
        const auto prior = encodeIisc(*file.document()).bytes;
        expect(bound.setDynamicFrameContent("bitmap", 9, RasterAsset{"replacement-9", makeRasterLayer(4, 4, 0xffabcdefU)}).changed,
               "file-bound replacement creates a new independent frame state");
        const auto statistics = file.lastWriteStatistics();
        expect(statistics.payloadBytesWritten < prior.size(), "a frame replacement does not rewrite all frame assets");
        expect(bound.setKeyframeAsset("bitmap", 9, "bitmap-9").changed, "restore original exact-frame state");
    }
    file.close();
    DocumentFile reopened;
    expect(reopened.open(path).ok(), "reopen actual native per-frame file");
    if (reopened.document()) verifyFrames(*reopened.document());
    {
        auto limitedDocument = fixture();
        SerializationLimits limits; limits.maximumAssets = limitedDocument.assets.size();
        const auto limitedPath = std::string(IISHAREDCANVAS_TEST_OUTPUT_DIR "/dynamic-frame-limit.iisc");
        std::filesystem::remove(limitedPath);
        DocumentFile limited;
        expect(limited.create(limitedPath, limitedDocument, limits).ok(), "create asset-limited document");
        DocumentEditor limitedEditor(limited);
        const auto prior = encodeIisc(*limited.document()).bytes;
        expect(limitedEditor.setDynamicFrameContent("bitmap", 1,
            RasterAsset{"over-budget", makeRasterLayer(1, 1)}).code == DocumentEditCode::PersistenceFailed,
            "persistence-budget rejection is surfaced by the content editor");
        expect(limited.revision() == 0 && limitedEditor.revision() == 0
            && encodeIisc(*limited.document()).bytes == prior, "failed durable write leaves no asset or key");
        limited.close();
        expect(limited.open(limitedPath).ok() && encodeIisc(*limited.document()).bytes == prior,
               "reopening proves rejected content was never saved");
    }
    return failures ? 1 : 0;
}
