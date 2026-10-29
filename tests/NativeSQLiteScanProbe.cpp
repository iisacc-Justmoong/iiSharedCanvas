// Test-only macOS dynamic interposer; SQLite keeps ownership of each statement.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <sqlite3.h>
#include <unistd.h>
namespace {
std::atomic<std::uint64_t> steps{0}, statements{0};
int observed_step(sqlite3_stmt* statement) {
    // An interposer resolving to itself must fail visibly instead of recursing.
    thread_local bool active = false;
    if (active) {
        constexpr char message[] = "recursive sqlite interposition\n";
        (void)::write(2, message, sizeof(message) - 1);
        return SQLITE_MISUSE;
    }
    active = true;
    struct Reset { bool& value; ~Reset() { value = false; } } guard{active};
    const auto original = &sqlite3_step;
    if (!original || reinterpret_cast<const void*>(original) == reinterpret_cast<const void*>(&observed_step)) {
        constexpr char message[] = "sqlite original resolved to interposer\n";
        (void)::write(2, message, sizeof(message) - 1);
        return SQLITE_MISUSE;
    }
    const auto result = original(statement);
    if (result == SQLITE_DONE) {
        const char* sql = sqlite3_sql(statement);
        if (sql && std::strstr(sql, "canvas_tiles")) {
            steps.fetch_add(static_cast<std::uint64_t>(sqlite3_stmt_status(statement, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0)));
            statements.fetch_add(1);
        }
    }
    return result;
}
__attribute__((used,section("__DATA,__interpose")))
static const struct { const void* replacement; const void* original; } mapping = {
    reinterpret_cast<const void*>(&observed_step), reinterpret_cast<const void*>(&sqlite3_step)
};
}
extern "C" void ii_scan_probe_reset() { steps.store(0); statements.store(0); }
extern "C" std::uint64_t ii_scan_probe_steps() { return steps.load(); }
extern "C" std::uint64_t ii_scan_probe_statements() { return statements.load(); }
