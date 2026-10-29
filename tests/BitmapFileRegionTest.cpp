#include <Bitmap/BitmapEditor.h>
#include <Bitmap/ChunkedBitmapEditor.h>
#include <File/DocumentFile.h>
#include <iiFileProviderNative.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <set>
#include <string_view>

namespace allocation {
thread_local bool enabled = false;
thread_local std::size_t largest = 0, total = 0;
thread_local std::size_t calls = 0, failAt = std::numeric_limits<std::size_t>::max();
}
void *operator new(std::size_t count)
{
    if (allocation::enabled) {
        allocation::largest = std::max(allocation::largest, count);
        allocation::total += count;
        if (allocation::calls++ == allocation::failAt) {
            allocation::enabled = false;
            throw std::bad_alloc();
        }
    }
    if (auto *p = std::malloc(count ? count : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t count) { return ::operator new(count); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(2); } } while (false)
using namespace iiSharedCanvas;

void negativeChunksAndConflicts(const std::filesystem::path &parent)
{
    iiFileProvider::native::TemporaryDirectory temporary{parent};
    CHECK(temporary.isValid());
    const auto path = (temporary.path() / "negative-chunks.iisc").string();
    Document document;
    document.extent = {128, 128};
    document.canvasMode = CanvasMode::Infinite;
    document.infiniteCanvas.origin = {-64, -64};
    document.infiniteCanvas.chunkSize = 32;
    document.assets.emplace_back(ChunkedRasterAsset{"paint", {{-1, -1, makeRasterLayer(32, 32)}}});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentFile file;
    CHECK(file.create(path, document).ok());
    ChunkedBitmapEditor editor(file, "paint");
    const std::array<std::uint32_t, 4> colors{0x00123456, 0xff223344, 0x80334455, 0xff445566};
    const std::array patch{BitmapPatch{{{-33, -33}, 2, 2}, colors}};
    CHECK(editor.replacePatches(patch));
    CHECK(findChunkedRasterAsset(*file.document(), "paint")->chunks.size() == 4);
    CHECK(editor.pixelAt(-33, -33) == colors[0] && editor.pixelAt(-32, -32) == colors[3]);
    CHECK(editor.undo());
    CHECK(findChunkedRasterAsset(*file.document(), "paint")->chunks.size() == 1);
    CHECK(editor.redo());
    CHECK(findChunkedRasterAsset(*file.document(), "paint")->chunks.size() == 4);
    CHECK(editor.beginPixelPreview("Move"));
    const std::array<std::uint32_t, 1> altered{0xffabcdee};
    const std::array preview{BitmapPatch{{{-33, -33}, 1, 1}, altered}};
    CHECK(editor.replacePixelPreview(preview) && editor.pixelAt(-33, -33) == altered[0]);
    CHECK(editor.cancelPixelPreview() && editor.pixelAt(-33, -33) == colors[0]);
    DocumentFile stale;
    CHECK(stale.open(path).ok());
    ChunkedBitmapEditor staleEditor(stale, "paint");
    CHECK(editor.replacePatches(preview));
    const auto staleRevision = stale.revision();
    CHECK(!staleEditor.replacePatches(preview));
    CHECK(stale.lastResult().code == DocumentFileCode::Conflict);
    CHECK(stale.revision() == staleRevision && staleEditor.historyStatistics().undoEntries == 0);
    stale.close();
    file.close();
    CHECK(file.open(path).ok());
    ChunkedBitmapEditor reopened(file, "paint");
    CHECK(reopened.pixelAt(-33, -33) == altered[0]);
    CHECK(reopened.pixelAt(-32, -32) == colors[3]);
    CHECK(findChunkedRasterAsset(*file.document(), "paint")->chunks.size() == 4);
    std::cout << "NEGATIVE_CHUNKS crossing=true zero_chunk_restored=true preview_cancel=true stale_conflict=true\n";
}

void allocationFailures(const std::filesystem::path &parent)
{
    iiFileProvider::native::TemporaryDirectory temporary{parent};
    CHECK(temporary.isValid());
    const auto path = (temporary.path() / "faults.iisc").string();
    Document document;
    document.extent = {64, 64};
    document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(64, 64, 0xff010203)});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentFile file;
    CHECK(file.create(path, document).ok());
    BitmapEditor editor(file, "paint");
    allocation::calls = 0;
    allocation::failAt = std::numeric_limits<std::size_t>::max();
    allocation::enabled = true;
    const bool initial = editor.setPixel(1, 1, 0xff123456);
    allocation::enabled = false;
    const auto observed = allocation::calls;
    CHECK(initial && observed > 10 && editor.undo());
    std::set<std::size_t> offsets{0, 1, 2, 3, observed - 1, observed, observed + 1024};
    for (std::size_t i = 1; i < 12; ++i) offsets.insert(observed * i / 12);
    std::size_t rejected = 0, accepted = 0;
    for (const auto offset : offsets) {
        const auto revision = file.revision();
        const auto author = file.document()->authorship.dump();
        const auto history = editor.historyStatistics();
        allocation::calls = 0;
        allocation::failAt = offset;
        allocation::enabled = true;
        bool applied = false;
        try { applied = editor.setPixel(1, 1, 0xff123456); }
        catch (const std::bad_alloc &) {}
        allocation::enabled = false;
        allocation::failAt = std::numeric_limits<std::size_t>::max();
        CHECK(file.isOpen() && editor.isBound());
        if (applied) {
            ++accepted;
            CHECK(file.revision() == revision + 1 && editor.pixelAt(1, 1) == 0xff123456);
            CHECK(editor.undo() && editor.pixelAt(1, 1) == 0xff010203);
        } else {
            ++rejected;
            CHECK(file.revision() == revision && editor.pixelAt(1, 1) == 0xff010203);
            CHECK(file.document()->authorship.dump() == author);
            const auto after = editor.historyStatistics();
            CHECK(after.undoEntries == history.undoEntries && after.redoEntries == history.redoEntries);
            Document snapshot;
            CHECK(DocumentFile::readSnapshot(path, snapshot).ok());
            CHECK(findRasterAsset(snapshot, "paint")->pixels.pixels[65] == 0xff010203);
            CHECK(snapshot.authorship.dump() == author);
        }
    }
    CHECK(rejected > 0 && accepted > 0);
    std::cout << "ALLOCATION_FAILURES measured_calls=" << observed << " injected=" << offsets.size()
              << " rejected=" << rejected << " accepted=" << accepted << " atomic=true\n";
}

void largeExistingChunk(const std::filesystem::path &parent)
{
    iiFileProvider::native::TemporaryDirectory temporary{parent};
    CHECK(temporary.isValid());
    const auto path = (temporary.path() / "large-chunk.iisc").string();
    Document document;
    document.extent = {4096, 4096};
    document.canvasMode = CanvasMode::Infinite;
    document.infiniteCanvas.chunkSize = 4096;
    document.assets.emplace_back(ChunkedRasterAsset{"paint", {{0, 0, makeRasterLayer(4096, 4096)}}});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentFile file;
    CHECK(file.create(path, document).ok());
    document = {};
    ChunkedBitmapEditor editor(file, "paint");
    const std::array<std::uint32_t, 1> pixel{0x00123456};
    const std::array patch{BitmapPatch{{{2050, 2050}, 1, 1}, pixel}};
    allocation::largest = allocation::total = 0;
    allocation::enabled = true;
    const bool changed = editor.replacePatches(patch);
    const bool undone = editor.undo();
    const bool redone = editor.redo();
    allocation::enabled = false;
    CHECK(changed && undone && redone && file.revision() == 3);
    CHECK(editor.pixelAt(2050, 2050) == pixel[0]);
    std::cout << "EXISTING_CHUNK_ALLOCATION source_bytes=" << 4096ULL * 4096 * 4
              << " operations=3 largest_cpp=" << allocation::largest
              << " total_cpp=" << allocation::total << '\n';
    CHECK(allocation::largest < 1024U * 1024U && allocation::total < 8U * 1024U * 1024U);
    file.close();
    CHECK(file.open(path).ok());
    ChunkedBitmapEditor reopened(file, "paint");
    CHECK(reopened.pixelAt(2050, 2050) == pixel[0]);
}

int main(int argc, char **argv)
{
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary{parent};
    CHECK(temporary.isValid());
    const auto path = (temporary.path() / "regions.iisc").string();
    const bool large = argc == 2 && std::string_view(argv[1]) == "--large";
    const int side = large ? 4096 : 512;
    Document document;
    document.extent = {side, side};
    document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(side, side, 0x804488cc)});
    document.assets.emplace_back(RasterAsset{"background", makeRasterLayer(side, side, 0xff223344)});
    document.assets.emplace_back(RasterAsset{"mask", makeRasterLayer(side, side, 0xff667788)});
    document.layers.emplace_back(StaticBitmapLayer{{"paint-layer", "Paint"}, StaticSource{"paint"}});
    layerProperties(document.layers.front()).appearance.rasterMask = LayerMask{.assetId = "mask"};
    document.layers.emplace_back(StaticBitmapLayer{{"background-layer", "Background"}, StaticSource{"background"}});
    DocumentFile file;
    const auto created = file.create(path, document);
    if (!created.ok()) std::cerr << created.message << '\n';
    CHECK(created.ok());
    document = {};
    BitmapEditor editor(file, "paint");
    const std::array<std::uint32_t, 1> color{0xff123456};
    const int coordinate = side / 2;
    const std::array patch{BitmapPatch{{{coordinate, coordinate}, 1, 1}, color}};
    allocation::largest = allocation::total = allocation::calls = 0;
    allocation::enabled = true;
    const bool changed = editor.replacePatches(patch);
    const bool undone = editor.undo();
    const bool redone = editor.redo();
    allocation::enabled = false;
    CHECK(changed && undone && redone && file.revision() == 3);
    CHECK(editor.pixelAt(coordinate, coordinate) == color[0]);
    CHECK(findRasterAsset(*file.document(), "background")->pixels.pixels.front() == 0xff223344);
    CHECK(findRasterAsset(*file.document(), "mask")->pixels.pixels.back() == 0xff667788);
    file.close();
    CHECK(file.open(path).ok());
    BitmapEditor reopened(file, "paint");
    CHECK(reopened.pixelAt(coordinate, coordinate) == color[0]);
    CHECK(findRasterAsset(*file.document(), "background")->pixels.pixels.front() == 0xff223344);
    CHECK(findRasterAsset(*file.document(), "mask")->pixels.pixels.back() == 0xff667788);
    std::cout << "FILE_REGION_ALLOCATION source_bytes=" << 3ULL * side * side * 4
              << " operations=3 largest_cpp=" << allocation::largest
              << " total_cpp=" << allocation::total << " persistence=true undo_redo=true\n";
    const bool bounded = large
        ? allocation::largest < 1024U * 1024U && allocation::total < 8U * 1024U * 1024U
        : allocation::largest < 256U * 1024U && allocation::total < 2U * 1024U * 1024U;
    if (!bounded) std::cerr << "small file edits allocated whole bitmap assets\n";
    if (!bounded) return 1;
    if (!large) {
        negativeChunksAndConflicts(parent);
        allocationFailures(parent);
        largeExistingChunk(parent);
    }
    return 0;
}
