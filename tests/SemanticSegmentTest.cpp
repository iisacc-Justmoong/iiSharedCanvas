#include <iiSharedCanvas.h>
#include <QTemporaryDir>
#include <algorithm>
#include <iostream>
#include <limits>

namespace {
int failures = 0;
void expect(bool ok, const char *message) { if (!ok) { std::cerr << message << '\n'; ++failures; } }
using namespace iiSharedCanvas;
SemanticSegmentation definition()
{
    SemanticSegmentation value;
    value.taxonomy.id = "scene"; value.taxonomy.version = "1";
    value.taxonomy.sourceUri = "urn:test:scene";
    SemanticClass person; person.id = 1; person.key = "person"; person.name = "Person";
    person.description = "A human figure"; person.category = "foreground";
    person.controlColor = 0xffaabbcc; person.externalId = "dataset:person";
    person.aliases = {"human", "pedestrian"};
    SemanticClass face; face.id = 2; face.key = "face"; face.name = "Face";
    face.parentId = 1; face.controlColor = 0xff123456;
    value.taxonomy.classes = {person, face};
    SemanticRegion left; left.id = 10; left.classId = 1; left.maskColor = 0xffff0000;
    left.name = "Left person"; left.description = "Person on the left";
    left.instanceId = 101; left.confidence = 0.95; left.origin = SemanticOrigin::Model;
    left.generator = "segmenter@1"; left.sourceReference = "urn:input:frame";
    left.attributes = {{"clothing", "coat"}, {"pose", "standing"}};
    auto right = left; right.id = 20; right.maskColor = 0xff00ff00; right.instanceId = 102;
    right.name = "Right person";
    value.regions = {left, right};
    return value;
}
Document fixture()
{
    Document doc; doc.extent = {2,2}; doc.timeline.frameCount = 2;
    auto mask = makeRasterLayer(2,2); mask.pixels = {0xffff0000,0xff00ff00,0xffff0000,0xff000000};
    auto next = mask; next.pixels[0] = 0xff00ff00;
    doc.assets = {RasterAsset{"mask",mask}, RasterAsset{"next",next}};
    DocumentEditor editor(doc);
    SemanticSegmentLayer layer; layer.properties = {"segments", "Semantic segments"};
    layer.source = StaticSource{"mask"}; layer.segmentation = definition();
    layer.control.modelId = "test/seg"; layer.control.modelRevision = "revision";
    layer.control.conditioningScale = 0.8; layer.control.guidanceStart = 0.1; layer.control.guidanceEnd = 0.9;
    expect(editor.insertSemanticSegmentLayer(layer).changed, "insert dedicated semantic layer");
    return doc;
}
void verify(const Document &doc)
{
    const auto *layer = findSemanticSegmentLayer(doc, "segments");
    expect(layer != nullptr, "dedicated layer type survives"); if (!layer) { return; }
    expect(layer->segmentation == definition(), "all semantic fields survive");
    expect(layerRole(doc.layers[0]) == LayerRole::ControlNet && controlNetKind(doc.layers[0]) == ControlNetKind::SemanticSegment,
           "control hierarchy is separate from bitmap representation");
    const auto map = renderSemanticControlMap(doc,"segments",0);
    expect(map.ok(), "render exact control map"); if (!map.ok()) { std::cerr << map.message << '\n'; return; }
    expect(map.pixels.pixels == std::vector<std::uint32_t>({0xffaabbcc,0xffaabbcc,0xffaabbcc,0xff000000}), "class palette output has exact colors");
    expect(map.classIds == std::vector<std::uint32_t>({1,1,1,0}) && map.regionIds == std::vector<std::uint32_t>({10,20,10,0})
        && map.validPixels == std::vector<std::uint8_t>({1,1,1,0}), "class/region labels preserve instance distinction and void");
    expect(map.regions.size() == 2 && map.regions[0].pixelCount == 2 && map.regions[0].bounds.width == 1
        && map.regions[0].bounds.height == 2 && map.regions[0].centroid.x == 0.5 && map.regions[0].centroid.y == 1,
        "derive exact region area, bounds and pixel-center centroid");
    const auto artwork = renderFrame(doc,0);
    expect(artwork.ok() && artwork.pixels.pixels == std::vector<std::uint32_t>(4,0), "control layers never contaminate artwork");
    const auto preview = renderFrameLayerTiles(doc,0,0,{{canvasRegion(doc),doc.extent}});
    expect(preview.ok() && preview.role == LayerRole::ControlNet && !preview.tiles.empty(), "explicit layer preview remains available");
}
}
int main()
{
    auto doc = fixture(); verify(doc);
    BitmapEditor brush;
    expect(!brush.bind(doc,"mask"), "soft bitmap brushes cannot corrupt categorical masks");
    auto lateBound = doc;
    const auto semanticLayer = lateBound.layers.front(); lateBound.layers.clear();
    expect(brush.bind(lateBound,"mask"), "ordinary raster can bind before semantic ownership");
    lateBound.layers.push_back(semanticLayer);
    expect(!brush.isBound() && !brush.clear(0xff112233), "existing brush binding cannot bypass semantic ownership");
    auto upgraded = doc; upgraded.layers.clear(); upgraded.formatVersion.minor = 6;
    DocumentEditor upgradeEditor(upgraded);
    auto dynamicLayer = *findSemanticSegmentLayer(doc,"segments"); dynamicLayer.source = KeyframedSource{};
    expect(upgradeEditor.insertSemanticSegmentLayer(dynamicLayer, {{0,"mask"},{1,"next"}}).changed
        && upgraded.formatVersion.minor == CurrentFormatMinor, "dynamic semantic creation upgrades legacy documents atomically");
    expect(renderSemanticControlMap(upgraded,"segments",1).regionIds[0] == 20, "dynamic creation populates frame content");
    auto zeroClass = doc;
    auto *zeroLayer = findSemanticSegmentLayer(zeroClass,"segments");
    zeroLayer->segmentation.taxonomy.classes[0].id = 0;
    zeroLayer->segmentation.taxonomy.classes[1].parentId = 0;
    for (auto &region : zeroLayer->segmentation.regions) { region.classId = 0; }
    const auto zeroMap = renderSemanticControlMap(zeroClass,"segments",0);
    expect(zeroMap.ok() && zeroMap.classIds[0] == 0 && zeroMap.validPixels[0] == 1 && zeroMap.validPixels[3] == 0,
           "class zero remains distinguishable from void");
    expect(!renderSemanticControlMap(doc,"segments",0,3).ok(), "dense output allocation obeys pixel budget");
    DocumentEditor editor(doc); const auto original = encodeIisc(doc); const auto revision = editor.revision();
    const auto rejectDefinition = [&](SemanticSegmentation value) {
        expect(!editor.setSemanticSegmentation("segments",std::move(value)).ok(), "reject invalid semantic definition");
        expect(editor.revision() == revision && encodeIisc(doc).bytes == original.bytes, "rejection is atomic");
    };
    auto bad = definition(); bad.regions[1].maskColor = bad.regions[0].maskColor; rejectDefinition(bad);
    bad = definition(); bad.regions[0].classId = 99; rejectDefinition(bad);
    bad = definition(); bad.taxonomy.classes[0].parentId = 2; rejectDefinition(bad);
    bad = definition(); bad.regions[0].confidence = std::numeric_limits<double>::quiet_NaN(); rejectDefinition(bad);
    bad = definition(); bad.regions[0].maskColor = 0x00ff0000; rejectDefinition(bad);
    bad = definition(); bad.regions.erase(bad.regions.begin()); rejectDefinition(bad);
    bad = definition(); bad.regions[0].attributes.push_back({"pose","sitting"}); rejectDefinition(bad);
    auto pixels = makeRasterLayer(2,2,0xffabcdef);
    expect(!editor.replaceRasterPixels("mask",pixels).ok() && editor.revision() == revision, "unknown mask colors cannot enter an existing semantic layer");
    ControlNetSettings invalid; invalid.guidanceStart = 0.9; invalid.guidanceEnd = 0.1;
    expect(!editor.setControlNetSettings("segments",invalid).ok(), "reject invalid control interval");
    expect(original.ok(), "encode semantic snapshot");
    // Independently locate the layer extension through its length-prefixed stable id.
    auto corrupt = original.bytes;
    const std::vector<std::uint8_t> marker{8,0,0,0,'s','e','g','m','e','n','t','s'};
    const auto located = std::search(corrupt.begin(),corrupt.end(),marker.begin(),marker.end());
    expect(located != corrupt.end(), "layer record is present");
    if (located != corrupt.end()) {
        auto cursor = static_cast<std::size_t>(located-corrupt.begin());
        const auto skipString = [&] {
            std::uint32_t size = 0;
            for (unsigned i = 0; i < 4; ++i) { size |= std::uint32_t(corrupt.at(cursor+i)) << (8*i); }
            cursor += 4 + size;
        };
        skipString(); skipString(); cursor += 1+8+48+1;
        if (doc.formatVersion.minor >= 18) {
            if (corrupt.at(cursor++)) skipString(); // Optional artboard membership.
        }
        if (doc.formatVersion.minor >= 19) ++cursor; // Explicit visual type.
        ++cursor; skipString(); ++cursor;
        expect(corrupt.at(cursor) == 1, "semantic role is serialized as tag one");
        corrupt.at(cursor) = 255;
        std::uint32_t crc = 0xffffffffU;
        for (std::size_t i = IiscHeaderSize; i < corrupt.size(); ++i) {
            crc ^= corrupt[i];
            for (int bit = 0; bit < 8; ++bit) { crc = (crc >> 1U) ^ (0xedb88320U & (0U-(crc & 1U))); }
        }
        crc ^= 0xffffffffU;
        for (unsigned i = 0; i < 4; ++i) { corrupt[24+i] = static_cast<std::uint8_t>(crc >> (8*i)); }
        expect(decodeIisc(corrupt).error.code == IiscErrorCode::InvalidData, "unknown layer role fails closed even with a valid checksum");
    }
    auto decoded = decodeIisc(original.bytes); expect(decoded.ok(), "decode semantic snapshot");
    if (decoded.ok()) { verify(decoded.document); expect(findSemanticSegmentLayer(decoded.document,"segments")->control == findSemanticSegmentLayer(doc,"segments")->control, "all control settings survive"); }
    auto legacy = doc; legacy.formatVersion.minor = 6;
    expect(!encodeIisc(legacy).ok(), "legacy format cannot silently lose semantic fields");
    SerializationLimits limits; limits.maximumSemanticRegions = 1;
    expect(!encodeIisc(doc,limits).ok() && !decodeIisc(original.bytes,limits).ok(), "semantic limits enforced on encode/decode");
    expect(editor.setKeyframedSource("segments",{{0,"mask"},{1,"next"}}).changed, "dynamic semantic source");
    expect(layerKind(doc.layers[0]) == LayerKind::DynamicBitmap, "dynamic semantic layer respects four-way identity");
    auto map = renderSemanticControlMap(doc,"segments",1);
    expect(map.ok() && map.regionIds[0] == 20, "semantic content follows current frame");
    expect(!renderSemanticControlMap(doc,"segments",2).ok(), "out of timeline rejected");
    auto settings = findSemanticSegmentLayer(doc,"segments")->control; settings.enabled = false;
    expect(editor.setControlNetSettings("segments",settings).changed && !renderSemanticControlMap(doc,"segments",0).ok(), "disabled control layer cannot export conditioning");
    settings.enabled = true; expect(editor.setControlNetSettings("segments",settings).changed, "reenable control");
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/semantic-XXXXXX"));
    expect(directory.isValid(), "temporary working directory");
    if (directory.isValid()) {
        DocumentFile file; const auto path = directory.filePath("working.iisc").toStdString();
        expect(file.create(path,doc).ok(), "persist semantic working file");
        DocumentEditor bound(file); auto updated = definition(); updated.regions[0].name = "Renamed";
        expect(bound.setSemanticSegmentation("segments",updated).changed, "persist semantic-only edit");
        expect(file.lastWriteStatistics().recordsWritten <= 2, "semantic-only changes reuse unchanged mask records");
        DocumentFile reopened; expect(reopened.open(path).ok(), "reopen semantic working file");
        if (reopened.document()) {
            const auto *restored = findSemanticSegmentLayer(*reopened.document(),"segments");
            expect(restored && restored->segmentation == updated, "working file preserves semantic-only edits");
            expect(renderSemanticControlMap(*reopened.document(),"segments",1).regionIds == map.regionIds, "working file preserves dynamic labels");
        }
        expect(exportPsd(doc,directory.filePath("semantic.psd").toStdString()).code == MediaIoCode::UnsupportedFeature,
               "foreign layered export rejects loss of semantics");
        expect(exportTimelineInterchange(doc,directory.filePath("timeline").toStdString()).code == MediaIoCode::UnsupportedFeature,
               "timeline interchange rejects loss of semantics");
    }
    return failures ? 1 : 0;
}
