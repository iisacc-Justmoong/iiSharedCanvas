#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>

namespace allocation {
thread_local bool active = false;
thread_local std::size_t bytes = 0;
thread_local std::ptrdiff_t fail_after = -1;
thread_local bool failed = false;
}
void *operator new(std::size_t size) {
    if (allocation::fail_after == 0) {
        allocation::fail_after = -1;
        allocation::failed = true;
        throw std::bad_alloc();
    }
    if (allocation::fail_after > 0) --allocation::fail_after;
    if (allocation::active) allocation::bytes += size;
    if (auto *result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete[](void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { std::free(pointer); }

using namespace iiSharedCanvas;
namespace {
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template <class Function> void measure(const char *stage, Function operation) {
    allocation::bytes = 0;
    allocation::active = true;
    const auto start = std::chrono::steady_clock::now();
    bool ok = false;
    try { ok = operation(); } catch (...) { allocation::active = false; throw; }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    allocation::active = false;
    check(ok, "history publication failed");
    std::cout << "HISTORY_PUBLICATION_8K stage=" << stage << " us=" << elapsed
              << " allocated_bytes=" << allocation::bytes << '\n';
    check(allocation::bytes < 8 * 1024 * 1024,
          "small history publication allocated an entire large raster");
}
void allocationFailures(const std::filesystem::path &path) {
    Document source; source.extent = {128, 128};
    source.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(128, 128)});
    source.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentFile file;
    check(file.create(path.string(), source).ok(), "fault fixture file setup failed");
    BitmapEditor editor(source, "paint");
    const auto before = source.authorship;
    const auto versionBefore = source.formatVersion;
    const auto original = encodeIisc(source).bytes;
    check(editor.setPixel(32, 32, 0xff112233), "fault fixture edit failed");
    const auto after = source.authorship;
    const auto versionAfter = source.formatVersion;
    const auto painted = encodeIisc(source).bytes;
    std::size_t rejected = 0, succeeded = 0;
    for (std::ptrdiff_t position = 0; position < 256 && succeeded < 3; ++position) {
        const auto revision = file.revision();
        allocation::failed = false;
        allocation::fail_after = position;
        bool published = false;
        try {
            published = editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                                   before, versionBefore, revision);
        } catch (const std::bad_alloc &) {}
        allocation::fail_after = -1;
        if (!published) {
            ++rejected;
            check(allocation::failed && file.revision() == revision && editor.canUndo() &&
                  encodeIisc(*file.document()).bytes == original && encodeIisc(source).bytes == painted,
                  "allocation failure changed owner, source or history");
            if (position % 16 == 0) {
                Document reopened;
                check(DocumentFile::readSnapshot(file.filePath(), reopened).ok() &&
                      encodeIisc(reopened).bytes == original,
                      "allocation failure changed durable pixels");
            }
        } else {
            ++succeeded;
            check(encodeIisc(*file.document()).bytes == painted && editor.canUndo(),
                  "successful fault trial did not commit exactly");
            check(editor.undo(), "fault trial reset undo failed");
            source.authorship = before; source.formatVersion = versionBefore;
            check(editor.publishHistoryState(file, BitmapHistoryState::Undone,
                                             after, versionAfter, file.revision()),
                  "fault trial reset publication failed");
            check(editor.redo(), "fault trial reset redo failed");
            source.authorship = after; source.formatVersion = versionAfter;
        }
    }
    check(rejected >= 20 && succeeded >= 1,
          "fault injection did not cover allocation failures and success");
    std::cout << "HISTORY_PUBLICATION_FAULTS rejected=" << rejected
              << " succeeded=" << succeeded << '\n';
}
}

int main() try {
    iiFileProvider::native::TemporaryDirectory directory(
        std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR));
    check(directory.isValid(), "temporary directory unavailable");
    allocationFailures(directory.path() / "faults.iisc");
    Document source; source.extent = {8192, 8192};
    source.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(8192, 8192)});
    source.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentFile file;
    check(file.create((directory.path() / "large.iisc").string(), source).ok(), "file setup failed");
    BitmapEditor editor(source, "paint");
    const auto before = source.authorship;
    const auto versionBefore = source.formatVersion;
    check(editor.setPixel(4000, 4000, 0xffabcdef), "source edit failed");
    const auto after = source.authorship;
    const auto versionAfter = source.formatVersion;
    measure("commit", [&] { return editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                        before, versionBefore, file.revision()); });
    check(editor.undo(), "source undo failed");
    source.authorship = before; source.formatVersion = versionBefore;
    measure("undo", [&] { return editor.publishHistoryState(file, BitmapHistoryState::Undone,
                                      after, versionAfter, file.revision()); });
    check(editor.redo(), "source redo failed");
    source.authorship = after; source.formatVersion = versionAfter;
    measure("redo", [&] { return editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                      before, versionBefore, file.revision()); });
    const auto *asset = findRasterAsset(*file.document(), "paint");
    check(asset && asset->pixels.pixels[std::size_t(4000) * 8192 + 4000] == 0xffabcdef,
          "large publication lost its changed pixel");
    return 0;
} catch (const std::exception &error) {
    allocation::active = false;
    std::cerr << error.what() << '\n';
    return 1;
}
