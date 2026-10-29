#include <Bitmap/BitmapEditor.h>
#include <Brush/BrushResolve.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderNative.h>
#include "LegacyWorkingFileFixture_p.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
Document fixture() {
    Document document;
    document.extent = {128, 128};
    document.canvasMode = CanvasMode::Infinite;
    document.infiniteCanvas = {{-64, -64}, 32};
    auto hidden = makeRasterLayer(32, 32);
    hidden.pixels[3] = 0xff123456;
    document.assets.emplace_back(ChunkedRasterAsset{"paint", {{-4, -4, std::move(hidden)}}});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return document;
}
BitmapBrush brush() {
    BitmapBrush result;
    result.argb = 0xff439bd1;
    result.size = 7;
    result.flow = 1;
    result.opacity = 1;
    return result;
}
StrokePoint point(double x, double y, double time) {
    StrokePoint result;
    result.position = {x, y};
    result.time = time;
    result.pressure = 1;
    return result;
}
std::uint32_t pixel(const Document &document, int x, int y) {
    const auto *asset = findChunkedRasterAsset(document, "paint");
    if (!asset) return 0;
    for (const auto &chunk : asset->chunks) {
        const auto left = chunk.column * 32, top = chunk.row * 32;
        if (x >= left && x < left + 32 && y >= top && y < top + 32)
            return chunk.pixels.pixels[std::size_t(y - top) * 32 + x - left];
    }
    return 0;
}
bool contains(CanvasRegion region, int x, int y) {
    return x >= region.origin.x && y >= region.origin.y &&
           std::int64_t(x) < std::int64_t(region.origin.x) + region.extent.width &&
           std::int64_t(y) < std::int64_t(region.origin.y) + region.extent.height;
}
void memory() {
    auto document = fixture();
    DocumentHistory history(document);
    const auto originalRegion = canvasRegion(document);
    const auto originalChunks = findChunkedRasterAsset(document, "paint")->chunks.size();
    expect(history.setLayerName("layer", "Ink").changed && history.undo().changed,
           "memory: establish redo branch");
    const auto start = history.beginStroke("paint", brush(), point(-72, -72, 1),
                                           StrokeCanvasMode::Grow, "Growing stroke");
    expect(start.changed && history.strokeActive() && history.statistics().redoEntries == 1 &&
           contains(canvasRegion(document), -72, -72) && pixel(document, -72, -72) != 0,
           "memory: first outside dab grows the canvas while retaining redo");
    expect(history.continueStroke(point(92, 90, 2)).changed &&
           history.endStroke(point(100, 100, 3)).changed,
           "memory: continuous stroke crosses the previous boundary");
    const auto expanded = canvasRegion(document);
    const auto paintedA = pixel(document, -72, -72), paintedB = pixel(document, 100, 100);
    expect(contains(expanded, 100, 100) && paintedA && paintedB &&
           history.statistics().undoEntries == 1 && !history.canRedo(),
           "memory: the whole grown stroke is one undo entry");
    expect(history.setLayerName("layer", "Mixed").changed && history.undo().changed &&
           layerProperties(*findLayer(document, "layer")).name == "Paint" && history.undo().changed &&
           canvasRegion(document) == originalRegion &&
           !pixel(document, -72, -72) && !pixel(document, 100, 100) &&
           findChunkedRasterAsset(document, "paint")->chunks.size() == originalChunks &&
           pixel(document, -125, -128) == 0xff123456,
           "memory: one undo restores geometry, pixels and original hidden chunk");
    expect(history.redo().changed && canvasRegion(document) == expanded &&
           pixel(document, -72, -72) == paintedA && pixel(document, 100, 100) == paintedB &&
           history.redo().changed && layerProperties(*findLayer(document, "layer")).name == "Mixed",
           "memory: mixed redo restores geometry, pixels and layer metadata in order");
    expect(history.undo().changed && history.undo().changed &&
           history.beginStroke("paint", brush(), point(90, -80, 4),
           StrokeCanvasMode::Grow).changed && history.cancelStroke().changed &&
           canvasRegion(document) == originalRegion && !pixel(document, 90, -80) && history.canRedo(),
           "memory: cancel restores geometry and keeps the previous redo");
    auto transparent = brush(); transparent.argb = 0;
    const auto before = encodeIisc(document).bytes;
    expect(history.beginStroke("paint", transparent, point(140, 140, 5), StrokeCanvasMode::Grow).ok() &&
           !history.endStroke(point(140, 140, 6)).changed && encodeIisc(document).bytes == before &&
           history.canRedo(), "memory: a no-op stroke does not expand or discard redo");
    expect(history.beginStroke("paint", brush(), point(140, 140, 7)).ok() &&
           !history.endStroke(point(140, 140, 8)).changed && canvasRegion(document) == originalRegion,
           "memory: the existing default remains clipped");
}
void constraints() {
    auto document = fixture(); DocumentHistoryLimits limits; limits.maximumBytes = 1;
    DocumentHistory history(document, limits); const auto before = encodeIisc(document).bytes;
    expect(history.beginStroke("paint", brush(), point(-72, -72, 1), StrokeCanvasMode::Grow).code ==
           DocumentHistoryCode::BudgetExceeded && encodeIisc(document).bytes == before && !history.strokeActive(),
           "budget: failed expansion never publishes geometry or pixels");
    Document finite; finite.extent = {32, 32};
    finite.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(32, 32)});
    finite.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentHistory finiteHistory(finite); const auto finiteBefore = encodeIisc(finite).bytes;
    expect(finiteHistory.beginStroke("paint", brush(), point(1, 1, 1), StrokeCanvasMode::Grow).code ==
           DocumentHistoryCode::InvalidArgument && encodeIisc(finite).bytes == finiteBefore,
           "finite: automatic growth requires a chunked infinite canvas");
    auto conflictDoc = fixture(); DocumentHistory conflicting(conflictDoc);
    expect(conflicting.beginStroke("paint", brush(), point(-72, -72, 1), StrokeCanvasMode::Grow).changed,
           "conflict: begin expanding stroke");
    DocumentEditor external(conflictDoc);
    expect(external.ensureInfiniteCanvasRegion({{300, 300}, {2, 2}}).changed &&
           conflicting.cancelStroke().code == DocumentHistoryCode::Conflict && conflicting.strokeActive(),
           "conflict: cancel never overwrites an externally expanded canvas");
    expect(conflicting.abandonStroke().ok() && contains(canvasRegion(conflictDoc), 300, 300),
           "conflict: explicit abandon preserves the external region");
    auto unrelatedDoc = fixture(); DocumentHistory unrelated(unrelatedDoc);
    expect(unrelated.beginStroke("paint", brush(), point(-72, -72, 1), StrokeCanvasMode::Grow).changed,
           "conflict: begin stroke before unrelated edit");
    const auto grownRegion = canvasRegion(unrelatedDoc);
    DocumentEditor another(unrelatedDoc);
    expect(another.setLayerName("layer", "External").changed &&
           unrelated.cancelStroke().code == DocumentHistoryCode::Conflict && unrelated.strokeActive() &&
           canvasRegion(unrelatedDoc) == grownRegion && pixel(unrelatedDoc, -72, -72) != 0 &&
           layerProperties(*findLayer(unrelatedDoc, "layer")).name == "External",
           "conflict: a growing stroke cannot shrink the canvas after any external revision");
    expect(unrelated.abandonStroke().ok(), "conflict: abandon after unrelated external edit");
    auto directDoc = fixture(); DocumentHistory direct(directDoc);
    expect(direct.beginStroke("paint", brush(), point(-72, -72, 1), StrokeCanvasMode::Grow).changed,
           "conflict: begin before direct geometry mutation");
    directDoc.infiniteCanvas.origin.x -= 32;
    directDoc.extent.width += 32;
    const auto changedRegion = canvasRegion(directDoc);
    expect(direct.cancelStroke().code == DocumentHistoryCode::Conflict && direct.strokeActive() &&
           canvasRegion(directDoc) == changedRegion && pixel(directDoc, -72, -72) != 0,
           "conflict: untracked geometry changes are detected before cancellation");
    expect(direct.abandonStroke().ok(), "conflict: release directly modified geometry");
    auto overflow = fixture();
    overflow.infiniteCanvas.origin = {std::numeric_limits<std::int32_t>::min(),
                                      std::numeric_limits<std::int32_t>::min()};
    overflow.extent = {64, 64};
    DocumentHistory bounds(overflow); const auto overflowBefore = encodeIisc(overflow).bytes;
    expect(bounds.beginStroke("paint", brush(), point(0, 0, 1), StrokeCanvasMode::Grow).code ==
           DocumentHistoryCode::InvalidArgument && encodeIisc(overflow).bytes == overflowBefore &&
           !bounds.strokeActive(), "bounds: unrepresentable growth is rejected without mutation");
    auto edge = fixture(); DocumentHistory edgeHistory(edge); const auto edgeBefore = encodeIisc(edge).bytes;
    expect(edgeHistory.beginStroke("paint", brush(), point(1000000000, 0, 1), StrokeCanvasMode::Grow).code ==
           DocumentHistoryCode::InvalidArgument && encodeIisc(edge).bytes == edgeBefore,
           "bounds: raster samples beyond the history coordinate limit are not truncated");
}
void wetSource() {
    auto growing = fixture(), alreadyExpanded = fixture();
    DocumentEditor editor(alreadyExpanded);
    expect(editor.ensureInfiniteCanvasRegion({{-136, -136}, {24, 24}}).changed,
           "wet: prepare an independently expanded reference");
    auto mixed = brush(); mixed.argb = 0xffff0000;
    BrushPreset preset; preset.size = mixed.size;
    preset.material.simulation.enabled = true;
    preset.material.simulation.model = BrushSimulationModel::Mixer;
    preset.material.simulation.wetness = 1;
    preset.material.simulation.pickup = 1;
    preset.material.simulation.mixStrength = 1;
    preset.material.simulation.deposit = .5;
    mixed.engineState = resolveBrushPreset(preset, mixed.argb).brush;
    if (!mixed.engineState) { expect(false, "wet: resolve brush"); return; }
    DocumentHistory actual(growing), reference(alreadyExpanded);
    const auto sample = point(-125, -128, 1);
    expect(actual.beginStroke("paint", mixed, sample, StrokeCanvasMode::Grow).changed &&
           actual.endStroke(sample).changed && reference.beginStroke("paint", mixed, sample).changed &&
           reference.endStroke(sample).changed, "wet: render equivalent growing and expanded strokes");
    bool equal = true;
    for (int y = -136; y < -118; ++y)
        for (int x = -133; x < -115; ++x)
            equal = equal && pixel(growing, x, y) == pixel(alreadyExpanded, x, y);
    expect(equal && pixel(growing, -125, -128) != 0xff123456,
           "wet: growth samples the same pre-existing hidden source as an explicitly expanded canvas");
    expect(actual.undo().changed && pixel(growing, -125, -128) == 0xff123456,
           "wet: undo restores the original source pixels outside the initial region");
}
void file() {
    namespace sql = iiFileProvider::native::sql;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false, "file: temporary directory"); return; }
    const auto path = (temporary.path() / "growing-stroke.iisc").string();
    DocumentFile owner; const auto initial = fixture();
    if (!owner.create(path, initial).ok()) { expect(false, "file: create"); return; }
    DocumentHistory history(owner);
    expect(history.setLayerName("layer", "Ink").changed && history.undo().changed,
           "file: establish redo");
    expect(history.beginStroke("paint", brush(), point(-72, -72, 1), StrokeCanvasMode::Grow).changed,
           "file: commit first outside dab");
    const auto writes = owner.lastWriteStatistics();
    expect(writes.recordsWritten >= 3 && writes.payloadBytesWritten < 65536,
           "file: the grown stroke writes bounded metadata and touched tiles");
    Document snapshot;
    expect(DocumentFile::readSnapshot(path, snapshot).ok() && contains(canvasRegion(snapshot), -72, -72) &&
           pixel(snapshot, -72, -72) == pixel(*owner.document(), -72, -72),
           "file: reader sees matching region and pixels in the same transaction");
    const auto stable = encodeIisc(*owner.document()).bytes; const auto revision = owner.revision();
    auto database = sql::Database::open(std::filesystem::u8path(path));
    if (!database) { expect(false, "file: lock connection"); return; }
    auto lock = sql::Transaction::begin(*database, true);
    if (!lock) { expect(false, "file: lock"); return; }
    expect(!history.continueStroke(point(92, 90, 2)).ok() && owner.revision() == revision &&
           encodeIisc(*owner.document()).bytes == stable && history.strokeActive(),
           "file: lock failure preserves prior pixels, geometry and active preview");
    expect(lock->rollback().has_value(), "file: unlock");
    expect(history.continueStroke(point(92, 90, 2)).changed &&
           history.endStroke(point(100, 100, 3)).changed, "file: retry and finish");
    const auto expanded = canvasRegion(*owner.document());
    const auto paintedA = pixel(*owner.document(), -72, -72), paintedB = pixel(*owner.document(), 100, 100);
    expect(history.undo().changed && canvasRegion(*owner.document()) == canvasRegion(initial) &&
           !pixel(*owner.document(), -72, -72) && !pixel(*owner.document(), 100, 100),
           "file: one undo restores original bounds and pixels");
    expect(history.redo().changed && DocumentFile::readSnapshot(path, snapshot).ok() &&
           canvasRegion(snapshot) == expanded && pixel(snapshot, -72, -72) == paintedA &&
           pixel(snapshot, 100, 100) == paintedB && pixel(snapshot, -125, -128) == 0xff123456,
           "file: redo and reopen preserve geometry, pixels and unrelated hidden chunk");
    expect(history.beginStroke("paint", brush(), point(200, 200, 4), StrokeCanvasMode::Grow).changed &&
           history.cancelStroke().changed && DocumentFile::readSnapshot(path, snapshot).ok() &&
           canvasRegion(snapshot) == expanded && !pixel(snapshot, 200, 200),
           "file: cancel restores the previous durable canvas region");
}
void legacy() {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false, "legacy: temporary directory"); return; }
    const auto path = temporary.path() / "legacy-growing.iisc";
    const auto resource = std::filesystem::u8path(IISHAREDCANVAS_TEST_RESOURCE_DIR) /
                          "fixtures/growing-stroke-v1.tsv";
    if (!writeLegacyWorkingFile(path, resource)) { expect(false, "legacy: fixture creation"); return; }
    DocumentFile owner;
    if (!owner.open(path.string()).ok()) { expect(false, "legacy: open"); return; }
    expect(owner.storageVersion() == 1, "legacy: opening does not migrate storage");
    const auto original = canvasRegion(*owner.document());
    DocumentHistory history(owner);
    auto transparent = brush(); transparent.argb = 0;
    expect(history.beginStroke("paint", transparent, point(100, 100, 1), StrokeCanvasMode::Grow).ok() &&
           !history.endStroke(point(100, 100, 2)).changed && owner.storageVersion() == 1,
           "legacy: a no-op outside stroke does not migrate storage");
    expect(history.beginStroke("paint", brush(), point(-72, -72, 3), StrokeCanvasMode::Grow).changed &&
           owner.storageVersion() == 2 && history.endStroke(point(90, 90, 4)).changed,
           "legacy: first real growing stroke migrates with its pixels and region");
    const auto expanded = canvasRegion(*owner.document());
    const auto first = pixel(*owner.document(), -72, -72), last = pixel(*owner.document(), 90, 90);
    Document snapshot;
    expect(history.undo().changed && canvasRegion(*owner.document()) == original &&
           DocumentFile::readSnapshot(path.string(), snapshot).ok() && canvasRegion(snapshot) == original &&
           !pixel(snapshot, -72, -72) && pixel(snapshot, -125, -128) == 0xff123456,
           "legacy: undo persists the old bounds and original hidden chunk");
    expect(history.redo().changed && DocumentFile::readSnapshot(path.string(), snapshot).ok() &&
           canvasRegion(snapshot) == expanded && pixel(snapshot, -72, -72) == first &&
           pixel(snapshot, 90, 90) == last, "legacy: redo survives reopening after migration");

    const auto limitedPath = temporary.path() / "legacy-growing-limited.iisc";
    if (!writeLegacyWorkingFile(limitedPath, resource)) { expect(false, "legacy budget: fixture"); return; }
    SerializationLimits limits; limits.maximumContainerBytes = 6000;
    DocumentFile limited;
    if (!limited.open(limitedPath.string(), limits).ok()) { expect(false, "legacy budget: open"); return; }
    const auto durableBefore = iiFileProvider::native::File::read(limitedPath, 1024 * 1024);
    const auto modelBefore = encodeIisc(*limited.document()).bytes;
    DocumentHistory bounded(limited);
    expect(!bounded.beginStroke("paint", brush(), point(-72, -72, 1), StrokeCanvasMode::Grow).ok() &&
           limited.isOpen() && limited.storageVersion() == 1 && limited.revision() == 0 &&
           encodeIisc(*limited.document()).bytes == modelBefore && !bounded.strokeActive() &&
           iiFileProvider::native::File::read(limitedPath, 1024 * 1024) == durableBefore,
           "legacy budget: rejected migration preserves schema, file bytes, geometry and pixels");
}
}
int main(int argc, char **argv) {
    memory(); constraints(); wetSource();
    if (!(argc == 2 && std::string_view(argv[1]) == "--memory-only")) { file(); legacy(); }
    return failures ? 1 : 0;
}
