#include <File/DocumentFile.h>
#include <File/IiscUpgrade.h>
#include <Document/DocumentEditor.h>
#include <Render/FrameRenderer.h>
#include <Serialization/IiscCodec.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <sqlite3.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool value, const std::string &message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
QByteArray read(const QString &path)
{
    QFile file(path);
    expect(file.open(QIODevice::ReadOnly), "read fixture " + path.toStdString());
    return file.readAll();
}
void write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size(),
           "create fixture " + path.toStdString());
}
QByteArray bytes(const Document &document)
{
    const auto encoded = encodeIisc(document);
    expect(encoded.ok(), "encode fixture: " + encoded.error.message);
    return {reinterpret_cast<const char *>(encoded.bytes.data()), qsizetype(encoded.bytes.size())};
}
Document current(Document document)
{
    document.formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
    return document;
}
Document fixture(std::uint16_t minor)
{
    Document d; d.formatVersion = {1, minor}; d.extent = {2, 2}; d.timeline.frameCount = 2;
    d.assets.emplace_back(RasterAsset{"r0", makeRasterLayer(2, 2, 0x80ff0033U)});
    d.assets.emplace_back(RasterAsset{"r1", makeRasterLayer(2, 2, 0xff0033ffU)});
    VectorPath path;
    path.commands = {MoveTo{{0,0}}, LineTo{{2,0}}, LineTo{{2,2}}, ClosePath{}};
    path.fill = SolidPaint{0x8000ff00U};
    d.assets.emplace_back(VectorAsset{"v0", {2,2}, {path}});
    path.fill = SolidPaint{0xff1100ffU};
    d.assets.emplace_back(VectorAsset{"v1", {2,2}, {path}});
    d.layers.emplace_back(StaticBitmapLayer{{"sb", "정적 픽셀"}, StaticBitmapContent{"r0"}});
    d.layers.emplace_back(StaticVectorLayer{{"sv", "Static vector"}, StaticVectorContent{"v0"}});
    d.layers.emplace_back(DynamicBitmapLayer{{"db"}, DynamicBitmapContent{KeyframedSource{{0,1}}}});
    d.layers.emplace_back(DynamicVectorLayer{{"dv"}, DynamicVectorContent{KeyframedSource{{0,1}}}});
    d.frames = {{0, {{"db","r0"}, {"dv","v0"}}}, {1, {{"db","r1"}, {"dv","v1"}}}};
    layerProperties(d.layers[1]).opacity = 0.6;
    layerProperties(d.layers[1]).blendMode = RasterBlendMode::Multiply;
    return d;
}
Document richFixture()
{
    auto d = fixture(19);
    d.infiniteCanvas.chunkSize = 64; // Working files also persist inactive canvas settings.
    d.artboards = {{"board", "대지", {{-1,0}, {2,2}}, 0xffffffffU}};
    for (auto &layer : d.layers) { layerProperties(layer).artboardId = "board"; }
    auto &motion = layerProperties(d.layers[1]).motion;
    motion = {{0, {}, MotionInterpolation::Linear}, {1, {}, MotionInterpolation::Hold}};
    motion[1].value.position.x = 1;
    d.assets.emplace_back(VideoAsset{"video", {24,1},
        {makeRasterLayer(1,1,0xff010203U), makeRasterLayer(1,1,0xff040506U)}});
    d.layers.emplace_back(VideoLayer{{"video-layer"}, StaticSource{"video"}});
    d.audioAssets.push_back({"audio"});
    d.audioAssets[0].samples.resize(8000, -1234);
    d.audioTracks.push_back({"track", "음성", false, -3.0,
        {{"clip", "원본", "audio", 0, 2, 0, -1.0, true}}});
    d.stableDiffusionMetadata = StableDiffusionMetadata{};
    d.stableDiffusionMetadata->positivePrompt = "original generation prompt 원본";
    d.stableDiffusionMetadata->negativePrompt = "blur";
    d.stableDiffusionMetadata->extraParameters = {{"custom", "preserve this value"}};
    const auto author = iiFileProvider::FileAuthor::fromIisaccAccount(
        QJsonObject{{"sub", "upgrade-author"}, {"email", "author@example.com"},
            {"displayName", "Original Author"}, {"userId", "@upgrade_author"},
            {"societyCloudMembership", "Free"}, {"avatarUrl", QJsonValue::Null}},
        QUrl("https://iisacc.com"), QDateTime::fromMSecsSinceEpoch(1700000000000, QTimeZone::UTC));
    expect(author.has_value(), "create original author fixture");
    if (author) { DocumentEditor editor(d); expect(editor.setFileAuthor(*author).ok(), "record original author"); }
    d.formatVersion = {1, 19};
    return d;
}
struct ProcessResult { int code; QByteArray out; QByteArray error; };
ProcessResult run(const QStringList &arguments, const QString &directory)
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("QT_QPA_PLATFORM");
    environment.remove("DISPLAY");
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral(IISHAREDCANVAS_UPGRADE_EXECUTABLE), arguments);
    const bool finished = process.waitForStarted(10000) && process.waitForFinished(40000);
    expect(finished, "upgrade CLI finishes: " + process.errorString().toStdString());
    if (!finished) { process.kill(); process.waitForFinished(1000); }
    expect(process.exitStatus() == QProcess::NormalExit, "upgrade CLI must not crash");
    return {finished ? process.exitCode() : -1, process.readAllStandardOutput(), process.readAllStandardError()};
}
void sql(const QString &path, const char *command)
{
    sqlite3 *db = nullptr;
    expect(sqlite3_open_v2(path.toUtf8().constData(), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
           "open independent SQLite fixture");
    expect(sqlite3_exec(db, command, nullptr, nullptr, nullptr) == SQLITE_OK, "mutate SQLite fixture");
    sqlite3_close(db);
}
std::int64_t scalar(const QString &path, const char *command)
{
    sqlite3 *db = nullptr; sqlite3_stmt *query = nullptr; std::int64_t value = -1;
    expect(sqlite3_open_v2(path.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
           "open independent SQLite inspection");
    if (sqlite3_prepare_v2(db, command, -1, &query, nullptr) == SQLITE_OK
        && sqlite3_step(query) == SQLITE_ROW) { value = sqlite3_column_int64(query, 0); }
    else { expect(false, "inspect independent SQLite field"); }
    sqlite3_finalize(query); sqlite3_close(db); return value;
}
QByteArray assetRecords(const QString &path)
{
    sqlite3 *db = nullptr; sqlite3_stmt *query = nullptr; QByteArray value;
    expect(sqlite3_open_v2(path.toUtf8().constData(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
           "open asset inspection");
    expect(sqlite3_prepare_v2(db,
        "SELECT rowid,id,position,data,digest FROM canvas_records WHERE kind IN (1,6) ORDER BY kind,position",
        -1, &query, nullptr) == SQLITE_OK, "query immutable asset records");
    while (sqlite3_step(query) == SQLITE_ROW) {
        value += QByteArray::number(sqlite3_column_int64(query,0));
        value += reinterpret_cast<const char *>(sqlite3_column_text(query,1));
        value += QByteArray::number(sqlite3_column_int64(query,2));
        value += QByteArray(static_cast<const char *>(sqlite3_column_blob(query,3)), sqlite3_column_bytes(query,3));
        value += QByteArray(static_cast<const char *>(sqlite3_column_blob(query,4)), sqlite3_column_bytes(query,4));
    }
    sqlite3_finalize(query); sqlite3_close(db); return value;
}
void createWorking(const QString &path, const Document &document, SerializationLimits limits = {})
{
    DocumentFile file;
    const auto created = file.create(path.toStdString(), document, limits);
    expect(created.ok(), "create working fixture: " + created.message);
}
void expectWorking(const QString &path, const Document &expected)
{
    DocumentFile file;
    const auto opened = file.open(path.toStdString());
    expect(opened.ok() && file.document(), "open upgraded working file: " + opened.message);
    if (file.document()) {
        expect(bytes(*file.document()) == bytes(expected), "working upgrade preserves the full native document");
        expect(file.document()->infiniteCanvas.chunkSize == expected.infiniteCanvas.chunkSize,
               "working upgrade retains inactive canvas settings omitted from snapshots");
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR) + "/upgrade-XXXXXX");
    expect(directory.isValid(), "private test directory");
    const auto root = directory.path();
    expect(run({"--help"}, root).code == 0, "help needs no desktop session");
    for (const auto &args : {QStringList{}, QStringList{"--unknown"}, QStringList{"-platform","offscreen"},
                             QStringList{"a","b","c"}, QStringList{"--help","a"}}) {
        expect(run(args, root).code == 2, "invalid arguments exit 2 without Qt consuming paths/options");
    }
    for (std::uint16_t minor = 0; minor < CurrentFormatMinor; ++minor) {
        auto source = fixture(minor);
        const auto path = directory.filePath(QString("legacy-%1.iisc").arg(minor));
        write(path, bytes(source));
        const auto result = run({path}, root);
        expect(result.code == 0, "upgrade snapshot 1." + std::to_string(minor) + ": " + result.error.toStdString());
        expect(read(path) == bytes(current(source)), "all legacy snapshot fields become canonical current fields");
        for (FrameIndex frame = 0; frame < 2; ++frame) {
            const auto before = renderFrame(source, frame);
            const auto after = renderFrame(current(source), frame);
            expect(before.ok() && after.ok() && before.pixels.pixels == after.pixels.pixels,
                   "migration preserves every rendered source state");
        }
        const auto working = directory.filePath(QString("working-%1.iisc").arg(minor));
        createWorking(working, source);
        const auto assets = assetRecords(working);
        const auto result2 = run({working}, root);
        expect(result2.code == 0, "upgrade working model 1." + std::to_string(minor) + ": " + result2.error.toStdString());
        expectWorking(working, current(source));
        expect(assetRecords(working) == assets, "format-only working upgrades do not rewrite asset records");
        expect(scalar(working, "SELECT revision FROM canvas_state") == 1, "working upgrade advances storage revision once");
    }
    const auto rich = richFixture();
    for (bool working : {false, true}) {
        const auto input = directory.filePath(working ? "rich-working.iisc" : "rich-snapshot.iisc");
        const auto output = directory.filePath(working ? "new-working.iisc" : "new-snapshot.iisc");
        if (working) { createWorking(input, rich); } else { write(input, bytes(rich)); }
        const auto original = read(input);
        const auto result = run({input, output}, root);
        expect(result.code == 0, "rich output migration succeeds: " + result.error.toStdString());
        expect(read(input) == original, "explicit output leaves source bytes intact");
        if (working) { expectWorking(output, current(rich)); }
        else { expect(read(output) == bytes(current(rich)), "snapshot preserves artboards, motion, media and authorship"); }
        expect(run({input, output}, root).code == 1 && read(input) == original,
               "existing output is never overwritten");
        const auto upgraded = run({input}, root);
        expect(upgraded.code == 0, "rich in-place migration succeeds");
        if (working) { expectWorking(input, current(rich)); }
        else { expect(read(input) == bytes(current(rich)), "rich snapshot upgrades exactly"); }
        const auto latest = read(input);
        const auto revision = working ? scalar(input, "SELECT revision FROM canvas_state") : 0;
        expect(run({input}, root).code == 0 && read(input) == latest,
               "already current input is a byte-preserving no-op");
        if (working) { expect(scalar(input, "SELECT revision FROM canvas_state") == revision, "no-op retains revision"); }
    }
    {
        const auto path = directory.filePath("-한글 drawing.iisc");
        write(path, bytes(fixture(0)));
        expect(run({"--", "-한글 drawing.iisc"}, root).code == 0, "dashed UTF-8 path with spaces upgrades");
        expect(read(path) == bytes(current(fixture(0))), "relative Unicode path contains upgraded content");
    }
    {
        const auto input = directory.filePath("bad.iisc");
        auto invalid = bytes(fixture(19)); invalid[invalid.size()-1] ^= 1;
        write(input, invalid);
        const auto output = directory.filePath("bad-output.iisc");
        expect(run({input}, root).code == 1 && read(input) == invalid, "checksum failure preserves source");
        expect(run({input,output}, root).code == 1 && !QFile::exists(output), "corruption never publishes output");
        const auto future = directory.filePath("future.iisc");
        invalid = bytes(fixture(19)); invalid[10] = char(CurrentFormatMinor + 1);
        write(future, invalid);
        expect(run({future}, root).code == 1 && read(future) == invalid, "future snapshot is rejected without mutation");
        const auto foreign = directory.filePath("foreign.iisc"); write(foreign, "unrecognized format");
        expect(run({foreign}, root).code == 1 && read(foreign) == "unrecognized format", "foreign input is preserved");
        expect(run({directory.filePath("missing.iisc")}, root).code == 1, "missing input fails");
        expect(run({future, directory.filePath("wrong.txt")}, root).code == 2, "output extension is checked");
    }
    {
        const auto input = directory.filePath("aliases.iisc"); write(input, bytes(fixture(19)));
        const auto link = directory.filePath("symbolic.iisc");
        std::error_code error;
        std::filesystem::create_symlink(std::filesystem::path(input.toStdU16String()),
                                       std::filesystem::path(link.toStdU16String()), error);
        if (!error) {
            expect(run({link}, root).code == 1 && read(input) == bytes(fixture(19)), "symbolic input cannot replace its target");
            expect(run({input,link}, root).code == 1, "symbolic output alias is rejected");
        }
    }
    for (const auto command : {"PRAGMA user_version=2", "UPDATE canvas_records SET digest=x'00' WHERE kind=0"}) {
        const auto path = directory.filePath(QString("invalid-working-%1.iisc").arg(failures) + QString::number(command[0]));
        createWorking(path, rich); sql(path, command);
        sql(path, "PRAGMA journal_mode=WAL");
        const auto original = read(path);
        DocumentFile invalid;
        expect(!invalid.open(path.toStdString()).ok() && read(path) == original,
               "direct working-file open rejects invalid records before changing WAL journaling");
        expect(run({path}, root).code == 1 && read(path) == original, "unsupported/corrupt SQLite input is never mutated");
    }
    {
        const auto path = directory.filePath("wal-source.iisc");
        Document source; source.formatVersion = {1,0}; source.extent = {2,2};
        createWorking(path, source);
        sqlite3 *database = nullptr; sqlite3_stmt *query = nullptr;
        expect(sqlite3_open_v2(path.toUtf8().constData(), &database, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
               "open source WAL session");
        expect(sqlite3_exec(database, "PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0", nullptr,nullptr,nullptr) == SQLITE_OK,
               "enable WAL fixture without checkpointing");
        expect(sqlite3_prepare_v2(database, "SELECT data FROM canvas_records WHERE kind=0", -1,&query,nullptr) == SQLITE_OK
            && sqlite3_step(query) == SQLITE_ROW, "read original header record");
        QByteArray header(static_cast<const char *>(sqlite3_column_blob(query,0)), sqlite3_column_bytes(query,0));
        sqlite3_finalize(query); query = nullptr;
        // Storage header: version u16/u16, chunk size i32, then canvas width i32.
        header[8] = 3;
        const auto digest = QCryptographicHash::hash(header, QCryptographicHash::Sha256);
        expect(sqlite3_prepare_v2(database, "UPDATE canvas_records SET data=?,digest=? WHERE kind=0", -1,&query,nullptr) == SQLITE_OK,
               "prepare committed WAL-only change");
        sqlite3_bind_blob(query,1,header.constData(),int(header.size()),SQLITE_TRANSIENT);
        sqlite3_bind_blob(query,2,digest.constData(),int(digest.size()),SQLITE_TRANSIENT);
        expect(sqlite3_step(query) == SQLITE_DONE, "commit source width only in WAL");
        sqlite3_finalize(query); query = nullptr;
        source.extent.width = 3;
        const auto original = read(path), wal = read(path + "-wal");
        const auto output = directory.filePath("wal-upgraded.iisc");
        const auto result = run({path,output}, root);
        expect(result.code == 0, "output migration includes committed WAL: " + result.error.toStdString());
        expectWorking(output, current(source));
        expect(read(path) == original && read(path + "-wal") == wal,
               "source main database and WAL remain byte-identical during output migration");
        expect(sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr,nullptr,nullptr) == SQLITE_OK, "hold external writer lock");
        expect(run({path},root).code == 1 && read(path) == original && read(path + "-wal") == wal,
               "in-place migration refuses a locked working file without losing WAL content");
        sqlite3_exec(database, "ROLLBACK", nullptr,nullptr,nullptr);
        sqlite3_close(database);
    }
    {
        const auto path = directory.filePath("locked-snapshot.iisc");
        const auto original = bytes(fixture(19)); write(path, original);
        QLockFile lock(path + ".iisacc-lock");
        expect(lock.tryLock(0), "hold provider snapshot mutation lock");
        expect(run({path},root).code == 1 && read(path) == original,
               "snapshot publication lock failure leaves original bytes intact");
    }
    {
        DocumentFile unopened;
        expect(upgradeIiscFile("").code == DocumentFileCode::InvalidPath, "native upgrade API validates input paths");
        const auto native = directory.filePath("native-api.iisc");
        write(native, bytes(fixture(0)));
        const auto nativeResult = upgradeIiscFile(native.toStdString());
        expect(nativeResult.ok() && nativeResult.changed && nativeResult.sourceVersion.minor == 0
            && nativeResult.outputPath == native.toStdString() && read(native) == bytes(current(fixture(0))),
            "standard C++ public API exposes the same atomic file upgrade");
        const auto noOp = upgradeIiscFile(native.toStdString());
        expect(noOp.ok() && !noOp.changed && noOp.sourceVersion.minor == CurrentFormatMinor, "native file API is idempotent");
        expect(unopened.upgradeFormat().code == DocumentFileCode::NotOpen, "format API requires an open file");
        const auto path = directory.filePath("atomic-upgrade.iisc");
        auto legacy = fixture(19); createWorking(path, legacy);
        const auto original = read(path);
        SerializationLimits limits;
        limits.maximumContainerBytes = std::uint64_t(scalar(path, "SELECT sum(length(data)) FROM canvas_records")) + IiscHeaderSize - 4;
        DocumentFile limited;
        expect(limited.open(path.toStdString(), limits).ok(), "original document fits exact byte budget");
        const auto rejected = limited.upgradeFormat();
        expect(rejected.code == DocumentFileCode::LimitExceeded && limited.revision() == 0
            && limited.document()->formatVersion.minor == 19 && read(path) == original,
            "upgrade size failure rolls back version, revision and disk bytes");
        limited.close();
        DocumentFile first, second;
        expect(first.open(path.toStdString()).ok() && second.open(path.toStdString()).ok(), "two upgrade sessions open");
        expect(second.upgradeFormat().changed, "first successful transaction advances the format");
        expect(first.upgradeFormat().code == DocumentFileCode::Conflict && first.revision() == 0,
               "stale migration cannot overwrite another commit");
        const auto after = read(path);
        expect(second.upgradeFormat().ok() && !second.lastResult().changed && second.revision() == 1 && read(path) == after,
               "current format API is a no-op");
    }
    expect(!QDir(root).entryList({".iisc-input-*", ".iifile-stage-*", "*.iisacc-lock"}, QDir::AllEntries | QDir::Hidden).size(),
           "successful and rejected migrations remove private temporary files");
    return failures ? 1 : 0;
}
