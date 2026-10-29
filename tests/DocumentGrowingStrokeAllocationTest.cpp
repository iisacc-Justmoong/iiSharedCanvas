#include <Bitmap/BitmapEditor.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderNative.h>
#include "AllocationTestSelection_p.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
#include <vector>

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
void expect(bool value, const char *message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
Document fixture() {
    Document d; d.extent = {32, 32}; d.canvasMode = CanvasMode::Infinite;
    d.infiniteCanvas = {{0, 0}, 32};
    d.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return d;
}
BitmapBrush brush() { BitmapBrush b; b.size = 5; b.argb = 0xff336699; return b; }
StrokePoint point(double x, double y, double time) {
    StrokePoint p; p.position = {x, y}; p.time = time; return p;
}
enum class Operation { Begin, Continue, End, Cancel, Undo, Redo };
enum class FileGroup { All, Control, Lower, Upper };
DocumentHistoryResult invoke(DocumentHistory &h, Operation operation) {
    switch (operation) {
    case Operation::Begin: return h.beginStroke("paint", brush(), point(-8, -8, 1), StrokeCanvasMode::Grow);
    case Operation::Continue: return h.continueStroke(point(40, 40, 2));
    case Operation::End: return h.endStroke(point(40, 40, 2));
    case Operation::Cancel: return h.cancelStroke();
    case Operation::Undo: return h.undo();
    case Operation::Redo: return h.redo();
    }
    return {DocumentHistoryCode::InvalidArgument};
}
bool sameBounds(DevicePixelRect a, DevicePixelRect b) {
    return a.origin.x == b.origin.x && a.origin.y == b.origin.y &&
           a.width == b.width && a.height == b.height;
}
void prepare(DocumentHistory &h, Operation operation) {
    expect(h.setLayerName("layer", "Ink").changed && h.undo().changed, "allocation: establish redo");
    if (operation != Operation::Begin)
        expect(h.beginStroke("paint", brush(), point(-8, -8, 1), StrokeCanvasMode::Grow).changed,
               "allocation: establish growing stroke");
    if (operation == Operation::Undo || operation == Operation::Redo)
        expect(h.endStroke(point(40, 40, 2)).changed, "allocation: complete growing stroke");
    if (operation == Operation::Redo) expect(h.undo().changed, "allocation: establish redo stroke");
}
void verifyOutcome(DocumentHistory &h, Operation operation, bool accepted) {
    if (h.strokeActive()) expect(h.cancelStroke().ok(), "allocation: active stroke can be cancelled");
    if (operation == Operation::Begin || operation == Operation::Continue || operation == Operation::Cancel ||
        (operation == Operation::End && !accepted))
        expect(h.canRedo(), "allocation: redo survives rejection or cancellation");
    else if (operation == Operation::End && accepted)
        expect(h.undo().changed && h.redo().changed, "allocation: completed stroke remains reversible");
    else if (operation == Operation::Undo)
        expect(accepted ? h.redo().changed : h.undo().changed, "allocation: undo remains retryable");
    else if (operation == Operation::Redo)
        expect(accepted ? h.undo().changed : h.redo().changed, "allocation: redo remains retryable");
}
void memory(Operation operation) {
    std::size_t rejected = 0, accepted = 0;
    for (std::ptrdiff_t offset = 0; offset < 768; ++offset) {
        auto d = fixture(); DocumentHistory h(d); prepare(h, operation);
        const auto stable = encodeIisc(d).bytes; const auto revision = d.authorship.revision();
        const auto stats = h.statistics(); const auto dirty = h.strokeDirtyBounds();
        const auto cache = h.strokeStorageBytes(), pending = h.strokePendingHistoryBytes();
        const bool wasActive = h.strokeActive();
        fault::remaining = offset; fault::fired = false;
        DocumentHistoryResult result;
        try { result = invoke(h, operation); }
        catch (const std::bad_alloc &) { result.code = DocumentHistoryCode::LimitExceeded; }
        fault::remaining = -1;
        if (!result.ok()) {
            ++rejected; const auto now = h.statistics();
            expect(fault::fired && encodeIisc(d).bytes == stable && d.authorship.revision() == revision &&
                   now.undoEntries == stats.undoEntries && now.redoEntries == stats.redoEntries &&
                   now.retainedBytes == stats.retainedBytes && h.strokeStorageBytes() == cache &&
                   h.strokePendingHistoryBytes() == pending && sameBounds(h.strokeDirtyBounds(), dirty) &&
                   h.strokeActive() == wasActive,
                   "allocation: failed operation preserves document, geometry, session and cursor");
        } else { ++accepted; expect(result.changed, "allocation: successful operation changes state"); }
        verifyOutcome(h, operation, result.ok());
    }
    expect(rejected >= 5 && accepted > 0, "allocation: exercised rejected and accepted positions");
    std::cout << "GROWING_MEMORY_ALLOCATION operation=" << int(operation)
              << " rejected=" << rejected << " accepted=" << accepted << '\n' << std::flush;
}
void file(Operation operation, FileGroup group) {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false, "file allocation: temporary directory"); return; }
    constexpr std::array<std::ptrdiff_t, 9> positions{0, 1, 4, 10, 30, 70, 150, 384, 768};
    std::size_t rejected = 0, accepted = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (group == FileGroup::Control && i != 0 && i != 1 && i != 8) continue;
        if (group == FileGroup::Lower && (i < 2 || i > 4)) continue;
        if (group == FileGroup::Upper && (i < 5 || i > 7)) continue;
        const auto path = (temporary.path() / (std::to_string(i) + ".iisc")).string();
        DocumentFile owner;
        if (!owner.create(path, fixture()).ok()) { expect(false, "file allocation: create"); return; }
        DocumentHistory h(owner); prepare(h, operation);
        const auto stable = encodeIisc(*owner.document()).bytes; const auto revision = owner.revision();
        const auto stats = h.statistics(); const auto dirty = h.strokeDirtyBounds();
        const auto cache = h.strokeStorageBytes(), pending = h.strokePendingHistoryBytes();
        const bool wasActive = h.strokeActive();
        fault::remaining = positions[i]; fault::fired = false;
        DocumentHistoryResult result;
        try { result = invoke(h, operation); }
        catch (const std::bad_alloc &) { result.code = DocumentHistoryCode::LimitExceeded; }
        fault::remaining = -1;
        Document snapshot;
        expect(DocumentFile::readSnapshot(path, snapshot).ok(), "file allocation: inspect durable state");
        if (!result.ok()) {
            ++rejected; const auto now = h.statistics();
            expect(fault::fired && encodeIisc(snapshot).bytes == stable &&
                   now.undoEntries == stats.undoEntries && now.redoEntries == stats.redoEntries &&
                   now.retainedBytes == stats.retainedBytes && h.strokeStorageBytes() == cache &&
                   h.strokePendingHistoryBytes() == pending && sameBounds(h.strokeDirtyBounds(), dirty) &&
                   (!owner.isOpen() || (owner.revision() == revision && encodeIisc(*owner.document()).bytes == stable)) &&
                   h.strokeActive() == wasActive,
                   "file allocation: failure preserves durable region, pixels, session and cursor");
        } else { ++accepted; expect(result.changed, "file allocation: successful operation changes state"); }
        if (owner.isOpen()) verifyOutcome(h, operation, result.ok());
        else if (h.strokeActive()) expect(h.abandonStroke().ok(), "file allocation: release closed binding");
    }
    if (group == FileGroup::All || group == FileGroup::Control)
        expect(rejected > 0 && accepted > 0, "file allocation: exercised rejected and accepted positions");
    if (group != FileGroup::All)
        expect(rejected + accepted == 3, "file allocation: exercised every selected position");
    std::cout << "GROWING_FILE_ALLOCATION operation=" << int(operation)
              << " group=" << int(group) << " rejected=" << rejected
              << " accepted=" << accepted << '\n' << std::flush;
}
}
int main(int argc, char **argv) {
    constexpr std::array<std::string_view, 6> names{"begin", "continue", "end", "cancel", "undo", "redo"};
    constexpr std::array operations{Operation::Begin, Operation::Continue, Operation::End,
                                    Operation::Cancel, Operation::Undo, Operation::Redo};
    FileGroup group = FileGroup::All;
    bool groupSelected = false;
    std::vector<char *> arguments{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        constexpr std::string_view prefix = "--allocation-group=";
        if (!argument.starts_with(prefix)) { arguments.push_back(argv[i]); continue; }
        if (groupSelected) { std::cerr << "invalid allocation test arguments\n"; return 2; }
        groupSelected = true;
        const auto value = argument.substr(prefix.size());
        if (value == "control") group = FileGroup::Control;
        else if (value == "lower") group = FileGroup::Lower;
        else if (value == "upper") group = FileGroup::Upper;
        else { std::cerr << "invalid allocation test arguments\n"; return 2; }
    }
    const auto selection = allocation_test::parse(static_cast<int>(arguments.size()), arguments.data(), names);
    if (!selection || (groupSelected && (selection->memory || !selection->file || !selection->operation ||
            (*selection->operation != 4 && *selection->operation != 5)))) {
        std::cerr << "invalid allocation test arguments\n"; return 2;
    }
    for (std::size_t i = 0; i < operations.size(); ++i) {
        if (selection->operation && *selection->operation != i) continue;
        if (selection->memory) memory(operations[i]);
        if (selection->file) file(operations[i], group);
    }
    return failures ? 1 : 0;
}
