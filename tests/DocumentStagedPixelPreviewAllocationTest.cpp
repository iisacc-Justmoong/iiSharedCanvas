#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderNative.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <string_view>

namespace fault {
thread_local std::ptrdiff_t remaining = -1;
thread_local bool fired = false;
}
void *operator new(std::size_t n) {
    if (fault::remaining == 0) { fault::remaining = -1; fault::fired = true; throw std::bad_alloc(); }
    if (fault::remaining > 0) --fault::remaining;
    if (auto *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, const char *message) { if (!value) { ++failures; std::cerr << message << '\n'; } }
Document fixture(bool grow) {
    Document d; d.extent = {32, 32};
    if (grow) { d.canvasMode = CanvasMode::Infinite; d.infiniteCanvas = {{0, 0}, 32};
                d.assets.emplace_back(ChunkedRasterAsset{"paint", {}}); }
    else d.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(32, 32)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return d;
}
std::uint64_t overlayHash(const DocumentHistory &h) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto &tile : h.stagedPixelPreviewTiles()) {
        for (auto n : {tile.bounds.origin.x, tile.bounds.origin.y, tile.bounds.width, tile.bounds.height}) {
            hash ^= std::uint32_t(n); hash *= 1099511628211ULL;
        }
        for (auto pixel : tile.pixels) { hash ^= pixel; hash *= 1099511628211ULL; }
    }
    return hash;
}
bool equal(DevicePixelRect a, DevicePixelRect b) {
    return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.width == b.width && a.height == b.height;
}
enum class Operation { Begin, Replace, Commit };
void memory(bool grow, Operation operation) {
    std::size_t rejected = 0, accepted = 0;
    for (std::ptrdiff_t position = 0; position < 512; ++position) {
        auto d = fixture(grow); DocumentHistory h(d);
        expect(h.setLayerName("layer", "Ink").changed && h.undo().changed, "memory allocation: establish redo");
        const std::array<std::uint32_t, 1> a{0xff112233}, b{0xffaabbcc};
        const std::array first{BitmapPatch{{{grow ? -3 : 3, grow ? -3 : 3}, 1, 1}, a}},
                         second{BitmapPatch{{{grow ? 40 : 7, grow ? 40 : 7}, 1, 1}, b}};
        const auto mode = grow ? PixelPreviewCanvasMode::Grow : PixelPreviewCanvasMode::Clip;
        if (operation != Operation::Begin)
            expect(h.beginStagedPixelPreview("paint", mode).ok() && h.replacePixelPreview(first).changed,
                   "memory allocation: establish preview");
        h.clearPreviewDirtyBounds();
        const auto stable = encodeIisc(d).bytes; const auto revision = d.authorship.revision();
        const auto stats = h.statistics(); const auto cache = h.previewStorageBytes();
        const auto pending = h.previewPendingHistoryBytes(); const auto region = h.stagedPixelPreviewRegion();
        const auto hash = overlayHash(h); const auto dirty = h.previewDirtyBounds();
        fault::remaining = position; fault::fired = false;
        DocumentHistoryResult result;
        try {
            if (operation == Operation::Begin) result = h.beginStagedPixelPreview("paint", mode);
            else if (operation == Operation::Replace) result = h.replacePixelPreview(second);
            else result = h.commitPixelPreview();
        } catch (const std::bad_alloc &) { result.code = DocumentHistoryCode::LimitExceeded; }
        fault::remaining = -1;
        if (!result.ok()) {
            ++rejected; const auto now = h.statistics();
            expect(fault::fired && encodeIisc(d).bytes == stable && d.authorship.revision() == revision &&
                   now.undoEntries == stats.undoEntries && now.redoEntries == stats.redoEntries &&
                   now.retainedBytes == stats.retainedBytes && h.previewStorageBytes() == cache &&
                   h.previewPendingHistoryBytes() == pending && h.stagedPixelPreviewRegion() == region &&
                   overlayHash(h) == hash && equal(h.previewDirtyBounds(), dirty) &&
                   h.pixelPreviewActive() == (operation != Operation::Begin),
                   "memory allocation: failure preserves owner, staged pixels, geometry and cursor");
        } else {
            ++accepted;
            expect(operation == Operation::Begin ? h.pixelPreviewActive() : result.changed,
                   "memory allocation: success publishes the intended operation");
        }
        if (h.pixelPreviewActive()) {
            fault::remaining = 0; fault::fired = false;
            const auto cancelled = h.cancelPixelPreview();
            fault::remaining = -1;
            expect(cancelled.ok() && !fault::fired && h.canRedo(),
                   "memory allocation: staged cancel needs no allocation and preserves redo");
        } else if (operation == Operation::Commit && result.ok()) {
            expect(h.undo().changed && h.redo().changed, "memory allocation: committed entry is reversible");
        } else expect(h.canRedo(), "memory allocation: redo survives failed begin");
    }
    expect(rejected >= 5 && accepted > 0, "memory allocation: covered failures and successes");
    std::cout << "STAGED_MEMORY_ALLOCATION grow=" << grow << " operation=" << int(operation)
              << " rejected=" << rejected << " accepted=" << accepted << '\n' << std::flush;
}
void file(bool grow) {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory temp(parent);
    if (!temp.isValid()) { expect(false, "file allocation: temporary directory"); return; }
    constexpr std::array<std::ptrdiff_t, 10> positions{0, 1, 4, 10, 30, 70, 150, 256, 512, 1024};
    std::size_t rejected = 0, accepted = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const auto path = (temp.path() / (std::to_string(i) + ".iisc")).string();
        DocumentFile owner;
        if (!owner.create(path, fixture(grow)).ok()) { expect(false, "file allocation: create"); return; }
        DocumentHistory h(owner); const std::array<std::uint32_t, 1> color{0xffaabbcc};
        const std::array patch{BitmapPatch{{{grow ? -3 : 3, grow ? -3 : 3}, 1, 1}, color}};
        const auto mode = grow ? PixelPreviewCanvasMode::Grow : PixelPreviewCanvasMode::Clip;
        expect(h.setLayerName("layer", "Ink").changed && h.undo().changed &&
               h.beginStagedPixelPreview("paint", mode).ok() && h.replacePixelPreview(patch).changed,
               "file allocation: establish preview and redo");
        const auto stable = encodeIisc(*owner.document()).bytes; const auto revision = owner.revision();
        const auto stats = h.statistics(); const auto cache = h.previewStorageBytes();
        const auto pending = h.previewPendingHistoryBytes(); const auto region = h.stagedPixelPreviewRegion();
        const auto hash = overlayHash(h);
        fault::remaining = positions[i]; fault::fired = false;
        DocumentHistoryResult result;
        try { result = h.commitPixelPreview(); }
        catch (const std::bad_alloc &) { result.code = DocumentHistoryCode::LimitExceeded; }
        fault::remaining = -1;
        Document snapshot; expect(DocumentFile::readSnapshot(path, snapshot).ok(), "file allocation: reopen snapshot");
        if (!result.ok()) {
            ++rejected; const auto now = h.statistics();
            expect(fault::fired && encodeIisc(snapshot).bytes == stable &&
                   now.undoEntries == stats.undoEntries && now.redoEntries == stats.redoEntries &&
                   now.retainedBytes == stats.retainedBytes && h.previewStorageBytes() == cache &&
                   h.previewPendingHistoryBytes() == pending && h.stagedPixelPreviewRegion() == region &&
                   overlayHash(h) == hash && h.pixelPreviewActive() &&
                   (!owner.isOpen() || (owner.revision() == revision && encodeIisc(*owner.document()).bytes == stable)),
                   "file allocation: failed commit preserves durable and staged state");
            expect(h.cancelPixelPreview().changed && !h.pixelPreviewActive(),
                   "file allocation: failed commit can always be cancelled");
        } else {
            ++accepted;
            expect(result.changed && owner.revision() == revision + 1 && h.undo().changed && h.redo().changed,
                   "file allocation: successful commit is one reversible revision");
        }
    }
    expect(rejected > 0 && accepted > 0, "file allocation: covered failures and successes");
    std::cout << "STAGED_FILE_ALLOCATION grow=" << grow << " rejected=" << rejected
              << " accepted=" << accepted << '\n' << std::flush;
}
}
int main(int argc, char **argv) {
    const bool memoryOnly = argc == 2 && std::string_view(argv[1]) == "--memory-only";
    const bool fileOnly = argc == 2 && std::string_view(argv[1]) == "--file-only";
    if (argc > 1 && !memoryOnly && !fileOnly) {
        std::cerr << "expected --memory-only or --file-only\n";
        return 2;
    }
    for (bool grow : {false, true}) {
        if (!fileOnly)
            for (auto operation : {Operation::Begin, Operation::Replace, Operation::Commit}) memory(grow, operation);
        if (!memoryOnly) file(grow);
    }
    return failures ? 1 : 0;
}
