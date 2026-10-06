#include <iiSharedCanvas.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QThread>
#include <iostream>
#include <limits>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
Document fixture() {
    Document d;
    d.extent = {2, 2};
    d.timeline.frameCount = 2;
    d.artboards = {{"a", "대지 A", {{-2, 0}, {2, 2}}, 0xffffffffU},
                   {"b", "대지 B", {{3, 0}, {3, 2}}, 0xff000000U}};
    d.assets.emplace_back(RasterAsset{"red", makeRasterLayer(4, 3, 0xffff0000U)});
    d.assets.emplace_back(RasterAsset{"blue", makeRasterLayer(1, 1, 0xff0000ffU)});
    LayerProperties p; p.id = "layer-a"; p.artboardId = "a";
    p.transform.translationX = -1;
    d.layers.emplace_back(StaticBitmapLayer{p, StaticSource{"red"}});
    p = {}; p.id = "layer-b"; p.artboardId = "b";
    d.layers.emplace_back(DynamicBitmapLayer{p, KeyframedSource{{0, 1}}});
    d.frames = {{0, {{"layer-b", "red"}}}, {1, {{"layer-b", "blue"}}}};
    return d;
}
std::uint32_t pixel(const FrameRenderResult &r, int x, int y) {
    return rasterLayerPixelAt(r.pixels, {x, y});
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    Document d = fixture();
    expect(validate(d).ok(), "multiple independent artboards must validate");
    auto a = renderArtboard(d, 0, "a");
    expect(a.ok() && a.origin.x == -2 && a.pixels.width == 2 && pixel(a, 0, 0) == 0xffff0000U,
           "artboard render must use local content coordinates and its own extent");
    auto full = renderFrame(d, 1);
    expect(full.ok() && full.origin.x == -2 && full.pixels.width == 8
        && pixel(full, 2, 0) == 0 && pixel(full, 5, 0) == 0xff0000ffU
        && pixel(full, 6, 0) == 0xff000000U,
        "overview must include negative and external artboards, clip content and keep gaps transparent");
    expect(renderArtboard(d, 0, "missing").status == FrameRenderStatus::ArtboardNotFound,
           "missing artboard must be a typed rendering error");
    const auto scaled = renderArtboard(d,1,"b",{6,4},RasterSampling::Smooth);
    expect(scaled.ok() && scaled.pixels.width == 6 && pixel(scaled,0,0) == 0xff0000ffU,
           "single artboard views must support scaled smooth rendering");
    {
        const auto region = documentViewRegion(d);
        auto batch = renderFrameLayers(d,0,{{region,region.extent}});
        batch.requests[0].region.extent.width = 0;
        expect(composeFrameLayers(batch).status == FrameRenderStatus::InvalidRegion,
               "composition must reject invalid geometry before evaluating artboard coverage");
        batch.requests[0].region = region;
        batch.artboards[1].id = "a";
        expect(composeFrameLayers(batch).status == FrameRenderStatus::InvalidDocument,
               "composition must reject corrupted detached artboard metadata");
    }
    {
        auto overlap = fixture();
        overlap.artboards[1].region = {{-2,0},{2,2}};
        expect(pixel(renderFrame(overlap,1),0,0) == 0xff0000ffU
            && pixel(renderArtboard(overlap,1,"a"),0,0) == 0xffff0000U,
            "overlapping artboards must preserve group order and single-board isolation");
        DocumentEditor oe(overlap);
        expect(oe.setArtboardVisible("b",false).changed
            && pixel(renderFrame(overlap,1),0,0) == 0xffff0000U
            && pixel(renderArtboard(overlap,1,"b"),0,0) == 0,
            "hidden boards must suppress their background and all owned content");
        expect(oe.setArtboardRegion("a",{{-2,0},{1,1}}).changed
            && renderArtboard(overlap,0,"a").pixels.width == 1
            && findRasterAsset(overlap,"red")->pixels.width == 4,
            "resize must clip the view without resampling source pixels");
        LayerProperties loose; loose.id = "loose"; loose.transform.translationX = -2;
        expect(oe.insertLayer(StaticBitmapLayer{loose,StaticSource{"blue"}}).changed
            && pixel(renderFrame(overlap,0),0,0) == 0xff0000ffU
            && pixel(renderArtboard(overlap,0,"a"),0,0) == 0xffff0000U,
            "loose artwork must be unclipped above groups and excluded from single-board renders");
    }
    {
        auto moving = fixture();
        MotionKeyframe key; key.frame = 1; key.value.position = {1,0};
        layerProperties(moving.layers[0]).motion = {key};
        expect(sampleLayerAt(moving,moving.layers[0],1).transform.translationX == -2,
               "local motion must be evaluated before adding artboard origin");
        VectorPath path;
        path.commands = {MoveTo{{0,0}}, LineTo{{3,0}}, LineTo{{3,3}}, LineTo{{0,3}}, ClosePath{}};
        path.fill = SolidPaint{0xff00ff00U};
        moving.assets.emplace_back(VectorAsset{"shape",{3,3},{path}});
        LayerProperties p; p.id = "shape-layer"; p.artboardId = "a";
        moving.layers.emplace_back(StaticVectorLayer{p,StaticSource{"shape"}});
        const auto view = renderFrame(moving,0);
        expect(view.ok() && pixel(view,1,1) == 0xff00ff00U && pixel(view,2,1) == 0,
               "native vector layers must translate and clip to their artboard");
    }
    const auto before = encodeIisc(d);
    const auto decoded = decodeIisc(before.bytes);
    expect(before.ok() && decoded.ok() && decoded.document.artboards == d.artboards
        && layerProperties(decoded.document.layers[0]).artboardId == "a"
        && encodeIisc(decoded.document).bytes == before.bytes,
        "snapshot must preserve artboards, membership and canonical bytes");
    SerializationLimits limits; limits.maximumArtboards = 1;
    expect(encodeIisc(d, limits).error.code == IiscErrorCode::LimitExceeded
        && decodeIisc(before.bytes, limits).error.code == IiscErrorCode::LimitExceeded,
        "artboard counts must obey encode and decode limits");
    Document invalid = d; invalid.artboards[1].id = "a";
    expect(!validate(invalid).ok(), "duplicate artboard ids must fail");
    invalid = d; invalid.artboards[0].region.extent.width = 0;
    expect(!validate(invalid).ok(), "zero-sized artboards must fail");
    invalid = d; invalid.artboards[0].region.origin.x = std::numeric_limits<std::int32_t>::max();
    expect(!validate(invalid).ok(), "overflowing artboard bounds must fail");
    invalid = d; layerProperties(invalid.layers[0]).artboardId = "missing";
    expect(!validate(invalid).ok(), "dangling membership must fail");
    invalid = d; invalid.formatVersion.minor = 17;
    expect(!encodeIisc(invalid).ok(), "legacy versions must reject silent artboard loss");
    Document legacy; legacy.extent = {2, 2}; legacy.formatVersion = {1, 17};
    const auto legacyBytes = encodeIisc(legacy);
    const auto legacyRead = decodeIisc(legacyBytes.bytes);
    expect(legacyRead.ok() && legacyRead.document.artboards.empty()
        && encodeIisc(legacyRead.document).bytes == legacyBytes.bytes,
        "pre-artboard files must retain their canonical representation");

    DocumentEditor e(d);
    expect(e.setArtboardName("a", "Phone").changed && e.setArtboardBackground("b", 0).changed,
           "name and transparent background must be editable");
    const auto revision = e.revision();
    expect(!e.setArtboardName("a", "Phone").changed && e.revision() == revision,
           "equivalent edits must be no-ops");
    auto stable = encodeIisc(d).bytes;
    expect(!e.setArtboardRegion("a", {{0,0}, {0,2}}).ok()
        && e.revision() == revision && encodeIisc(d).bytes == stable,
        "invalid geometry must roll back model, version and revision");
    expect(e.setArtboardRegion("a", {{7,4}, {2,2}}).changed
        && pixel(renderArtboard(d,0,"a"),0,0) == 0xffff0000U,
        "moving artboard must move content without editing source pixels");
    expect(e.setLayerArtboard("layer-a", "b").changed
        && sampleLayerAt(d, d.layers[0], 0).transform.translationX == 6,
        "reparent must preserve world placement");
    expect(e.setLayerArtboard("layer-a", "a").changed,
           "reparent back must preserve placement");
    expect(e.duplicateArtboard("b", "copy", {10,0}).changed && d.assets.size() == 4
        && d.layers.size() == 3 && d.frames[0].keyframes.size() == 2,
        "duplicate must clone layer/keyframe/source identities for independent artwork");
    expect(pixel(renderArtboard(d,1,"copy"),0,0) == 0xff0000ffU,
           "duplicate must preserve dynamic content sampling");
    BitmapEditor copyEditor(d,"copy:asset:blue");
    expect(copyEditor.setPixel(0,0,0xff00ff00U)
        && pixel(renderArtboard(d,1,"b"),0,0) == 0xff0000ffU,
        "editing duplicated raster content must not mutate the original artboard");
    expect(e.moveArtboard("copy",0).changed && d.artboards.front().id == "copy",
           "artboard stacking must be reorderable");
    expect(e.removeArtboard("copy", ArtboardRemoval::DeleteLayers).changed
        && d.layers.size() == 2 && d.frames[0].keyframes.size() == 1,
        "delete-with-content must remove owned layers and keyframes without deleting shared assets");
    expect(e.removeArtboard("a").changed && !layerProperties(d.layers[0]).artboardId
        && sampleLayerAt(d,d.layers[0],0).transform.translationX == 6,
        "default deletion must retain layers at their world position");
    expect(e.insertArtboard({"new", "New", {{0,0},{4,4}}, 0xffffffffU}).changed,
           "new artboards must be insertable");
    DocumentEditor legacyEditor(legacy);
    expect(legacyEditor.insertArtboard({"a", "A", {{0,0},{2,2}}}).changed
        && legacy.formatVersion.minor == CurrentFormatMinor, "first artboard edit must upgrade legacy documents");

    const std::string directory = IISHAREDCANVAS_TEST_OUTPUT_DIR;
    QDir().mkpath(QString::fromStdString(directory));
    const std::string path = directory + "/artboards.iisc";
    QFile::remove(QString::fromStdString(path));
    DocumentFile file;
    SerializationLimits fileLimits; fileLimits.maximumStringBytes = 32;
    expect(file.create(path, fixture(), fileLimits).ok(), "working file must accept artboards");
    DocumentEditor fe(file);
    expect(fe.setArtboardName("a", "Saved").changed
        && file.lastWriteStatistics().payloadBytesWritten < 1024,
        "metadata editing must persist incrementally without rewriting assets");
    const auto fileRevision = file.revision();
    const auto fileBytes = encodeIisc(*file.document()).bytes;
    expect(!fe.setLayerArtboard("layer-a", "missing").ok()
        && file.revision() == fileRevision && encodeIisc(*file.document()).bytes == fileBytes,
        "rejected file-bound edit must leave the durable model unchanged");
    expect(fe.setArtboardName("a",std::string(33,'x')).code == DocumentEditCode::PersistenceFailed
        && file.revision() == fileRevision && encodeIisc(*file.document()).bytes == fileBytes,
        "persistence-limit failure must roll back accepted draft edits and authorship");
    DocumentFile reopened;
    expect(reopened.open(path).ok() && findArtboard(*reopened.document(),"a")->name == "Saved"
        && pixel(renderArtboard(*reopened.document(),1,"b"),0,0) == 0xff0000ffU,
        "reopen must preserve artboard properties, membership and rendered output");

    AsyncFrameRenderer async;
    Document source = fixture();
    auto view = documentViewRegion(source);
    const auto request = async.request(source,1,{{view,view.extent}});
    QElapsedTimer timer; timer.start();
    while (async.lastCompletedRequest() != request && timer.elapsed() < 10000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    const auto result = async.takeResult();
    expect(async.lastCompletedRequest() == request && result.ok()
        && result.tiles.size() == 1 && result.tiles[0].pixels.pixels == full.pixels.pixels,
        "parallel async renderer must preserve group backgrounds and clipping");
    expect(encodePsd(source).result.code == MediaIoCode::UnsupportedFeature,
           "foreign layered export must reject unsupported artboard semantics explicitly");
    const auto pdf = directory + "/artboards.pdf";
    QFile::remove(QString::fromStdString(pdf));
    expect(exportPdf(source,pdf).code == MediaIoCode::UnsupportedFeature,
           "PDF export must reject unsupported artboard semantics explicitly");
    expect(exportTimelineInterchange(source,directory + "/artboard-timeline").code == MediaIoCode::UnsupportedFeature,
           "layered timeline export must reject unsupported artboard semantics explicitly");
    BitmapExportOptions bitmap; bitmap.overwrite = true;
    expect(exportBitmapFrame(source,1,directory + "/artboard-overview.png",bitmap).ok(),
           "bitmap overview export must use the complete workspace dimensions");
    return failures ? 1 : 0;
}
