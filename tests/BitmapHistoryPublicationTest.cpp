#include <iiSharedCanvasNative.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderNative.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace iiSharedCanvas;

namespace {
#if defined(__unix__) || defined(__APPLE__)
std::string testExecutable;
#endif
void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
Document fixture(bool chunks) {
    Document document;
    document.extent = {192, 128};
    if (chunks) {
        document.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    } else {
        document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(192, 128)});
    }
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return document;
}
auto snapshot(const Document &document) {
    auto result = encodeIisc(document);
    check(result.ok(), "document encoding failed");
    return result.bytes;
}
void expectFile(const DocumentFile &file, const Document &expected) {
    check(file.document() && snapshot(*file.document()) == snapshot(expected),
          "file owner differs from the source document");
    Document reopened;
    check(DocumentFile::readSnapshot(file.filePath(), reopened).ok() &&
          snapshot(reopened) == snapshot(expected), "durable snapshot differs from source");
}

template<class Publish>
void publishWhileReaderFinishes(const std::filesystem::path &path, Publish publish) {
    namespace sql = iiFileProvider::native::sql;
    std::promise<std::string> ready;
    auto started = ready.get_future();
    std::promise<std::string> finished;
    auto completed = finished.get_future();
    std::jthread reader([&] {
        try {
            // The independent connection performs only a read transaction.
            // Opening it read/write avoids macOS SQLite's same-process
            // read-only descriptor lock-upgrade error (SQLITE_IOERR_LOCK).
            auto connection = sql::Database::open(path);
            if (!connection) throw std::runtime_error(connection.error().message);
            auto transaction = sql::Transaction::begin(*connection, false);
            if (!transaction) throw std::runtime_error(transaction.error().message);
            {
                auto statement = connection->prepare("SELECT revision FROM canvas_state WHERE singleton=1");
                if (!statement) throw std::runtime_error(statement.error().message);
                auto row = statement->step();
                if (!row || *row != sql::Step::row) throw std::runtime_error("reader did not obtain a snapshot");
            }
            ready.set_value({});
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            auto result = transaction->commit();
            finished.set_value(result ? std::string{} : result.error().message);
        } catch (const std::exception &error) {
            try { ready.set_value(error.what()); } catch (const std::future_error &) {}
            finished.set_value(error.what());
        }
    });
    const auto startError = started.get();
    if (!startError.empty()) throw std::runtime_error("reader setup: " + startError);
    const bool published = publish();
    reader.join();
    const auto finishError = completed.get();
    if (!finishError.empty()) throw std::runtime_error("reader cleanup: " + finishError);
    check(published, "history publication failed while a transient reader was finishing");
}

#if defined(__unix__) || defined(__APPLE__)
int readInSeparateProcess(const std::filesystem::path &path) {
    namespace sql = iiFileProvider::native::sql;
    auto connection = sql::Database::open(path, {.mode = sql::OpenMode::read_only});
    if (!connection) return 2;
    auto transaction = sql::Transaction::begin(*connection, false);
    if (!transaction) return 3;
    {
        auto statement = connection->prepare("SELECT revision FROM canvas_state WHERE singleton=1");
        if (!statement) return 4;
        auto row = statement->step();
        if (!row || *row != sql::Step::row) return 5;
    }
    const char ready = 'R';
    if (::write(STDOUT_FILENO, &ready, 1) != 1) return 6;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    return transaction->commit() ? 0 : 7;
}

template<class Publish>
void publishWhileProcessReads(const std::filesystem::path &path, Publish publish) {
    int signal[2];
    check(::pipe(signal) == 0, "reader process pipe failed");
    const auto sourcePath = path.string();
    const auto pid = ::fork();
    if (pid == 0) {
        ::close(signal[0]);
        if (::dup2(signal[1], STDOUT_FILENO) < 0) ::_exit(126);
        ::close(signal[1]);
        ::execl(testExecutable.c_str(), testExecutable.c_str(), "--read-lock",
                sourcePath.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ::close(signal[1]);
    if (pid < 0) { ::close(signal[0]); throw std::runtime_error("reader process launch failed"); }
    char ready = 0;
    const auto received = ::read(signal[0], &ready, 1);
    ::close(signal[0]);
    bool published = false;
    try { if (received == 1 && ready == 'R') published = publish(); }
    catch (...) { int status = 0; (void)::waitpid(pid, &status, 0); throw; }
    int status = 0;
    check(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "reader process did not finish successfully");
    check(received == 1 && ready == 'R', "reader process did not acquire a snapshot");
    check(published, "history publication failed while a separate process was reading");
}
#endif

template <class Editor>
void exercise(const std::filesystem::path &path, bool chunks) {
    auto source = fixture(chunks);
    DocumentFile file;
    check(file.create(path.string(), source).ok(), "file setup failed");
    Editor editor(source, "paint");
    const auto before = source.authorship;
    const auto versionBefore = source.formatVersion;
    const auto original = snapshot(source);
    const std::array<std::uint32_t, 3> first{0xff123456, 0x80000011, 0xffabc123};
    const std::array<std::uint32_t, 1> second{0xff998877};
    const std::array patches{BitmapPatch{{{63, 4}, 3, 1}, first},
                             BitmapPatch{{{130, 68}, 1, 1}, second}};
    check(editor.replacePatches(patches) && editor.canUndo(), "source edit failed");
    const auto after = source.authorship;
    const auto versionAfter = source.formatVersion;
    const auto changed = snapshot(source);
    const auto fileRevision = file.revision();
    check(!editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                      after, versionBefore, fileRevision) &&
          snapshot(*file.document()) == original && editor.canUndo(),
          "unexpected authorship changed the file or cursor");
    std::uint32_t *tampered = nullptr;
    if (auto *asset = findRasterAsset(source, "paint"))
        tampered = &asset->pixels.pixels[4 * asset->pixels.width + 63];
    else {
        auto *chunkedAsset = findChunkedRasterAsset(source, "paint");
        auto *chunk = chunkedAsset ? findRasterChunk(*chunkedAsset, 0, 0) : nullptr;
        check(chunk != nullptr, "source chunk unavailable");
        tampered = &chunk->pixels.pixels[4 * chunk->pixels.width + 63];
    }
    const auto savedPixel = *tampered;
    *tampered ^= 1;
    check(!editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                      before, versionBefore, fileRevision) &&
          snapshot(*file.document()) == original && editor.canUndo(),
          "changed source pixels were published or moved history");
    *tampered = savedPixel;
    check(editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                     before, versionBefore, fileRevision),
          "completed history could not be published");
    check(file.revision() == fileRevision + 1 && editor.canUndo() && !editor.canRedo(),
          "publication changed the source cursor or wrong file revision");
    check(file.lastWriteStatistics().payloadBytesWritten < 60000,
          "publication wrote more than the changed tiles");
    expectFile(file, source);

    check(editor.undo(), "source undo failed");
    source.authorship = before;
    source.formatVersion = versionBefore;
    check(snapshot(source) == original && editor.canRedo(), "source undo is not exact");
    publishWhileReaderFinishes(path, [&] {
        const bool ok = editor.publishHistoryState(file, BitmapHistoryState::Undone,
                                                   after, versionAfter, file.revision());
        if (!ok) std::cerr << "undo publication: " << editor.lastError() << '\n';
        return ok;
    });
    expectFile(file, source);

    check(editor.redo(), "source redo failed");
    source.authorship = after;
    source.formatVersion = versionAfter;
    check(snapshot(source) == changed, "source redo is not exact");
    publishWhileReaderFinishes(path, [&] {
        const bool ok = editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                                   before, versionBefore, file.revision());
        if (!ok) std::cerr << "redo publication: " << editor.lastError() << '\n';
        return ok;
    });
    expectFile(file, source);

#if defined(__unix__) || defined(__APPLE__)
    check(editor.undo(), "undo before external reader failed");
    source.authorship = before;
    source.formatVersion = versionBefore;
    publishWhileProcessReads(path, [&] {
        return editor.publishHistoryState(file, BitmapHistoryState::Undone,
                                          after, versionAfter, file.revision());
    });
    expectFile(file, source);
    check(editor.redo(), "redo before external reader failed");
    source.authorship = after;
    source.formatVersion = versionAfter;
    publishWhileProcessReads(path, [&] {
        return editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                          before, versionBefore, file.revision());
    });
    expectFile(file, source);
#endif

    const auto stable = snapshot(*file.document());
    const auto revision = file.revision();
    check(!editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                      before, versionBefore, revision - 1),
          "stale file revision was accepted");
    check(snapshot(*file.document()) == stable && file.revision() == revision &&
          editor.canUndo(), "stale publication changed owner or history");
    check(!editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                      after, versionAfter, revision),
          "publishing to already changed pixels was accepted");
    check(snapshot(*file.document()) == stable && file.revision() == revision &&
          editor.canUndo(), "pixel conflict changed owner or history");
    expectFile(file, source);

    check(editor.undo(), "undo before locked publication failed");
    source.authorship = before;
    source.formatVersion = versionBefore;
    namespace sql = iiFileProvider::native::sql;
    auto connection = sql::Database::open(path);
    check(bool(connection), "cannot open file for locking");
    auto lock = sql::Transaction::begin(*connection, true);
    check(bool(lock), "cannot lock file");
    check(!editor.publishHistoryState(file, BitmapHistoryState::Undone,
                                      after, versionAfter, revision),
          "locked file accepted publication");
    check(file.revision() == revision && snapshot(*file.document()) == stable && editor.canRedo(),
          "locked publication changed owner or source cursor");
    check(bool(lock->rollback()), "could not release file lock");
    check(editor.publishHistoryState(file, BitmapHistoryState::Undone,
                                     after, versionAfter, revision),
          "publication could not retry after a lock failure");
    expectFile(file, source);
}

void grow(const std::filesystem::path &path) {
    Document source;
    source.extent = {128, 128};
    source.canvasMode = CanvasMode::Infinite;
    source.infiniteCanvas = {{-64, -64}, 64};
    source.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    source.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    DocumentFile file;
    check(file.create(path.string(), source).ok(), "growing document setup failed");
    ChunkedBitmapEditor editor(source, "paint");
    BitmapBrush brush; brush.size = 4; brush.argb = 0xff123456;
    brush.pressureToOpacityEnabled = false;
    check(editor.setBrush(brush) && editor.setStrokeCanvasGrowth(true), "growth setup failed");
    const auto before = source.authorship;
    const auto versionBefore = source.formatVersion;
    StrokePoint first; first.position = {50, 0}; first.time = 1;
    StrokePoint last = first; last.position = {90, 0}; last.time = 2;
    check(editor.beginStrokeInput(first) && editor.endStrokeInput(last), "growing stroke failed");
    const auto after = source.authorship;
    const auto versionAfter = source.formatVersion;
    check(editor.publishHistoryState(file, BitmapHistoryState::Applied,
                                     before, versionBefore, file.revision()),
          "growing stroke publication failed");
    expectFile(file, source);
    check(editor.undo(), "growing stroke undo failed");
    source.authorship = before; source.formatVersion = versionBefore;
    check(editor.publishHistoryState(file, BitmapHistoryState::Undone,
                                     after, versionAfter, file.revision()),
          "growing stroke undo publication failed");
    expectFile(file, source);
}
}

int main(int argc, char **argv) try {
#if defined(__unix__) || defined(__APPLE__)
    if (argc == 3 && std::string_view(argv[1]) == "--read-lock")
        return readInSeparateProcess(argv[2]);
    testExecutable = std::filesystem::absolute(argv[0]).string();
#else
    (void)argc; (void)argv;
#endif
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    iiFileProvider::native::TemporaryDirectory directory(parent);
    check(directory.isValid(), "test directory unavailable");
    exercise<BitmapEditor>(directory.path() / "finite.iisc", false);
    exercise<ChunkedBitmapEditor>(directory.path() / "chunks.iisc", true);
    grow(directory.path() / "growth.iisc");
    std::cout << "BITMAP_HISTORY_PUBLICATION_OK\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
