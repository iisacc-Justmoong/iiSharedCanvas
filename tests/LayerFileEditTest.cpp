#include <Bitmap/BitmapEditor.h>
#include <Document/DocumentEditor.h>
#include <File/DocumentFile.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderNative.h>
#include "LegacyWorkingFileFixture_p.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <set>

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
    if (auto *value = std::malloc(count ? count : 1)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t count) { return ::operator new(count); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

#define CHECK(value) do { if (!(value)) { std::cerr << __LINE__ << ": " #value "\n"; return 1; } } while (false)

int main()
{
    using namespace iiSharedCanvas;
    std::cout << std::unitbuf;
    constexpr int side = 1024;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    CHECK(temporary.isValid());
    const auto path = (temporary.path() / "layers.iisc").string();

    Document initial;
    initial.extent = {side, side};
    initial.timeline = {{24, 1}, 8};
    for (const auto *id : {"paint", "background", "mask"})
        initial.assets.emplace_back(RasterAsset{id, makeRasterLayer(side, side)});
    findRasterAsset(initial, "paint")->pixels.pixels[10] = 0x00123456;
    findRasterAsset(initial, "background")->pixels.pixels[20] = 0xff224466;
    findRasterAsset(initial, "mask")->pixels.pixels[30] = 0xff667788;
    initial.layers.emplace_back(StaticBitmapLayer{{"paint-layer", "Paint"}, StaticSource{"paint"}});
    layerProperties(initial.layers.front()).appearance.rasterMask = LayerMask{.assetId = "mask"};
    initial.layers.emplace_back(DynamicBitmapLayer{{"animated", "Animation"}, KeyframedSource{{0, 6}}});
    initial.frames = {{0, {{"animated", "paint"}}}, {6, {{"animated", "background"}}}};
    DocumentFile file;
    CHECK(file.create(path, initial).ok());
    initial = {};
    DocumentEditor editor(file);
    CHECK(editor.isBound());

    allocation::largest = allocation::total = 0;
    allocation::enabled = true;
    const auto renamed = editor.setLayerName("paint-layer", "Line art");
    allocation::enabled = false;
    CHECK(renamed.ok() && renamed.changed && file.revision() == 1);
    CHECK(layerProperties(*findLayer(*file.document(), "paint-layer")).name == "Line art");
    std::cout << "LAYER_EDIT_ALLOCATION source_pixels=" << 3ULL * side * side * 4
              << " largest_cpp=" << allocation::largest << " total_cpp=" << allocation::total
              << " measurement=" << (IISHAREDCANVAS_ADDRESS_SANITIZED_TEST ? "asan-excluded" : "enabled") << '\n';
    if constexpr (!IISHAREDCANVAS_ADDRESS_SANITIZED_TEST) {
        if (allocation::largest >= 256U * 1024 || allocation::total >= 1024U * 1024) {
            std::cerr << "layer metadata edit allocated whole raster content\n";
            return 1;
        }
    }

    const std::string longName = "Line art with a deliberately long and stable layer name";
    CHECK(editor.setLayerName("paint-layer", longName).changed);
    const auto originalNameData = layerProperties(*findLayer(*file.document(), "paint-layer")).name.data();
    const auto revision = file.revision();
    const auto author = authorshipDump(file.document()->authorship);
    CHECK(editor.setLayerName("paint-layer", longName).ok());
    CHECK(file.revision() == revision && authorshipDump(file.document()->authorship) == author);
    CHECK(!editor.setLayerOpacity("paint-layer", 1.5).ok());
    CHECK(!editor.setLayerBlendMode("paint-layer", RasterBlendMode::DestinationOut).ok());
    AffineTransform invalid;
    invalid.m11 = std::numeric_limits<double>::quiet_NaN();
    CHECK(!editor.setLayerTransform("paint-layer", invalid).ok());
    CHECK(!editor.setLayerName("paint-layer", std::string(1, char(0xff))).ok());
    CHECK(file.revision() == revision && authorshipDump(file.document()->authorship) == author);
    CHECK(layerProperties(*findLayer(*file.document(), "paint-layer")).name == longName);
    CHECK(layerProperties(*findLayer(*file.document(), "paint-layer")).name.data() == originalNameData);

    CHECK(editor.setLayerVisible("paint-layer", false).changed);
    CHECK(editor.setLayerOpacity("paint-layer", 0.5).changed);
    AffineTransform transform;
    transform.translationX = 17;
    transform.translationY = -9;
    CHECK(editor.setLayerTransform("paint-layer", transform).changed);
    CHECK(editor.setLayerBlendMode("paint-layer", RasterBlendMode::Multiply).changed);
    CHECK(editor.setLayerName("animated", "Animated cel").changed);
    const auto &properties = layerProperties(*findLayer(*file.document(), "paint-layer"));
    CHECK(!properties.visible && properties.opacity == 0.5 && properties.transform.translationX == 17);
    CHECK(properties.blendMode == RasterBlendMode::Multiply);
    CHECK(file.lastWriteStatistics().payloadBytesWritten < 64U * 1024);

    BitmapEditor bitmap(file, "paint");
    CHECK(bitmap.setPixel(23, 24, 0xff123456));
    CHECK(editor.setLayerName("paint-layer", "Final layer").changed);
    CHECK(bitmap.undo() && bitmap.pixelAt(23, 24) == 0);
    CHECK(bitmap.redo() && bitmap.pixelAt(23, 24) == 0xff123456);
    CHECK(layerProperties(*findLayer(*file.document(), "paint-layer")).name == "Final layer");

    allocation::calls = 0;
    allocation::enabled = true;
    const auto probe = editor.setLayerName("paint-layer", "Allocation probe");
    allocation::enabled = false;
    const auto calls = allocation::calls;
    CHECK(probe.changed);
    CHECK(editor.setLayerName("paint-layer", "Final layer").changed);
    std::size_t rejected = 0, accepted = 0, tested = 0;
    if constexpr (!IISHAREDCANVAS_ADDRESS_SANITIZED_TEST) {
        CHECK(calls > 10);
        std::set<std::size_t> offsets{0, 1, 2, 3, calls - 1, calls, calls + 1024};
        for (std::size_t i = 1; i < 12; ++i) offsets.insert(calls * i / 12);
        tested = offsets.size();
        for (const auto offset : offsets) {
            std::cout << "LAYER_EDIT_INJECT offset=" << offset << " calls=" << calls << '\n';
            const auto beforeRevision = file.revision();
            const auto beforeAuthor = authorshipDump(file.document()->authorship);
            allocation::calls = 0;
            allocation::failAt = offset;
            allocation::enabled = true;
            DocumentEditResult result;
            bool threw = false;
            try { result = editor.setLayerName("paint-layer", "Allocation probe"); }
            catch (const std::bad_alloc &) { threw = true; }
            allocation::enabled = false;
            allocation::failAt = std::numeric_limits<std::size_t>::max();
            CHECK(file.isOpen() && editor.isBound());
            if (!threw && result.ok() && result.changed) {
                ++accepted;
                CHECK(file.revision() == beforeRevision + 1);
                CHECK(layerProperties(*findLayer(*file.document(), "paint-layer")).name == "Allocation probe");
                CHECK(editor.setLayerName("paint-layer", "Final layer").changed);
            } else {
                ++rejected;
                CHECK(file.revision() == beforeRevision);
                CHECK(authorshipDump(file.document()->authorship) == beforeAuthor);
                CHECK(layerProperties(*findLayer(*file.document(), "paint-layer")).name == "Final layer");
            }
        }
        CHECK(rejected > 0 && accepted > 0);
    }
    std::cout << "LAYER_EDIT_ALLOCATION_FAILURES observed=" << calls << " tested=" << tested
              << " rejected=" << rejected << " accepted=" << accepted
              << " checks=" << (IISHAREDCANVAS_ADDRESS_SANITIZED_TEST ? "asan-excluded" : "enabled") << '\n';

    DocumentFile stale;
    CHECK(stale.open(path).ok());
    DocumentEditor staleEditor(stale);
    const auto staleRevision = stale.revision();
    CHECK(editor.setLayerVisible("paint-layer", true).changed);
    CHECK(!staleEditor.setLayerName("paint-layer", "Final layer").ok());
    CHECK(stale.lastResult().code == DocumentFileCode::Conflict && stale.revision() == staleRevision);
    CHECK(!stale.isOpen() && !staleEditor.isBound());
    CHECK(layerProperties(*findLayer(*file.document(), "paint-layer")).visible);
    stale.close();
    Document snapshot;
    CHECK(DocumentFile::readSnapshot(path, snapshot).ok());
    CHECK(layerProperties(*findLayer(snapshot, "paint-layer")).name == "Final layer");
    CHECK(layerProperties(*findLayer(snapshot, "animated")).name == "Animated cel");
    CHECK(findKeyframe(snapshot, "animated", 6)->assetId == "background");
    CHECK(findRasterAsset(snapshot, "paint")->pixels.pixels[24 * side + 23] == 0xff123456);
    CHECK(findRasterAsset(snapshot, "mask")->pixels.pixels[30] == 0xff667788);
    CHECK(layerProperties(*findLayer(snapshot, "paint-layer")).appearance.rasterMask->assetId == "mask");

    Document legacy;
    legacy.formatVersion = {CurrentFormatMajor, CurrentFormatMinor - 1};
    legacy.extent = {16, 16};
    legacy.assets.emplace_back(RasterAsset{"legacy", makeRasterLayer(16, 16)});
    legacy.layers.emplace_back(StaticBitmapLayer{{"old-layer", "Old"}, StaticSource{"legacy"}});
    const auto legacyPath = (temporary.path() / "old-model.iisc").string();
    DocumentFile legacyFile;
    CHECK(legacyFile.create(legacyPath, legacy).ok());
    DocumentEditor legacyEditor(legacyFile);
    CHECK(legacyEditor.setLayerName("old-layer", "New").changed);
    CHECK(legacyFile.document()->formatVersion.minor == CurrentFormatMinor);
    legacyFile.close();
    CHECK(legacyFile.open(legacyPath).ok());
    CHECK(layerProperties(*findLayer(*legacyFile.document(), "old-layer")).name == "New");

    Document limitedDocument;
    limitedDocument.extent = {16, 16};
    limitedDocument.assets.emplace_back(RasterAsset{"p", makeRasterLayer(16, 16)});
    limitedDocument.layers.emplace_back(StaticBitmapLayer{{"l", "Old"}, StaticSource{"p"}});
    SerializationLimits limits;
    limits.maximumStringBytes = 8;
    DocumentFile limited;
    CHECK(limited.create((temporary.path() / "limited.iisc").string(), limitedDocument, limits).ok());
    DocumentEditor limitedEditor(limited);
    const auto limitedAuthor = authorshipDump(limited.document()->authorship);
    CHECK(!limitedEditor.setLayerName("l", "123456789").ok());
    CHECK(limited.lastResult().code == DocumentFileCode::LimitExceeded && limited.revision() == 0);
    CHECK(layerProperties(*findLayer(*limited.document(), "l")).name == "Old");
    CHECK(authorshipDump(limited.document()->authorship) == limitedAuthor);

    namespace sql = iiFileProvider::native::sql;
    const auto v1Path = temporary.path() / "legacy-storage.iisc";
    {
        auto database = sql::Database::open(v1Path, {.mode = sql::OpenMode::create});
        CHECK(database.has_value());
        for (const auto *statement : {
            "PRAGMA application_id=1229542211",
            "PRAGMA user_version=1",
            "CREATE TABLE canvas_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), revision INTEGER NOT NULL CHECK(revision>=0))",
            "CREATE TABLE canvas_records (kind INTEGER NOT NULL, id TEXT NOT NULL, position INTEGER NOT NULL CHECK(position>=0), data BLOB NOT NULL, digest BLOB NOT NULL, UNIQUE(kind,id))",
            "INSERT INTO canvas_state VALUES(1,0)"})
            CHECK(database->execute(statement).has_value());
        const auto unhex = [](std::string_view text) {
            const auto digit = [](char c) { return c <= '9' ? c - '0' : (c <= 'F' ? c - 'A' + 10 : c - 'a' + 10); };
            sql::Blob bytes;
            for (std::size_t i = 0; i < text.size(); i += 2)
                bytes.push_back(std::uint8_t((digit(text[i]) << 4) | digit(text[i + 1])));
            return bytes;
        };
        for (const auto &record : loadLegacyWorkingRecords(
                 std::filesystem::u8path(IISHAREDCANVAS_TEST_RESOURCE_DIR) / "fixtures/legacy-working-v1.tsv")) {
            auto insert = database->prepare("INSERT INTO canvas_records VALUES(?,?,?,?,?)");
            CHECK(insert.has_value());
            CHECK(insert->bind(1, std::int64_t(record.kind)).has_value());
            CHECK(insert->bind(2, record.id).has_value());
            CHECK(insert->bind(3, std::int64_t(record.position)).has_value());
            CHECK(insert->bind(4, unhex(record.data)).has_value());
            CHECK(insert->bind(5, unhex(record.digest)).has_value());
            CHECK(insert->step().has_value());
        }
    }
    DocumentFile v1;
    CHECK(v1.open(v1Path.string()).ok() && v1.storageVersion() == 1);
    const auto v1Bytes = iiFileProvider::native::File::read(v1Path);
    DocumentEditor v1Editor(v1);
    CHECK(v1Editor.setLayerName("layer", "Paint").ok());
    CHECK(v1.storageVersion() == 1 && v1.revision() == 0);
    CHECK(iiFileProvider::native::File::read(v1Path) == v1Bytes);
    CHECK(v1Editor.setLayerName("layer", "Migrated").changed);
    CHECK(v1.storageVersion() == 2 && v1.revision() == 1);
    v1.close();
    CHECK(v1.open(v1Path.string()).ok());
    CHECK(layerProperties(*findLayer(*v1.document(), "layer")).name == "Migrated");
    CHECK(findRasterAsset(*v1.document(), "paint")->pixels.pixels[0] == 0xff123456);
    std::cout << "LAYER_EDIT_ROUNDTRIP metadata=true keyframes=true mask=true bitmap_undo=true "
                 "stale_conflict=true older_model_fallback=true v1_migration=true budget_rollback=true\n";
    return 0;
}
