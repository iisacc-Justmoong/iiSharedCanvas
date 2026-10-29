#include <File/DocumentFile.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderNative.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <string>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define IISHAREDCANVAS_ADDRESS_SANITIZED_TEST 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(IISHAREDCANVAS_ADDRESS_SANITIZED_TEST)
#define IISHAREDCANVAS_ADDRESS_SANITIZED_TEST 1
#endif
#ifndef IISHAREDCANVAS_ADDRESS_SANITIZED_TEST
#define IISHAREDCANVAS_ADDRESS_SANITIZED_TEST 0
#endif

namespace allocation {
thread_local bool enabled = false;
thread_local std::size_t largest = 0, total = 0;
thread_local std::size_t calls = 0, failAt = std::numeric_limits<std::size_t>::max();
}

void *operator new(std::size_t size)
{
    if (allocation::enabled) {
        allocation::largest = std::max(allocation::largest, size);
        allocation::total += size;
        if (allocation::calls++ == allocation::failAt) {
            allocation::enabled = false;
            throw std::bad_alloc();
        }
    }
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __LINE__ << ": " #condition "\n"; return 1; \
} } while (false)

int main()
{
    using namespace iiSharedCanvas;
    constexpr int side = 1024;
    constexpr std::size_t pixelBytes = 3ULL * side * side * sizeof(std::uint32_t);
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    CHECK(temporary.isValid());
    const auto path = (temporary.path() / "snapshot.iisc").string();
    const auto replacementPath = (temporary.path() / "replacement.iisc").string();
    Document original;
    original.extent = {side, side};
    for (const auto *id : {"paint", "background", "mask"})
        original.assets.emplace_back(RasterAsset{id, makeRasterLayer(side, side)});
    findRasterAsset(original, "paint")->pixels.pixels[1] = 0x00123456;
    findRasterAsset(original, "background")->pixels.pixels[side + 2] = 0xff334455;
    findRasterAsset(original, "mask")->pixels.pixels.back() = 0xff667788;
    original.layers.emplace_back(StaticBitmapLayer{{"paint-layer", "Paint"}, StaticSource{"paint"}});
    layerProperties(original.layers.front()).appearance.rasterMask = LayerMask{.assetId = "mask"};
    original.layers.emplace_back(StaticBitmapLayer{{"background-layer", "Background"},
                                                    StaticSource{"background"}});
    original.publicationMetadata.emplace();
    original.publicationMetadata->title = "Snapshot";
    original.publicationMetadata->settings.emplace("published", true);
    DocumentFile file;
    CHECK(file.create(path, original).ok());
    file.close();
    findRasterAsset(original, "paint")->pixels.pixels[1] = 0xffaa7733;
    CHECK(file.create(replacementPath, original).ok());
    file.close();
    original = {};
    const auto beforeHash = iiFileProvider::native::sha256(iiFileProvider::native::File::read(path));
    Document snapshot;
    allocation::largest = allocation::total = allocation::calls = 0;
    allocation::enabled = true;
    const auto result = DocumentFile::readSnapshot(path, snapshot);
    allocation::enabled = false;
    const auto successfulCalls = allocation::calls;
    CHECK(result.ok());
    CHECK(findRasterAsset(snapshot, "paint")->pixels.pixels[1] == 0x00123456);
    CHECK(findRasterAsset(snapshot, "background")->pixels.pixels[side + 2] == 0xff334455);
    CHECK(findRasterAsset(snapshot, "mask")->pixels.pixels.back() == 0xff667788);
    CHECK(layerProperties(snapshot.layers.front()).appearance.rasterMask->assetId == "mask");
    CHECK(snapshot.publicationMetadata && snapshot.publicationMetadata->title == "Snapshot");
    CHECK(iiFileProvider::native::sha256(iiFileProvider::native::File::read(path)) == beforeHash);
    std::cout << "WORKING_SNAPSHOT_ALLOCATION source_pixels=" << pixelBytes
              << " largest_cpp=" << allocation::largest << " total_cpp=" << allocation::total
              << " output_and_source_verified=true measurement="
              << (IISHAREDCANVAS_ADDRESS_SANITIZED_TEST ? "asan-excluded" : "enabled") << '\n';
    if constexpr (!IISHAREDCANVAS_ADDRESS_SANITIZED_TEST) {
        const bool bounded = allocation::total < pixelBytes + pixelBytes / 2;
        if (!bounded) std::cerr << "working snapshot allocated an extra complete document\n";
        if (!bounded) return 1;
    }
    SerializationLimits limits;
    limits.maximumContainerBytes = 1;
    CHECK(!DocumentFile::readSnapshot(path, snapshot, limits).ok());
    CHECK(findRasterAsset(snapshot, "paint")->pixels.pixels[1] == 0x00123456);
    if constexpr (!IISHAREDCANVAS_ADDRESS_SANITIZED_TEST) {
        for (const auto failAt : {std::size_t{0}, successfulCalls / 4, successfulCalls / 2}) {
            allocation::calls = 0;
            allocation::failAt = failAt;
            allocation::enabled = true;
            const auto failed = DocumentFile::readSnapshot(path, snapshot);
            allocation::enabled = false;
            allocation::failAt = std::numeric_limits<std::size_t>::max();
            CHECK(!failed.ok());
            CHECK(findRasterAsset(snapshot, "paint")->pixels.pixels[1] == 0x00123456);
            CHECK(snapshot.publicationMetadata && snapshot.publicationMetadata->title == "Snapshot");
        }
    }
    CHECK(iiFileProvider::native::sha256(iiFileProvider::native::File::read(path)) == beforeHash);

    namespace sql = iiFileProvider::native::sql;
    auto database = sql::Database::open(path);
    CHECK(database.has_value());
    CHECK(database->execute("PRAGMA journal_mode=WAL").has_value());
    CHECK(database->execute("PRAGMA wal_autocheckpoint=0").has_value());
    auto attach = database->prepare("ATTACH DATABASE ? AS changed");
    CHECK(attach.has_value() && attach->bind(1, replacementPath).has_value());
    CHECK(attach->step().has_value());
    auto transaction = sql::Transaction::begin(*database);
    CHECK(transaction.has_value());
    for (const auto *statement : {
        "DELETE FROM main.canvas_records",
        "INSERT INTO main.canvas_records SELECT * FROM changed.canvas_records",
        "DELETE FROM main.canvas_tiles",
        "INSERT INTO main.canvas_tiles SELECT * FROM changed.canvas_tiles",
        "UPDATE main.canvas_tile_state SET tile_count=(SELECT tile_count FROM changed.canvas_tile_state)",
        "UPDATE main.canvas_state SET revision=revision+1"})
        CHECK(database->execute(statement).has_value());
    CHECK(transaction->commit().has_value());
    const auto walPath = path + "-wal";
    const auto beforeWal = iiFileProvider::native::File::read(walPath);
    CHECK(!beforeWal.empty());
    const auto mainBytes = iiFileProvider::native::File::read(path);
    CHECK(DocumentFile::readSnapshot(path, snapshot).ok());
    CHECK(findRasterAsset(snapshot, "paint")->pixels.pixels[1] == 0xffaa7733);
    CHECK(snapshot.publicationMetadata && snapshot.publicationMetadata->title == "Snapshot");
    CHECK(iiFileProvider::native::File::read(path) == mainBytes);
    CHECK(iiFileProvider::native::File::read(walPath) == beforeWal);
    std::cout << "WORKING_SNAPSHOT_FAILURES count="
              << (IISHAREDCANVAS_ADDRESS_SANITIZED_TEST ? 0 : 3)
              << " output_preserved=true wal_committed_content=true source_unchanged=true"
              << " allocation_checks="
              << (IISHAREDCANVAS_ADDRESS_SANITIZED_TEST ? "asan-excluded" : "enabled") << '\n';
    return 0;
}
