#include <Bitmap/BitmapEditor.h>
#include <Bitmap/ChunkedBitmapEditor.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderNative.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <filesystem>
#include <iostream>
#include <tuple>

namespace
{
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool value, const char *message)
{
    if (!value)
    {
        ++failures;
        std::cerr << message << '\n';
    }
}
template <class E>
concept HasPixelPreview = requires(E &e, std::span<const BitmapPatch> patches) {
    { e.beginPixelPreview("Pixel edit") } -> std::same_as<bool>;
    { e.replacePixelPreview(patches) } -> std::same_as<bool>;
    { e.commitPixelPreview() } -> std::same_as<bool>;
    { e.cancelPixelPreview() } -> std::same_as<bool>;
    { e.pixelPreviewActive() } -> std::same_as<bool>;
    { e.pixelPreviewSourceAt(0, 0) } -> std::same_as<std::optional<std::uint32_t>>;
};
Document fixture(bool chunked)
{
    Document d;
    d.extent = {260, 132};
    if (chunked)
    {
        d.canvasMode = CanvasMode::Infinite;
        d.infiniteCanvas.origin = {-130, -1};
        d.infiniteCanvas.chunkSize = 128;
        d.assets.emplace_back(
            ChunkedRasterAsset{"paint", {{-1, -1, makeRasterLayer(128, 128, 0x804488cc)}}});
    }
    else
        d.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(260, 132, 0x804488cc)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return d;
}
auto pixelState(const Document &d)
{
    using Item = std::tuple<int, int, int, int, std::vector<std::uint32_t>>;
    std::vector<Item> result;
    if (auto *a = findRasterAsset(d, "paint"))
        result.emplace_back(0, 0, a->pixels.width, a->pixels.height, a->pixels.pixels);
    if (auto *a = findChunkedRasterAsset(d, "paint"))
        for (const auto &c : a->chunks)
            result.emplace_back(c.column, c.row, c.pixels.width, c.pixels.height, c.pixels.pixels);
    return result;
}
template <class E> void contract(bool chunked)
{
    auto d = fixture(chunked);
    E e(d, "paint");
    const auto original = pixelState(d);
    expect(e.replacePatches(std::array{
               BitmapPatch{{{70, 5}, 1, 1}, std::array<std::uint32_t, 1>{0xff123456}}}) &&
               e.undo(),
           "preview: establish prior redo");
    const auto revision = e.revision(), author = d.authorship.revision();
    expect(e.beginPixelPreview("Preview operation") && e.pixelPreviewActive(),
           "preview: begin without input trajectories");
    expect(e.revision() == revision && d.authorship.revision() == author,
           "preview: begin does not mutate document");
    expect(!e.beginPixelPreview("Nested") && !e.canUndo() && !e.canRedo(),
           "preview: session owns cursor until resolution");
    auto other = fixture(chunked);
    expect(!e.bind(other, "paint") && e.pixelPreviewActive(),
           "preview: binding change cannot discard a live session");
    e.unbind();
    expect(e.isBound() && e.pixelPreviewActive() &&
               e.lastHistoryResult().code == RasterHistoryCode::InvalidArgument,
           "preview: noexcept unbind preserves a live session until explicit resolution");
    std::array<std::uint32_t, 4> a{0x406699cc, 0x406699cc, 0x406699cc, 0x406699cc};
    std::array<std::uint32_t, 1> b{0x80cc4422};
    const int left = chunked ? -1 : 1, top = chunked ? -1 : 1;
    const auto prior = e.pixelAt(left, top);
    const std::array first{BitmapPatch{{{left, top}, 2, 2}, a}, BitmapPatch{{{129, 65}, 1, 1}, b}};
    expect(e.replacePixelPreview(first) && e.pixelAt(left, top) == a[0],
           "preview: raw final pixels replace without re-compositing alpha");
    expect(e.pixelPreviewSourceAt(left, top) == prior,
           "preview: sampler reads session-start source");
    expect(e.historyStatistics().undoEntries == 1 && !e.replacePatches(first) &&
               !e.beginStroke({8., 8.}),
           "preview: ordinary edits and brush cannot interleave");
    b[0] = 0x60223344;
    const std::array second{BitmapPatch{{{129, 65}, 1, 1}, b}};
    expect(e.replacePixelPreview(second) && e.pixelAt(left, top) == prior &&
               e.pixelAt(129, 65) == b[0],
           "preview: omitted old footprint restores original source");
    const auto stable = pixelState(d);
    const auto stableRevision = e.revision();
    const auto cache = e.previewStorageBytes();
    expect(e.replacePixelPreview(second) && e.revision() == stableRevision,
           "preview: identical replacement does not create a document revision");
    expect(!e.replacePixelPreview(std::array{second[0], second[0]}) &&
               e.lastHistoryResult().code == RasterHistoryCode::InvalidArgument,
           "preview: overlap rejects whole replacement");
    expect(pixelState(d) == stable && e.revision() == stableRevision &&
               e.previewStorageBytes() == cache && e.pixelPreviewActive(),
           "preview: rejected replacement preserves previous visible result and cursor");
    expect(e.replacePixelPreview({}) && pixelState(d) == original,
           "preview: empty complete replacement restores all original pixels/chunks");
    expect(e.historyStatistics().undoEntries == 0 && e.historyStatistics().redoEntries == 1,
           "preview: empty final result retains pre-session redo");
    const auto emptyRevision = e.revision();
    expect(e.commitPixelPreview() && !e.pixelPreviewActive() && e.previewStorageBytes() == 0 &&
               e.revision() == emptyRevision && e.canRedo(),
           "preview: no-op commit releases cache without extra revision");
    expect(e.beginPixelPreview("Final edit") && e.replacePixelPreview(first) &&
               e.replacePixelPreview(second) && e.commitPixelPreview(),
           "preview: publish final result as one command");
    const auto final = pixelState(d);
    expect(e.historyStatistics().undoEntries == 1 && e.undo() && pixelState(d) == original &&
               e.redo() && pixelState(d) == final,
           "preview: one undo/redo restores entire final result");
    expect(e.undo() && e.beginPixelPreview("Cancel edit") && e.replacePixelPreview(first) &&
               e.cancelPixelPreview(),
           "preview: explicit cancellation succeeds");
    expect(!e.pixelPreviewActive() && pixelState(d) == original && e.canRedo() &&
               e.previewStorageBytes() == 0,
           "preview: cancellation preserves earlier redo");
    expect(!e.cancelPixelPreview() && !e.commitPixelPreview(),
           "preview: resolving an absent session is rejected");
    expect(e.beginPixelPreview("Conflicting edit") && e.replacePixelPreview(second),
           "preview: conflict fixture");
    if (chunked)
    {
        auto *asset = findChunkedRasterAsset(d, "paint");
        for (auto &c : asset->chunks)
            if (c.column == 1 && c.row == 0)
                c.pixels.pixels[65 * 128 + 1] = 0xffdeadbe;
    }
    else
        findRasterAsset(d, "paint")->pixels.pixels[65 * 260 + 129] = 0xffdeadbe;
    expect(!e.cancelPixelPreview() && e.lastHistoryResult().code == RasterHistoryCode::Conflict &&
               e.pixelPreviewActive() && e.pixelAt(129, 65) == 0xffdeadbe,
           "preview: cancellation never overwrites foreign edits");
    if (chunked)
    {
        auto *asset = findChunkedRasterAsset(d, "paint");
        for (auto &c : asset->chunks)
            if (c.column == 1 && c.row == 0)
                c.pixels.pixels[65 * 128 + 1] = b[0];
    }
    else
        findRasterAsset(d, "paint")->pixels.pixels[65 * 260 + 129] = b[0];
    expect(e.cancelPixelPreview(), "preview: conflict remains retryable");
}
template <class E> void limits(bool chunked)
{
    auto d = fixture(chunked);
    RasterHistoryLimits limits;
    limits.maximumEntries = 1;
    E e(d, "paint", limits);
    const std::array<std::uint32_t, 1> value{0xff123456};
    const std::array one{BitmapPatch{{{70, 5}, 1, 1}, value}},
        two{BitmapPatch{{{72, 5}, 1, 1}, value}};
    expect(e.replacePatches(one) && e.beginPixelPreview("Budget"), "preview budget: fixture");
    const auto before = pixelState(d);
    const auto revision = e.revision();
    expect(!e.replacePixelPreview(two) &&
               e.lastHistoryResult().code == RasterHistoryCode::BudgetExceeded,
           "preview budget: reject before publishing");
    expect(pixelState(d) == before && e.revision() == revision && e.pixelPreviewActive() &&
               e.previewStorageBytes() == 0,
           "preview budget: failed first update remains empty and active");
    expect(e.cancelPixelPreview() && e.historyStatistics().undoEntries == 1,
           "preview budget: empty cancellation retains prior history");
}
template <class E> void fileContract(bool chunked)
{
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false, "preview file: reserve test directory"); return; }
    const auto root = temporary.path() / (chunked ? "chunked" : "finite");
    std::filesystem::create_directories(root);
    const auto path = (root / "preview.iisc").string();
    DocumentFile file;
    auto d = fixture(chunked);
    const auto original = pixelState(d);
    expect(file.create(path, d).ok(), "preview file: create");
    E e(file, "paint");
    const auto initialRevision = file.revision();
    const std::array<std::uint32_t, 1> value{0x80445566};
    const std::array patch{BitmapPatch{{{70, 5}, 1, 1}, value}};
    expect(e.beginPixelPreview("File preview") && file.revision() == initialRevision,
           "preview file: begin is navigation only");
    expect(e.replacePixelPreview(patch) && file.revision() == initialRevision + 1 &&
               e.cancelPixelPreview(),
           "preview file: synchronous replacement/cancel");
    file.close();
    expect(file.open(path).ok() && pixelState(*file.document()) == original,
           "preview file: cancellation persisted exactly");
    E stale(file, "paint");
    expect(stale.beginPixelPreview("Stale") && stale.replacePixelPreview(patch),
           "preview file: stale fixture");
    file.close();
    expect(!stale.cancelPixelPreview() &&
               stale.lastHistoryResult().code == RasterHistoryCode::Conflict &&
               !stale.pixelPreviewSourceAt(70, 5),
           "preview file: generation rejects old session and source access");
}
template <class E> void admission(bool chunked)
{
    auto d = fixture(chunked);
    RasterHistoryLimits limits;
    limits.maximumEntries = 1;
    limits.overflow = RasterHistoryOverflow::EvictOldest;
    E e(d, "paint", limits);
    const std::array<std::uint32_t, 1> seed{0xff112233}, final{0x80665544};
    const std::array before{BitmapPatch{{{70, 5}, 1, 1}, seed}},
        preview{BitmapPatch{{{129, 65}, 1, 1}, final}};
    const auto original = e.pixelAt(129, 65);
    expect(e.replacePatches(before) && e.beginPixelPreview("Reported operation") &&
               e.replacePixelPreview(preview) && e.lastHistoryResult().evictedEntries == 1,
           "preview admission: provisional eviction is explicit");
    expect(!e.replacePixelPreview(std::array{preview[0], preview[0]}),
           "preview admission: rejected update keeps admitted result");
    const auto revision = e.revision();
    expect(e.commitPixelPreview() && e.revision() == revision && e.lastHistoryResult().changed &&
               e.lastHistoryResult().evictedEntries == 1,
           "preview admission: commit reports the final successful admission after a rejection");
    expect(e.undo() && e.pixelAt(129, 65) == original && e.pixelAt(70, 5) == seed[0] &&
               !e.canUndo(),
           "preview admission: only the final command remains after explicit eviction");
}
template <class E> void emptyStrokeAdmission(bool chunked)
{
    auto d = fixture(chunked);
    RasterHistoryLimits limits;
    limits.maximumEntries = 0;
    E e(d, "paint", limits);
    BitmapBrush brush;
    brush.size = 4;
    e.setBrush(brush);
    StrokePoint point;
    point.position = {16, 16};
    point.pressure = 0;
    point.time = 1;
    expect(e.beginStrokeInput(point), "stroke admission: begin zero-coverage stroke");
    auto rejected = point;
    rejected.position.x = 18;
    rejected.pressure = 1;
    rejected.time = 1.1;
    expect(!e.continueStrokeInput(rejected) &&
               e.lastHistoryResult().code == RasterHistoryCode::BudgetExceeded,
           "stroke admission: visible input rejected by zero-entry budget");
    point.time = 1.2;
    expect(e.endStrokeInput(point) && e.lastHistoryResult().ok() && !e.lastHistoryResult().changed,
           "stroke admission: successful empty finish clears a prior rejected-input result");
}
template <class E> void dispatch(bool chunked)
{
    if constexpr (!HasPixelPreview<E>)
    {
        ++failures;
        std::cerr << "missing generic final-pixel preview replacement API\n";
    }
    else
    {
        contract<E>(chunked);
        limits<E>(chunked);
        fileContract<E>(chunked);
        admission<E>(chunked);
        emptyStrokeAdmission<E>(chunked);
    }
}
void large()
{
    auto d = fixture(false);
    d.extent = {8192, 8192};
    findRasterAsset(d, "paint")->pixels = makeRasterLayer(8192, 8192, 0x804488cc);
    d.assets.emplace_back(RasterAsset{"backdrop", makeRasterLayer(8192, 8192, 0x7f010203)});
    d.assets.emplace_back(RasterAsset{"mask", makeRasterLayer(8192, 8192, 0xff112233)});
    d.layers.emplace_back(
        StaticBitmapLayer{{"backdrop-layer", "Backdrop"}, StaticSource{"backdrop"}});
    layerProperties(d.layers.front()).appearance.rasterMask = LayerMask{.assetId = "mask"};
    RasterHistoryLimits limits;
    limits.maximumBytes = 1024 * 1024;
    BitmapEditor e(d, "paint", limits);
    expect(e.setPixel(0, 0, 0xff123456) && e.undo(), "8K preview: establish prior redo");
    std::vector<std::uint32_t> pixels(64 * 16, 0xff556677);
    std::array<std::uint32_t, 1> corner{0x40aabbcc};
    const std::array a{BitmapPatch{{{4096, 4096}, 64, 16}, pixels}},
        b{BitmapPatch{{{8191, 8191}, 1, 1}, corner}};
    const auto begin = std::chrono::steady_clock::now();
    expect(e.beginPixelPreview("8K cancel") && e.replacePixelPreview(a) && e.replacePixelPreview(b),
           "8K preview: replace distant complete result");
    const auto cache = e.previewStorageBytes();
    expect(e.pixelAt(4096, 4096) == 0x804488cc && e.pixelAt(8191, 8191) == corner[0] &&
               e.pixelPreviewSourceAt(8191, 8191) == 0x804488cc,
           "8K preview: old footprint and sampler use original source");
    expect(cache < 1024 * 1024 && e.cancelPixelPreview() && e.historyStatistics().redoEntries == 1,
           "8K preview: sparse cache and earlier redo retained on cancel");
    expect(e.beginPixelPreview("8K final") && e.replacePixelPreview(a) &&
               e.replacePixelPreview(b) && e.commitPixelPreview() &&
               e.historyStatistics().undoEntries == 1 && e.undo(),
           "8K preview: complete final result is a single command");
    const auto operationTime = std::chrono::steady_clock::now() - begin;
    const std::array<std::uint32_t, 3> originals{0x804488cc, 0x7f010203, 0xff112233};
    bool restored = true;
    for (std::size_t i = 0; i < d.assets.size(); ++i)
        restored = std::ranges::all_of(std::get<RasterAsset>(d.assets[i]).pixels.pixels,
                                       [&](auto pixel) { return pixel == originals[i]; }) &&
                   restored;
    expect(restored, "8K preview: every original paint, backdrop and mask pixel remains intact");
    std::cout << "PIXEL_PREVIEW_8K_OK source_bytes=" << 3ULL * 8192 * 8192 * 4
              << " cache_bytes=" << cache
              << " retained_bytes=" << e.historyStatistics().retainedBytes
              << " checked_pixels=" << 3ULL * 8192 * 8192
              << " source_restored=" << (restored ? "true" : "false") << " operations_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(operationTime).count()
              << '\n';
}
} // namespace
int main(int argc, char **)
{
    if (argc > 1)
    {
        large();
        return failures ? 1 : 0;
    }
    dispatch<iiSharedCanvas::BitmapEditor>(false);
    dispatch<iiSharedCanvas::ChunkedBitmapEditor>(true);
    return failures ? 1 : 0;
}
