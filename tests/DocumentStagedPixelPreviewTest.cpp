#include <Document/DocumentEditor.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderNative.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, std::string_view message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
Document fixture(bool chunked) {
    Document d; d.extent = {32, 32};
    if (chunked) {
        d.canvasMode = CanvasMode::Infinite; d.infiniteCanvas = {{0, 0}, 32};
        d.assets.emplace_back(ChunkedRasterAsset{"paint", {{2, 2, makeRasterLayer(32, 32, 0xff123456)}}});
    } else d.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(32, 32)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return d;
}
std::uint32_t pixel(const Document &d, int x, int y) {
    if (const auto *r = findRasterAsset(d, "paint")) return rasterLayerPixelAt(r->pixels, {x, y});
    const auto *r = findChunkedRasterAsset(d, "paint");
    for (const auto &c : r->chunks) {
        const int left = c.column * 32, top = c.row * 32;
        if (x >= left && x < left + 32 && y >= top && y < top + 32)
            return rasterLayerPixelAt(c.pixels, {x - left, y - top});
    }
    return 0;
}
std::optional<std::uint32_t> preview(const DocumentHistory &h, int x, int y) {
    for (const auto &tile : h.stagedPixelPreviewTiles()) {
        const auto &r = tile.bounds;
        if (x >= r.origin.x && y >= r.origin.y &&
            std::int64_t(x) < std::int64_t(r.origin.x) + r.width &&
            std::int64_t(y) < std::int64_t(r.origin.y) + r.height)
            return tile.pixels[std::size_t(y - r.origin.y) * r.width + x - r.origin.x];
    }
    return std::nullopt;
}
std::uint32_t resolved(const DocumentHistory &h, const Document &d, int x, int y) {
    return preview(h, x, y).value_or(pixel(d, x, y));
}
void memory(bool chunked) {
    auto d = fixture(chunked); DocumentHistory h(d);
    expect(h.setLayerName("layer", "Ink").changed && h.undo().changed, "memory: establish redo");
    const auto initial = encodeIisc(d).bytes; const auto revision = d.authorship.revision();
    const auto originalRegion = canvasRegion(d);
    expect(h.beginStagedPixelPreview("paint").ok() && h.pixelPreviewActive() &&
           h.stagedPixelPreviewRegion() == originalRegion && h.stagedPixelPreviewTiles().empty(),
           "memory: staged preview begins without document changes");
    const std::array<std::uint32_t, 1> a{0xff234567}, b{0xffabcdef};
    const std::array first{BitmapPatch{{{3, 3}, 1, 1}, a}};
    const std::array second{BitmapPatch{{{7, 7}, 1, 1}, b}};
    expect(h.replacePixelPreview(first).changed && preview(h, 3, 3) == a[0] &&
           h.pixelPreviewSourceAt(3, 3) == 0 && encodeIisc(d).bytes == initial &&
           d.authorship.revision() == revision && h.previewPendingHistoryBytes() > 0,
           "memory: staged pixels are visible only through the borrowed overlay");
    h.clearPreviewDirtyBounds();
    expect(!h.replacePixelPreview(first).changed && h.previewDirtyBounds().width == 0,
           "memory: identical staged output is a view no-op");
    expect(h.replacePixelPreview(second).changed && resolved(h, d, 3, 3) == 0 &&
           preview(h, 7, 7) == b[0] && h.previewDirtyBounds().width > 0 &&
           encodeIisc(d).bytes == initial, "memory: a replacement restores omitted original pixels");
    const auto pending = h.previewPendingHistoryBytes();
    expect(h.replacePixelPreview(std::array{second[0], second[0]}).code == DocumentHistoryCode::InvalidArgument &&
           h.previewPendingHistoryBytes() == pending && preview(h, 7, 7) == b[0],
           "memory: invalid input preserves the prior staged preview");
    h.clearPreviewDirtyBounds();
    expect(h.cancelPixelPreview().changed && !h.pixelPreviewActive() && h.canRedo() &&
           h.previewDirtyBounds().width > 0 && encodeIisc(d).bytes == initial &&
           d.authorship.revision() == revision, "memory: cancellation redraws but does not mutate the owner");
    expect(h.beginStagedPixelPreview("paint").ok() && h.replacePixelPreview(second).changed &&
           h.commitPixelPreview().changed && d.authorship.revision() == revision + 1 &&
           pixel(d, 7, 7) == b[0] && !h.canRedo(), "memory: commit applies once and discards redo");
    expect(h.undo().changed && pixel(d, 7, 7) == 0 && h.redo().changed && pixel(d, 7, 7) == b[0],
           "memory: staged commit is one reversible entry");
    expect(h.undo().changed, "memory: undo before no-op preview");
    const auto beforeNoOp = encodeIisc(d).bytes;
    expect(h.beginStagedPixelPreview("paint").ok() && h.replacePixelPreview(second).changed &&
           h.replacePixelPreview({}).changed && !h.commitPixelPreview().changed && h.canRedo() &&
           encodeIisc(d).bytes == beforeNoOp,
           "memory: an empty final preview preserves the old redo branch");
}
void growth() {
    auto d = fixture(true); DocumentHistory h(d); const auto before = encodeIisc(d).bytes;
    const auto original = canvasRegion(d); const auto revision = d.authorship.revision();
    const std::array<std::uint32_t, 1> untouched{0xff123456}, paint{0xffeeddcc};
    const std::array same{BitmapPatch{{{65, 65}, 1, 1}, untouched}};
    const std::array outside{BitmapPatch{{{65, 65}, 1, 1}, paint}};
    expect(h.beginStagedPixelPreview("paint", PixelPreviewCanvasMode::Grow).ok() &&
           !h.replacePixelPreview(same).changed && h.stagedPixelPreviewRegion() == original &&
           h.pixelPreviewSourceAt(65, 65) == untouched[0],
           "growth: unchanged hidden pixels never expand the preview");
    expect(h.replacePixelPreview(outside).changed && preview(h, 65, 65) == paint[0] &&
           h.stagedPixelPreviewRegion() && h.stagedPixelPreviewRegion()->extent.width == 96 &&
           canvasRegion(d) == original && pixel(d, 65, 65) == untouched[0] && encodeIisc(d).bytes == before,
           "growth: preview expands without mutating hidden pixels or owner geometry");
    expect(h.commitPixelPreview().changed && d.authorship.revision() == revision + 1 &&
           canvasRegion(d).extent.width == 96 && pixel(d, 65, 65) == paint[0],
           "growth: one commit publishes pixels and region together");
    expect(h.undo().changed && canvasRegion(d) == original && pixel(d, 65, 65) == untouched[0] &&
           h.redo().changed && canvasRegion(d).extent.width == 96 && pixel(d, 65, 65) == paint[0],
           "growth: undo and redo restore geometry and the previous hidden pixel");
    auto finite = fixture(false); DocumentHistory finiteHistory(finite);
    expect(finiteHistory.beginStagedPixelPreview("paint", PixelPreviewCanvasMode::Grow).code ==
           DocumentHistoryCode::InvalidArgument && !finiteHistory.pixelPreviewActive(),
           "growth: finite bitmaps reject automatic canvas expansion");
    expect(finiteHistory.beginStagedPixelPreview("paint", static_cast<PixelPreviewCanvasMode>(42)).code ==
           DocumentHistoryCode::InvalidArgument && !finiteHistory.pixelPreviewActive(),
           "growth: unknown policies are rejected");
    auto shrinking = fixture(true); DocumentHistory transient(shrinking);
    const std::array<std::uint32_t, 1> insideColor{0xff445566};
    const std::array inside{BitmapPatch{{{1, 1}, 1, 1}, insideColor}};
    expect(transient.beginStagedPixelPreview("paint", PixelPreviewCanvasMode::Grow).ok() &&
           transient.replacePixelPreview(outside).changed, "growth: expanded transient preview");
    transient.clearPreviewDirtyBounds();
    expect(transient.replacePixelPreview(inside).changed &&
           transient.stagedPixelPreviewRegion() == canvasRegion(shrinking) &&
           transient.previewDirtyBounds().origin.x <= 1 &&
           std::int64_t(transient.previewDirtyBounds().origin.x) + transient.previewDirtyBounds().width > 65 &&
           pixel(shrinking, 65, 65) == untouched[0] && transient.cancelPixelPreview().changed,
           "growth: shrinking preview redraws both old and new bounds without touching the owner");
    auto far = fixture(true); DocumentHistory bounded(far); const auto farBefore = encodeIisc(far).bytes;
    const std::array<std::uint32_t, 2> farPixels{paint[0], paint[0]};
    const std::array invalid{BitmapPatch{{{999999999, 0}, 2, 1}, farPixels}};
    expect(bounded.beginStagedPixelPreview("paint", PixelPreviewCanvasMode::Grow).ok() &&
           bounded.replacePixelPreview(invalid).code == DocumentHistoryCode::InvalidArgument &&
           bounded.pixelPreviewActive() && encodeIisc(far).bytes == farBefore &&
           !bounded.cancelPixelPreview().changed,
           "growth: out-of-range inputs leave no partial preview");
}
void budgets() {
    auto d = fixture(false); DocumentHistoryLimits limits; limits.maximumBytes = 1; limits.maximumEntries = 1;
    DocumentHistory h(d, limits); const auto before = encodeIisc(d).bytes;
    const std::array<std::uint32_t, 1> value{0xffaabbcc};
    const std::array patch{BitmapPatch{{{3, 3}, 1, 1}, value}};
    expect(h.beginStagedPixelPreview("paint").ok() &&
           h.replacePixelPreview(patch).code == DocumentHistoryCode::BudgetExceeded &&
           h.pixelPreviewActive() && h.stagedPixelPreviewTiles().empty() &&
           encodeIisc(d).bytes == before && !h.cancelPixelPreview().changed,
           "budget: an unadmitted staged result preserves the owner and session");
    auto limited = fixture(false); DocumentHistoryLimits inputLimit;
    inputLimit.maximumBitmapInputBytes = 32;
    DocumentHistory inputHistory(limited, inputLimit); const auto inputBefore = encodeIisc(limited).bytes;
    expect(inputHistory.beginStagedPixelPreview("paint").ok() &&
           inputHistory.replacePixelPreview(patch).code == DocumentHistoryCode::BudgetExceeded &&
           inputHistory.previewStorageBytes() == 0 && encodeIisc(limited).bytes == inputBefore &&
           !inputHistory.cancelPixelPreview().changed,
           "budget: an input/cache limit failure leaves no partial preview");
}
void movingFootprint() {
    auto d = fixture(true); DocumentHistoryLimits limits;
    limits.maximumBitmapInputBytes = 100000;
    DocumentHistory h(d, limits); const auto before = encodeIisc(d).bytes;
    const auto region = canvasRegion(d); const auto revision = d.authorship.revision();
    const std::array<std::uint32_t, 1> color{0xffcceeaa};
    expect(h.beginStagedPixelPreview("paint", PixelPreviewCanvasMode::Grow).ok(),
           "moving: begin staged preview");
    bool okay = true;
    for (int i = 0; i < 100; ++i) {
        const int x = 128 + i * 64, y = -128 - i * 64;
        const std::array patch{BitmapPatch{{{x, y}, 1, 1}, color}};
        const auto changed = h.replacePixelPreview(patch);
        if (!changed.ok() || !changed.changed || h.stagedPixelPreviewTiles().size() != 1 ||
            h.previewStorageBytes() > 40000 || pixel(d, x, y) != 0 ||
            d.authorship.revision() != revision) { okay = false; break; }
    }
    expect(okay && encodeIisc(d).bytes == before,
           "moving: discarded footprints do not consume cache or change the owner");
    if (okay) {
        expect(h.commitPixelPreview().changed && pixel(d, 128 + 99 * 64, -128 - 99 * 64) == color[0] &&
               h.undo().changed && canvasRegion(d) == region &&
               pixel(d, 128 + 99 * 64, -128 - 99 * 64) == 0,
               "moving: last footprint alone commits and undoes");
    } else if (h.pixelPreviewActive()) h.cancelPixelPreview();
}
void randomized(bool grow) {
    using Coordinate = std::pair<int, int>;
    auto d = fixture(grow); DocumentHistory h(d);
    const auto before = encodeIisc(d).bytes; const auto original = canvasRegion(d);
    const auto mode = grow ? PixelPreviewCanvasMode::Grow : PixelPreviewCanvasMode::Clip;
    expect(h.beginStagedPixelPreview("paint", mode).ok(), "random: begin staged preview");
    std::map<Coordinate, std::uint32_t> previous;
    std::uint32_t random = 0x91a2b3c4;
    const auto nextRandom = [&]() {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random;
    };
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::array<std::uint32_t, 4> values{};
        std::vector<BitmapPatch> patches;
        std::map<Coordinate, std::uint32_t> all, changed;
        for (int i = 0; i < 4; ++i) {
            const int x = grow ? int(nextRandom() % 127) - 31 : int(nextRandom() % 32);
            const int y = grow ? int(nextRandom() % 127) - 31 : int(nextRandom() % 32);
            const Coordinate key{x, y};
            if (all.contains(key)) continue;
            const auto base = pixel(d, x, y);
            values[std::size_t(i)] = nextRandom() % 3 == 0 ? base : (0xff000000U | (nextRandom() & 0xffffffU));
            all.emplace(key, values[std::size_t(i)]);
            if (values[std::size_t(i)] != base) changed.emplace(key, values[std::size_t(i)]);
            patches.push_back({{{x, y}, 1, 1}, std::span<const std::uint32_t>(&values[std::size_t(i)], 1)});
        }
        const auto result = h.replacePixelPreview(patches);
        expect(result.ok() && result.changed == (previous != changed) && encodeIisc(d).bytes == before,
               "random: replacement matches expected change without owner mutation");
        for (const auto &[key, old] : previous) {
            const auto found = changed.find(key);
            const auto expected = found == changed.end() ? pixel(d, key.first, key.second) : found->second;
            expect(resolved(h, d, key.first, key.second) == expected, "random: old footprint is restored or replaced");
        }
        for (const auto &[key, value] : all)
            expect(preview(h, key.first, key.second) == value, "random: submitted final pixel is present");
        auto expectedRegion = original;
        if (grow && !changed.empty()) {
            int left = 0, top = 0, right = 32, bottom = 32;
            for (const auto &[key, value] : changed) {
                left = std::min(left, key.first); top = std::min(top, key.second);
                right = std::max(right, key.first + 1); bottom = std::max(bottom, key.second + 1);
            }
            const auto floor32 = [](int value) { return (value >= 0 ? value / 32 : (value - 31) / 32) * 32; };
            const auto ceil32 = [](int value) { return (value > 0 ? (value + 31) / 32 : value / 32) * 32; };
            left = floor32(left); top = floor32(top); right = ceil32(right); bottom = ceil32(bottom);
            expectedRegion = {{left, top}, {right - left, bottom - top}};
        }
        expect(h.stagedPixelPreviewRegion() == expectedRegion, "random: preview region encloses only net changes");
        previous = std::move(changed);
    }
    const std::array<std::uint32_t, 1> finalColor{0xffaabbcc};
    const std::array finalPatch{BitmapPatch{{{2, 2}, 1, 1}, finalColor}};
    expect(h.replacePixelPreview(finalPatch).ok() && h.commitPixelPreview().changed &&
           pixel(d, 2, 2) == finalColor[0] && h.undo().changed && canvasRegion(d) == original &&
           pixel(d, 2, 2) == 0, "random: final result commits and undoes as one edit");
}
void conflicts() {
    auto d = fixture(false); DocumentHistory h(d);
    const std::array<std::uint32_t, 1> value{0xffaabbcc};
    const std::array patch{BitmapPatch{{{2, 2}, 1, 1}, value}};
    expect(h.beginStagedPixelPreview("paint").ok() && h.replacePixelPreview(patch).changed,
           "conflict: stage a preview");
    findRasterAsset(d, "paint")->pixels.pixels[2 * 32 + 2] = 0xff998877;
    expect(h.commitPixelPreview().code == DocumentHistoryCode::Conflict && h.pixelPreviewActive() &&
           pixel(d, 2, 2) == 0xff998877 && h.cancelPixelPreview().changed &&
           pixel(d, 2, 2) == 0xff998877, "conflict: direct external pixels are never overwritten");
    expect(h.beginStagedPixelPreview("paint").ok() && h.replacePixelPreview(patch).changed,
           "conflict: stage before metadata edit");
    DocumentEditor external(d); expect(external.setLayerVisible("layer", false).changed, "conflict: external metadata");
    expect(h.commitPixelPreview().code == DocumentHistoryCode::Conflict &&
           h.cancelPixelPreview().changed && !h.pixelPreviewActive() &&
           !layerProperties(*findLayer(d, "layer")).visible && h.reset().ok(),
           "conflict: cancel is safe even after an unrelated external revision");
    auto grown = fixture(true); DocumentHistory geometry(grown);
    const std::array outside{BitmapPatch{{{-4, -4}, 1, 1}, value}};
    expect(geometry.beginStagedPixelPreview("paint", PixelPreviewCanvasMode::Grow).ok() &&
           geometry.replacePixelPreview(outside).changed, "geometry conflict: stage outside the canvas");
    grown.infiniteCanvas.origin.x -= 32; grown.extent.width += 32;
    const auto changedRegion = canvasRegion(grown);
    expect(geometry.commitPixelPreview().code == DocumentHistoryCode::Conflict &&
           geometry.cancelPixelPreview().changed && canvasRegion(grown) == changedRegion &&
           pixel(grown, -4, -4) == 0, "geometry conflict: preserve an untracked external region change");
}
void file(bool chunked) {
    namespace sql = iiFileProvider::native::sql;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory temp(parent);
    if (!temp.isValid()) { expect(false, "file: temporary directory"); return; }
    const auto path = (temp.path() / "staged-preview.iisc").string();
    DocumentFile owner;
    if (!owner.create(path, fixture(chunked)).ok()) { expect(false, "file: create"); return; }
    DocumentHistory h(owner); const auto revision = owner.revision();
    const auto before = encodeIisc(*owner.document()).bytes;
    const std::array<std::uint32_t, 1> value{0xffaabbcc};
    const std::array patch{BitmapPatch{{{chunked ? 65 : 3, chunked ? 65 : 3}, 1, 1}, value}};
    const auto mode = chunked ? PixelPreviewCanvasMode::Grow : PixelPreviewCanvasMode::Clip;
    expect(h.beginStagedPixelPreview("paint", mode).ok() && h.replacePixelPreview(patch).changed &&
           owner.revision() == revision && encodeIisc(*owner.document()).bytes == before,
           "file: stage without any durable edit");
    Document snapshot;
    expect(DocumentFile::readSnapshot(path, snapshot).ok() && encodeIisc(snapshot).bytes == before,
           "file: another reader sees only committed content");
    auto database = sql::Database::open(std::filesystem::u8path(path));
    if (!database) { expect(false, "file: open lock connection"); return; }
    auto lock = sql::Transaction::begin(*database, true);
    if (!lock) { expect(false, "file: acquire lock"); return; }
    expect(!h.commitPixelPreview().ok() && h.pixelPreviewActive() && owner.revision() == revision &&
           preview(h, chunked ? 65 : 3, chunked ? 65 : 3) == value[0],
           "file: failed commit retains the preview and owner revision");
    expect(lock->rollback().has_value(), "file: release lock");
    expect(h.commitPixelPreview().changed && owner.revision() == revision + 1 &&
           DocumentFile::readSnapshot(path, snapshot).ok() &&
           pixel(snapshot, chunked ? 65 : 3, chunked ? 65 : 3) == value[0],
           "file: one retry publishes exactly one revision");
    expect(h.undo().changed && h.redo().changed &&
           DocumentFile::readSnapshot(path, snapshot).ok() &&
           pixel(snapshot, chunked ? 65 : 3, chunked ? 65 : 3) == value[0],
           "file: durable staged commit has one undo and redo");
    const auto committed = encodeIisc(*owner.document()).bytes; const auto committedRev = owner.revision();
    expect(h.beginStagedPixelPreview("paint", mode).ok() && h.replacePixelPreview({}).ok() &&
           h.cancelPixelPreview().ok() && owner.revision() == committedRev &&
           DocumentFile::readSnapshot(path, snapshot).ok() && encodeIisc(snapshot).bytes == committed,
           "file: cancellation leaves the persisted document and revision unchanged");
}
void externalFile() {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory temp(parent);
    if (!temp.isValid()) { expect(false, "external: temporary directory"); return; }
    const auto path = (temp.path() / "external.iisc").string();
    DocumentFile owner;
    if (!owner.create(path, fixture(false)).ok()) { expect(false, "external: create"); return; }
    DocumentHistory h(owner); const std::array<std::uint32_t, 1> value{0xff112233};
    const std::array patch{BitmapPatch{{{2, 2}, 1, 1}, value}};
    expect(h.beginStagedPixelPreview("paint").ok() && h.replacePixelPreview(patch).changed,
           "external: stage before another writer");
    DocumentFile other; expect(other.open(path).ok(), "external: second file handle");
    DocumentEditor editor(other); expect(editor.setLayerVisible("layer", false).changed,
                                        "external: publish independent edit");
    expect(!h.commitPixelPreview().ok() && h.pixelPreviewActive() && h.cancelPixelPreview().changed &&
           !h.pixelPreviewActive(), "external: staged cancel needs no file write after conflict");
    Document snapshot;
    expect(DocumentFile::readSnapshot(path, snapshot).ok() &&
           !layerProperties(*findLayer(snapshot, "layer")).visible && pixel(snapshot, 2, 2) == 0,
           "external: cancellation preserves the other writer's document");
    DocumentFile closed; const auto closedPath = (temp.path() / "closed.iisc").string();
    expect(closed.create(closedPath, fixture(false)).ok(), "closed: create");
    DocumentHistory closedHistory(closed);
    expect(closedHistory.beginStagedPixelPreview("paint").ok() &&
           closedHistory.replacePixelPreview(patch).changed, "closed: stage");
    closed.close();
    expect(closedHistory.cancelPixelPreview().changed && !closedHistory.pixelPreviewActive() &&
           DocumentFile::readSnapshot(closedPath, snapshot).ok() && pixel(snapshot, 2, 2) == 0,
           "closed: cancellation safely releases a preview after the binding is closed");
}
}
int main() {
    memory(false); memory(true); growth(); budgets(); movingFootprint(); randomized(false); randomized(true);
    conflicts(); file(false); file(true); externalFile();
    if (!failures) std::cout << "STAGED_PREVIEW_OK memory=true file=true grow=true conflicts=true undo=true\n";
    return failures ? 1 : 0;
}
