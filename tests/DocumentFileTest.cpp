#include <iiSharedCanvas.h>
#include <File/DocumentFile.h>

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QTemporaryDir>

#include <sqlite3.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

using namespace iiSharedCanvas;
int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

Document makeDocument()
{
    Document document;
    document.extent = {64, 64};
    document.timeline.frameCount = 48;
    document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(64, 64)});
    document.assets.emplace_back(VectorAsset{"vector", {64, 64}, {}});
    document.layers.emplace_back(BitmapLayer{
        {"paint-layer", "Paint"}, StaticSource{"paint"}});
    document.layers.emplace_back(VectorLayer{
        {"vector-layer", "Vector"}, StaticSource{"vector"}});
    return document;
}

Document readDocument(const std::string &path)
{
    DocumentFile reader;
    const auto result = reader.open(path);
    expect(result.ok(), "a second connection must read the committed file: " + result.message);
    return reader.document() ? *reader.document() : makeDocument();
}

std::vector<std::uint8_t> snapshot(const Document &document)
{
    auto encoded = encodeIisc(document);
    expect(encoded.ok(), "test snapshots must be serializable");
    return encoded.bytes;
}

void expectCurrent(const DocumentFile &file, const std::string &message)
{
    expect(file.document() && snapshot(*file.document()) == snapshot(readDocument(file.filePath())),
           message);
}

void sql(sqlite3 *database, const char *statement)
{
    char *error = nullptr;
    const int result = sqlite3_exec(database, statement, nullptr, nullptr, &error);
    expect(result == SQLITE_OK, error ? error : "test SQL must succeed");
    sqlite3_free(error);
}

void testWriteThrough(const std::string &path)
{
    DocumentFile file;
    const auto created = file.create(path, makeDocument());
    expect(created.ok(), "creating a working file must persist its initial document: " + created.message);
    if (!created.ok()) {
        return;
    }
    expectCurrent(file, "creation must be readable without a save call");

    DocumentFile duplicate;
    expect(!duplicate.create(path, makeDocument()).ok(), "create must never overwrite an existing file");

    DocumentEditor structure(file);
    expect(structure.isBound(), "a structural editor must bind directly to a working file");
    expect(structure.document() == nullptr,
           "a file-backed editor must not expose a mutable aggregate outside a transaction");
    expect(structure.setLayerName("paint-layer", "Renamed").ok(), "renaming a layer must write through");
    expectCurrent(file, "structural edits must reach the file before returning");
    expect(structure.setLayerFrameRange("paint-layer", LayerFrameRange{1, 24}).ok(),
           "layer existence ranges must write through");
    expect(structure.setKeyframedSource("vector-layer", {{0, "vector"}, {12, "vector"}}).ok(),
           "frame-owned keyframes must write through");
    expectCurrent(file, "timeline changes must persist frame-owned keys and inclusive ranges");

    VectorEditor vector(file, "vector");
    expect(vector.createPath({1, 1}, std::nullopt, StrokeStyle{{0xff00ffffU}, 1.0}).ok(),
           "vector path creation must write through");
    expect(vector.appendLineTo(0, {8, 8}).ok(), "linear edits must write through");
    expect(vector.appendCubicBezierTo(0, {10, 5}, {20, 5}, {30, 20}).ok(),
           "Bezier edits must write through");
    expectCurrent(file, "vector edits must be durable without an explicit save");

    BitmapEditor bitmap(file, "paint");
    expect(bitmap.setPixel(3, 4, 0xff123456U), "pixel editing must write through");
    expect(file.lastWriteStatistics().payloadBytesWritten <= 4,
           "one pixel must update only its changed bytes, not dump a document or raster");
    expect(file.lastWriteStatistics().recordsWritten == 1,
           "a pixel edit must not rewrite unrelated layer or vector records");
    expectCurrent(file, "pixels must reach the file before setPixel returns");

    const auto revision = file.revision();
    expect(bitmap.setPixel(3, 4, 0xff123456U), "an unchanged pixel is a successful no-op");
    expect(file.revision() == revision && file.lastWriteStatistics().payloadBytesWritten == 0,
           "no-op edits must not write or advance the file revision");
    expect(bitmap.undo(), "file-backed bitmap edits must be undoable");
    expectCurrent(file, "undo must itself immediately write through");
    expect(bitmap.redo(), "file-backed bitmap edits must be redoable");
    expectCurrent(file, "redo must itself immediately write through");
    expect(bitmap.replacePatch({{62, 1}, 2, 2}, {0xff112233U, 0xff223344U, 0xff334455U, 0xff445566U}),
           "pixel patches crossing stored byte blocks must write through");
    expectCurrent(file, "a rectangular patch must persist row-major pixels");
    expect(bitmap.replacePixels(makeRasterLayer(64, 64, 0xff998877U)),
           "complete raster replacement must write through");
    expectCurrent(file, "raster replacement must preserve the rest of the document");
    expect(bitmap.clear(), "bitmap clear must write through");
    expectCurrent(file, "cleared pixels must persist");

    const auto beforeStroke = snapshot(*file.document());
    BitmapBrush brush;
    brush.size = 3;
    brush.argb = 0xffff3300U;
    expect(bitmap.setBrush(brush) && bitmap.beginStroke({5, 8}), "a file-backed stroke must begin");
    expectCurrent(file, "the first committed stroke pixels must persist before pointer release");
    expect(bitmap.continueStroke({30, 8}), "streamed stroke samples must be accepted");
    expectCurrent(file, "each stroke increment must write pixels while the stroke is active");
    bitmap.cancelStroke();
    expect(!bitmap.strokeActive() && snapshot(*file.document()) == beforeStroke,
           "cancelling a stroke must restore the document");
    expectCurrent(file, "stroke cancellation must immediately restore file pixels too");
    expect(bitmap.beginStroke({5, 5}) && bitmap.continueStroke({20, 20}) && bitmap.endStroke({30, 30}),
           "a completed streamed stroke must write through every stage");
    expectCurrent(file, "pointer release must not be a separate save boundary");

    expect(file.edit([](Document &draft) {
        layerProperties(draft.layers.front()).opacity = 0.5;
        draft.stableDiffusionMetadata = StableDiffusionMetadata{};
        draft.stableDiffusionMetadata->positivePrompt = "direct aggregate transaction";
        return true;
    }).ok(), "application-defined aggregate edits must share the write-through boundary");
    expectCurrent(file, "custom edits and generation metadata must also persist immediately");
    expect(file.edit([](Document &draft) {
        draft.infiniteCanvas.chunkSize = 64;
        return true;
    }).ok(), "inactive canvas configuration is still document data");
    expect(file.document()->infiniteCanvas.chunkSize == 64
               && readDocument(path).infiniteCanvas.chunkSize == 64,
           "working files must preserve fields that the legacy interchange snapshot omits");

    const auto beforeRejected = snapshot(*file.document());
    const auto beforeRevision = file.revision();
    expect(!file.edit([](Document &draft) {
        draft.layers.clear();
        return false;
    }).ok(), "a rejected custom edit must not commit");
    expect(!file.edit([](Document &draft) {
        draft.extent.width = -1;
        return true;
    }).ok(), "invalid aggregate edits must fail closed");
    expect(!file.edit([](Document &draft) -> bool {
        draft.layers.clear();
        throw std::runtime_error("cancelled edit");
    }).ok(), "an exception in a custom edit must not leak partial state");
    expect(snapshot(*file.document()) == beforeRejected && file.revision() == beforeRevision,
           "rejected edits must preserve both document and revision");
    expectCurrent(file, "a rejected edit must preserve the prior file");
}

void testFailureAndConflict(const std::string &path)
{
    DocumentFile file;
    expect(file.open(path).ok(), "the test file must reopen");
    BitmapEditor bitmap(file, "paint");
    DocumentEditor structure(file);
    const auto before = snapshot(*file.document());
    const auto fileRevision = file.revision();
    sqlite3 *reader = nullptr;
    expect(sqlite3_open(path.c_str(), &reader) == SQLITE_OK, "a competing reader must open");
    sql(reader, "BEGIN; SELECT * FROM canvas_state;");
    expect(!bitmap.setPixel(1, 1, 0xffffffffU), "a blocked disk commit must report failure");
    expect(bitmap.revision() == 0 && !bitmap.canUndo(), "failed disk writes must not create undo entries");
    expect(!structure.setLayerName("paint-layer", "must not commit").ok(),
           "a structural disk failure must be returned to the caller");
    expect(structure.revision() == 0 && file.revision() == fileRevision,
           "disk failure must preserve editor and file revisions");
    expect(snapshot(*file.document()) == before, "disk failure must restore the in-memory document");
    sql(reader, "ROLLBACK;");
    sqlite3_close(reader);
    expectCurrent(file, "failed transactions must leave the previous file intact");
    expect(bitmap.setPixel(1, 1, 0xffffffffU), "editing may retry after a temporary lock is released");
    expect(bitmap.beginStroke({10, 10}), "a persisted stroke must begin before a simulated I/O failure");
    const auto strokePixels = snapshot(*file.document());
    const auto strokeRevision = bitmap.revision();
    expect(sqlite3_open(path.c_str(), &reader) == SQLITE_OK, "a reader must reopen for a stroke lock");
    sql(reader, "BEGIN; SELECT * FROM canvas_state;");
    expect(!bitmap.endStroke({55, 55}) && bitmap.strokeActive(),
           "a failed stroke end must preserve the active gesture for retry");
    bitmap.cancelStroke();
    expect(bitmap.strokeActive() && !bitmap.lastError().empty()
               && bitmap.revision() == strokeRevision && snapshot(*file.document()) == strokePixels,
           "failed cancellation must retain pixels, stream state, revision and undo state");
    sql(reader, "ROLLBACK;");
    sqlite3_close(reader);
    bitmap.cancelStroke();
    expect(!bitmap.strokeActive() && bitmap.lastError().empty(), "cancellation may retry after an I/O failure");
    expectCurrent(file, "a retried cancellation must restore the file too");

    DocumentFile stale;
    expect(stale.open(path).ok(), "another author may take a snapshot");
    expect(bitmap.setPixel(2, 2, 0xff556677U), "the current writer must commit a later revision");
    DocumentEditor staleEditor(stale);
    expect(!staleEditor.setLayerName("paint-layer", "stale").ok(),
           "a stale writer must not overwrite newer changes");
    expectCurrent(file, "a stale writer rejection must preserve the latest file");
    CanvasItem canvas;
    expect(canvas.bind(file), "a canvas must bind before closing its file");
    file.close();
    expect(!canvas.refresh() && !canvas.documentReady(), "a closed-file canvas must not render a transient fallback");
    expect(!bitmap.setPixel(0, 0, 0xff112233U), "a closed file must not silently fall back to memory editing");
    expect(file.open(path).ok(), "a file can reopen after close");
    expect(!canvas.refresh() && !canvas.documentReady(), "a stale canvas binding must not follow a reopened file");
    expect(!bitmap.setPixel(0, 0, 0xff112233U), "old editor bindings must not follow a new open session");
    canvas.unbind();
}

void testChunkedAndAdapters(const std::string &path)
{
    Document document;
    document.extent = {64, 64};
    document.canvasMode = CanvasMode::Infinite;
    document.infiniteCanvas = {{-32, -32}, 32};
    document.assets.emplace_back(ChunkedRasterAsset{"chunks", {}});
    document.layers.emplace_back(BitmapLayer{{"chunk-layer", "Chunks"}, StaticSource{"chunks"}});
    DocumentFile file;
    const auto created = file.create(path, document);
    expect(created.ok(), "an infinite-canvas file must be creatable: " + created.message);
    if (!created.ok()) {
        return;
    }
    ChunkedBitmapEditor chunks(file, "chunks");
    expect(chunks.replaceRegion({-32, -32}, makeRasterLayer(64, 64, 0xff556677U)),
           "chunk creation and pixel replacement must write through");
    expectCurrent(file, "negative-coordinate chunks must survive a live reopen");
    expect(chunks.undo() && chunks.redo(), "chunk undo and redo must write through");
    expectCurrent(file, "chunk history navigation must persist immediately");
    expect(chunks.beginStroke({-5, -5}) && chunks.continueStroke({20, 20}),
           "chunked strokes must support write-through increments");
    expectCurrent(file, "in-progress chunk strokes must persist pixels only");
    chunks.cancelStroke();
    expectCurrent(file, "chunk stroke cancellation must write through");

    CanvasItem canvas;
    expect(canvas.bind(file) && canvas.selectLayer(QStringLiteral("chunk-layer")),
           "the full canvas adapter must bind to a working file");
    expect(canvas.document() == nullptr, "a file-backed canvas must not expose mutable document state");
    expect(canvas.clearSelectedLayer(), "adapter editing must use its file-backed editor");
    expectCurrent(file, "adapter changes must persist without refresh or save");
    expect(canvas.editDocument([](DocumentEditor &editor) {
        return editor.setLayerName("chunk-layer", "Adapter edit");
    }).ok(), "adapter structural editing must write through");
    expectCurrent(file, "adapter structural changes must persist");
    canvas.unbind();
}

void testRecordStructure(const std::string &path)
{
    DocumentFile file;
    expect(file.create(path, makeDocument()).ok(), "structural test file must be created");
    if (!file.isOpen()) {
        return;
    }
    DocumentEditor editor(file);
    expect(editor.moveAsset("vector", 0).ok() && file.lastWriteStatistics().payloadBytesWritten == 0,
           "asset reordering must update positions without rewriting raster payloads");
    expectCurrent(file, "reordered asset positions must reopen identically");
    expect(editor.moveLayer("vector-layer", 0).ok() && file.lastWriteStatistics().payloadBytesWritten == 0,
           "layer reordering must not rewrite payloads");
    expect(editor.renameAsset("paint", "renamed-paint").ok(), "asset rename must update dependent records");
    expect(editor.renameLayer("vector-layer", "renamed-vector").ok(), "layer rename must move the record identity");
    expectCurrent(file, "renames must persist identities and references together");
    expect(editor.insertVectorAsset("spare", {64, 64}, {}).ok() && editor.removeAsset("spare").ok(),
           "inserted and removed asset records must be transactional");
    expect(editor.setKeyframedSource("renamed-vector", {{0, "vector"}, {12, "vector"}}).ok()
               && editor.moveKeyframe("renamed-vector", 12, 20).ok()
               && editor.insertKeyframe("renamed-vector", 24, "vector").ok()
               && editor.removeKeyframe("renamed-vector", 20).ok(),
           "frame-owned key insertion, moves, and deletion must write through");
    expectCurrent(file, "frame ownership must survive multiple incremental source changes");
    expect(editor.setStaticSource("renamed-vector", "vector").ok()
               && editor.removeLayer("renamed-vector").ok() && editor.removeAsset("vector").ok(),
           "layer and unreferenced asset deletion must remove their records");
    expectCurrent(file, "record deletion must not resurrect keys or assets on reopen");
}

void testInvalidFiles(const std::string &sourcePath, const QString &directory)
{
    const auto corruptCopy = [&](const QString &name, const char *statement, DocumentFileCode expected) {
        const QString path = QDir(directory).filePath(name);
        expect(QFile::copy(QString::fromStdString(sourcePath), path), "a corrupt fixture must copy the idle working file");
        sqlite3 *database = nullptr;
        expect(sqlite3_open(path.toUtf8().constData(), &database) == SQLITE_OK, "the corruption fixture must open");
        sql(database, statement);
        sqlite3_close(database);
        DocumentFile file;
        const auto result = file.open(path.toStdString());
        expect(result.code == expected && !file.isOpen(), "invalid working files must fail closed: " + name.toStdString());
    };
    corruptCopy(QStringLiteral("unknown-version.iisc"), "PRAGMA user_version=999", DocumentFileCode::UnsupportedFormat);
    corruptCopy(QStringLiteral("unknown-application.iisc"), "PRAGMA application_id=123", DocumentFileCode::UnsupportedFormat);
    corruptCopy(QStringLiteral("unknown-schema.iisc"), "CREATE VIEW unexpected AS SELECT * FROM canvas_state", DocumentFileCode::CorruptFile);
    corruptCopy(QStringLiteral("corrupt-record.iisc"), "UPDATE canvas_records SET data=zeroblob(length(data)) WHERE kind=1", DocumentFileCode::CorruptFile);

    const QString badReferencePath = QDir(directory).filePath(QStringLiteral("bad-reference.iisc"));
    expect(QFile::copy(QString::fromStdString(sourcePath), badReferencePath), "a bad-reference fixture must copy");
    sqlite3 *database = nullptr;
    expect(sqlite3_open(badReferencePath.toUtf8().constData(), &database) == SQLITE_OK, "bad-reference database must open");
    sqlite3_stmt *query = nullptr;
    expect(sqlite3_prepare_v2(database, "SELECT data FROM canvas_records WHERE kind=3 AND id='paint-layer'", -1, &query, nullptr) == SQLITE_OK
               && sqlite3_step(query) == SQLITE_ROW, "the layer payload must be queryable");
    QByteArray data(static_cast<const char *>(sqlite3_column_blob(query, 0)), sqlite3_column_bytes(query, 0));
    sqlite3_finalize(query);
    const auto sourceOffset = data.lastIndexOf("paint");
    expect(sourceOffset >= 0, "the fixture must contain a raster source id");
    if (sourceOffset >= 0) {
        data[sourceOffset] = 'x';
    }
    const QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    expect(sqlite3_prepare_v2(database, "UPDATE canvas_records SET data=?,digest=? WHERE kind=3 AND id='paint-layer'", -1, &query, nullptr) == SQLITE_OK,
           "a correctly checksummed invalid reference must be writable as a fixture");
    sqlite3_bind_blob(query, 1, data.constData(), static_cast<int>(data.size()), SQLITE_TRANSIENT);
    sqlite3_bind_blob(query, 2, hash.constData(), static_cast<int>(hash.size()), SQLITE_TRANSIENT);
    expect(sqlite3_step(query) == SQLITE_DONE, "the invalid-reference fixture must update");
    sqlite3_finalize(query);
    sqlite3_close(database);
    DocumentFile invalidReference;
    expect(invalidReference.open(badReferencePath.toStdString()).code == DocumentFileCode::CorruptFile,
           "valid checksums must not bypass model/reference validation");

    DocumentFile limited;
    SerializationLimits limits;
    limits.maximumContainerBytes = 64;
    expect(limited.open(sourcePath, limits).code == DocumentFileCode::LimitExceeded,
           "working-file reads must enforce resource limits");
    const auto limitedPath = QDir(directory).filePath(QStringLiteral("too-large.iisc"));
    expect(!limited.create(limitedPath.toStdString(), makeDocument(), limits).ok() && !QFile::exists(limitedPath),
           "over-limit documents must fail before file creation");

    const auto legacyBytes = snapshot(makeDocument());
    const QString legacyPath = QDir(directory).filePath(QStringLiteral("legacy-snapshot.iisc"));
    QFile legacy(legacyPath);
    expect(legacy.open(QIODevice::WriteOnly | QIODevice::NewOnly), "a legacy snapshot fixture must be creatable");
    legacy.write(reinterpret_cast<const char *>(legacyBytes.data()), static_cast<qint64>(legacyBytes.size()));
    legacy.close();
    DocumentFile legacyReader;
    expect(legacyReader.open(legacyPath.toStdString()).code == DocumentFileCode::UnsupportedFormat,
           "opening a legacy snapshot must require explicit import, not silently overwrite it");
    expect(legacy.open(QIODevice::ReadOnly)
               && legacy.readAll() == QByteArray(reinterpret_cast<const char *>(legacyBytes.data()), static_cast<qsizetype>(legacyBytes.size())),
           "legacy snapshot bytes must remain unchanged");
    for (std::uint16_t minor = 0; minor <= CurrentFormatMinor; ++minor) {
        Document initial = makeDocument();
        initial.formatVersion.minor = minor;
        const auto imported = QDir(directory).filePath(QStringLiteral("import-%1.iisc").arg(minor));
        DocumentFile file;
        const auto result = file.create(imported.toStdString(), initial);
        expect(result.ok(), "every supported legacy model version must import to a working file: " + result.message);
        if (result.ok()) {
            expectCurrent(file, "imported legacy versions must round-trip, including an empty metadata record");
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    using namespace iiSharedCanvas;
    static_assert(std::is_same_v<decltype(std::declval<DocumentFile &>().document()), const Document *>);
    static_assert(!std::is_copy_constructible_v<DocumentFile>);

    if (argc == 3 && std::string(argv[1]) == "--exit-without-close") {
        DocumentFile file;
        if (!file.open(argv[2]).ok()) {
            std::_Exit(2);
        }
        BitmapEditor bitmap(file, "paint");
        if (!bitmap.setPixel(63, 63, 0xffabcdefU)) {
            std::_Exit(3);
        }
        std::_Exit(0);
    }
    if (argc == 3 && std::string(argv[1]) == "--interrupt-transaction") {
        sqlite3 *database = nullptr;
        if (sqlite3_open(argv[2], &database) != SQLITE_OK) {
            std::_Exit(2);
        }
        sql(database, "PRAGMA cache_size=1; BEGIN IMMEDIATE; "
                      "UPDATE canvas_records SET data=zeroblob(length(data)) WHERE kind=1; "
                      "UPDATE canvas_state SET revision=revision+1;");
        // Intentionally skip COMMIT, ROLLBACK, sqlite3_close and destructors.
        std::_Exit(failures ? 3 : 0);
    }

    QDir().mkpath(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR));
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/live-file-XXXXXX"));
    expect(directory.isValid(), "test files must live under build/");
    const std::string path = directory.filePath(QStringLiteral("canvas.iisc")).toStdString();
    testWriteThrough(path);
    if (failures) {
        return 1;
    }
    testFailureAndConflict(path);
    testChunkedAndAdapters(directory.filePath(QStringLiteral("infinite.iisc")).toStdString());
    testRecordStructure(directory.filePath(QStringLiteral("structure.iisc")).toStdString());
    testInvalidFiles(path, directory.path());

    QProcess child;
    child.start(application.applicationFilePath(), {QStringLiteral("--exit-without-close"), QString::fromStdString(path)});
    expect(child.waitForFinished(15000) && child.exitCode() == 0,
           "a child must commit and exit without destructors, close, flush or save");
    Document recovered = readDocument(path);
    expect(findRasterAsset(recovered, "paint")->pixels.pixels.back() == 0xffabcdefU,
           "committed pixels must survive abrupt process termination");
    const auto committed = snapshot(recovered);
    child.start(application.applicationFilePath(), {QStringLiteral("--interrupt-transaction"), QString::fromStdString(path)});
    expect(child.waitForFinished(15000) && child.exitCode() == 0,
           "a child must exit during a partially written transaction");
    expect(snapshot(readDocument(path)) == committed,
           "SQLite recovery must restore the last complete edit after an interrupted transaction");

    BitmapItem bitmapItem;
    DocumentFile file;
    expect(file.open(path).ok() && bitmapItem.bind(file, "paint"),
           "the bitmap adapter must bind to a working file");
    expect(bitmapItem.setPixel(0, 0, QColor::fromRgba(0xff334455U)), "bitmap adapter pixel edits must succeed");
    expectCurrent(file, "bitmap adapter edits must persist without an explicit save");
    bitmapItem.unbind();

    CanvasItem ownedCanvas;
    const auto ownedPath = directory.filePath(QStringLiteral("owned.iisc"));
    expect(ownedCanvas.createFile(ownedPath, 16, 16), "QML creation must create a file before authoring");
    expect(ownedCanvas.beginStrokeAt({4, 4}) && ownedCanvas.endStrokeAt({8, 4}),
           "an owned-file canvas must write brush edits through");
    Document ownedDocument = readDocument(ownedPath.toStdString());
    expect(findRasterAsset(ownedDocument, "canvas.raster.0") != nullptr,
           "QML file creation must retain the canvas raster asset");
    ownedCanvas.unbind();
    expect(ownedCanvas.openFile(ownedPath) && ownedCanvas.documentReady(),
           "QML must reopen a working file without importing a snapshot");

    return failures == 0 ? 0 : 1;
}
