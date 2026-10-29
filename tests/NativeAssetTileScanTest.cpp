// Test-only SQL-work budget for removing an empty asset next to dense tiles.
// The bound counts SQLite full-scan steps, not milliseconds or RSS.
#include <iiSharedCanvasNative.h>
#include <iiFileProviderNative.h>
#include <sqlite3.h>
#include <cstdint>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace iiSharedCanvas;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Probe {
    using Reset = void (*)(); using Read = std::uint64_t (*)();
    Reset reset = reinterpret_cast<Reset>(dlsym(RTLD_DEFAULT, "ii_scan_probe_reset"));
    Read steps = reinterpret_cast<Read>(dlsym(RTLD_DEFAULT, "ii_scan_probe_steps"));
    Read statements = reinterpret_cast<Read>(dlsym(RTLD_DEFAULT, "ii_scan_probe_statements"));
    Probe() { check(reset && steps && statements, "interposer missing"); }
};
struct PreserveFailure {
    iiFileProvider::native::TemporaryDirectory& directory;
    int exceptions = std::uncaught_exceptions();
    ~PreserveFailure() {
        if (std::uncaught_exceptions() > exceptions) {
            try { std::cerr << "preserved_fixture=" << directory.path().string() << '\n'; } catch (...) {}
            directory.release();
        }
    }
};
void control(Probe& probe) {
    sqlite3* db = nullptr; check(sqlite3_open(":memory:", &db) == SQLITE_OK, "control open");
    const char* create = "CREATE TABLE canvas_tiles(x INTEGER);"
        "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<128)"
        " INSERT INTO canvas_tiles SELECT x FROM n;";
    check(sqlite3_exec(db, create, nullptr, nullptr, nullptr) == SQLITE_OK, "control fixture");
    probe.reset(); sqlite3_stmt* stmt = nullptr;
    check(sqlite3_prepare_v2(db, "SELECT sum(x) FROM canvas_tiles", -1, &stmt, nullptr) == SQLITE_OK,
          "control query");
    check(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 8256
          && sqlite3_step(stmt) == SQLITE_DONE, "control result");
    sqlite3_finalize(stmt);
    check(probe.statements() > 0 && probe.steps() >= 127, "SQLite scan was not intercepted");
    std::cout << "control_fullscan_steps=" << probe.steps() << '\n';
    sqlite3_close(db);
}
void actual(Probe& probe, const std::filesystem::path& parent) {
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    check(temporary.isValid(), "temporary fixture");
    PreserveFailure preserve{temporary};
    const auto path = (temporary.path() / "structural-scan.iisc").string();
    constexpr int side = 2048;
    Document d; d.extent = {side, side};
    auto dense = makeRasterLayer(side, side, 0xff336699U);
    dense.pixels[100] = 0xff012345U;
    d.assets.emplace_back(RasterAsset{"unrelated", std::move(dense)});
    d.assets.emplace_back(RasterAsset{"blank", makeRasterLayer(8, 8, 0)});
    d.layers.emplace_back(StaticBitmapLayer{{"unrelated-layer", "Unrelated"}, StaticSource{"unrelated"}});
    d.layers.emplace_back(StaticBitmapLayer{{"blank-layer", "Blank"}, StaticSource{"blank"}});
    DocumentFile owner;
    check(owner.create(path, d).ok(), "create fixture"); d = {};
    DocumentHistory history(owner); const auto revision = owner.revision();
    auto* before = findRasterAsset(*owner.document(), "unrelated");
    check(before && before->pixels.pixels.size() == std::size_t(side) * side, "unrelated asset exists");
    const auto* data = before->pixels.pixels.data();
    probe.reset();
    const auto result = history.removeLayerWithAsset("blank-layer");
    const auto scanSteps = probe.steps(), statements = probe.statements();
    std::cout << "unrelated_tiles=1024 canvas_statements=" << statements
              << " fullscan_steps=" << scanSteps << " scan_budget=32\n" << std::flush;
    check(result.changed && owner.revision() == revision + 1 && !findAsset(*owner.document(), "blank"),
          "remove is one revision");
    check(statements > 0, "provider's SQLite statements were not observed");
    check(scanSteps <= 32, "asset edit scans unrelated tile rows");
    check(history.undo().changed && findRasterAsset(*owner.document(), "blank") && history.redo().changed,
          "undo/redo preserve asset");
    auto* after = findRasterAsset(*owner.document(), "unrelated");
    check(after && after->pixels.pixels.data() == data && after->pixels.pixels[100] == 0xff012345U,
          "unrelated raster preserved");
    Document reopened;
    check(DocumentFile::readSnapshot(path, reopened).ok(), "reopen snapshot");
    auto* restored = findRasterAsset(reopened, "unrelated");
    check(restored && restored->pixels.pixels.size() == std::size_t(side) * side
          && restored->pixels.pixels[100] == 0xff012345U && !findAsset(reopened, "blank"), "reopened data");
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected control or owned fixture directory");
        Probe probe; control(probe);
        if (std::string_view(argv[1]) != "--control") actual(probe, std::filesystem::path(argv[1]));
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
