#include <iiSharedCanvas.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <sqlite3.h>

#include <algorithm>
#include <iostream>

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
           "write fixture " + path.toStdString());
}

QByteArray hash(const QString &path)
{
    return QCryptographicHash::hash(read(path), QCryptographicHash::Sha256);
}

struct ProcessResult {
    int code = -1;
    QByteArray output;
    QByteArray error;
};

ProcessResult run(const QStringList &arguments, const QString &directory, bool withoutPlatform = false)
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    if (withoutPlatform) { environment.remove("QT_QPA_PLATFORM"); }
    else { environment.insert("QT_QPA_PLATFORM", "offscreen"); }
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(directory);
    process.start(QStringLiteral(IISHAREDCANVAS_EXPORT_PSD_EXECUTABLE), arguments);
    const bool finished = process.waitForStarted(10000) && process.waitForFinished(40000);
    expect(finished, "PSD export CLI finishes: " + process.errorString().toStdString());
    if (!finished) { process.kill(); process.waitForFinished(1000); }
    expect(process.exitStatus() == QProcess::NormalExit, "PSD export CLI must not crash");
    return {finished ? process.exitCode() : -1, process.readAllStandardOutput(), process.readAllStandardError()};
}

Document document(std::uint32_t background = 0xff0000ffU, bool withVector = true)
{
    Document result;
    result.extent = {3, 2};
    result.assets.emplace_back(RasterAsset{"background", makeRasterLayer(3, 2, background)});
    BitmapLayer bitmap;
    bitmap.properties.id = "background-layer";
    bitmap.properties.name = "Background";
    bitmap.source = StaticSource{"background"};
    result.layers.emplace_back(bitmap);
    if (withVector) {
        VectorPath path;
        path.commands = {MoveTo{{0, 0}}, LineTo{{1, 0}}, LineTo{{1, 2}}, LineTo{{0, 2}}, ClosePath{}};
        path.fill = SolidPaint{0xffff0000U};
        result.assets.emplace_back(VectorAsset{"shape", {1, 2}, {path}});
        VectorLayer vector;
        vector.properties.id = "shape-layer";
        vector.properties.name = "Vector 한글";
        vector.properties.transform.translationX = 1;
        vector.source = StaticSource{"shape"};
        result.layers.emplace_back(vector);
    }
    return result;
}

void snapshot(const QString &path, const Document &value)
{
    const auto encoded = encodeIisc(value);
    expect(encoded.ok(), "encode snapshot source fixture");
    write(path, QByteArray(reinterpret_cast<const char *>(encoded.bytes.data()), qsizetype(encoded.bytes.size())));
}

void workingFile(const QString &path, const Document &value)
{
    DocumentFile file;
    const auto created = file.create(path.toStdString(), value);
    expect(created.ok(), "create working-file source fixture: " + created.message);
}

void expectPsd(const QString &path, const Document &source)
{
    const auto bytes = read(path);
    expect(bytes.startsWith("8BPS"), "CLI publishes a PSD file");
    if (bytes.size() < 26) { return; }
    const auto u16 = [&](qsizetype offset) {
        return std::uint16_t((std::uint16_t(std::uint8_t(bytes[offset])) << 8)
                             | std::uint16_t(std::uint8_t(bytes[offset + 1])));
    };
    const auto u32 = [&](qsizetype offset) {
        return (std::uint32_t(u16(offset)) << 16) | u16(offset + 2);
    };
    qsizetype position = 26;
    for (int block = 0; block < 3; ++block) {
        if (bytes.size() - position < 4) { expect(false, "PSD has complete section headers"); return; }
        const auto length = u32(position);
        position += 4;
        if (std::uint64_t(length) > std::uint64_t(bytes.size() - position)) {
            expect(false, "PSD sections stay within output bounds"); return;
        }
        if (block == 2) {
            if (length < 6) { expect(false, "PSD has editable layer records"); return; }
            const auto count = std::int16_t(u16(position + 4));
            expect(std::size_t(count < 0 ? -int(count) : count) == source.layers.size(),
                   "independent PSD record count preserves the source layers");
        }
        position += length;
    }
    const auto expected = renderFrame(source, 0).pixels;
    if (bytes.size() - position != 2 + qsizetype(expected.pixels.size() * 4)
        || u16(12) != 4 || u16(22) != 8 || u16(24) != 3 || u16(position) != 0) {
        expect(false, "PSD exposes the documented raw 8-bit RGBA merged preview"); return;
    }
    position += 2;
    std::vector<std::uint32_t> merged(expected.pixels.size(), 0);
    for (const int shift : {16, 8, 0, 24}) {
        for (auto &pixel : merged) { pixel |= std::uint32_t(std::uint8_t(bytes[position++])) << shift; }
    }
    expect(merged == expected.pixels, "independent PSD merged preview matches native frame-zero pixels");
    const bool hasVector = std::any_of(source.layers.begin(), source.layers.end(), [](const Layer &layer) {
        return std::holds_alternative<VectorLayer>(layer);
    });
    if (hasVector) {
        expect(bytes.contains("%PDF-") && (bytes.contains("8BIMSoLd") || bytes.contains("8BIMSoLE")),
               "vector source remains an embedded PDF Smart Object, not only raster pixels");
        for (const auto &layer : source.layers) {
            QByteArray encodedName;
            for (const QChar character : QString::fromStdString(layerProperties(layer).name)) {
                encodedName.append(char(character.unicode() >> 8));
                encodedName.append(char(character.unicode()));
            }
            expect(bytes.contains(encodedName), "PSD contains the original Unicode layer name");
        }
        const auto conservative = decodeLayeredDocument({reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                                                         std::size_t(bytes.size())});
        expect(conservative.result.code == MediaIoCode::UnsupportedFeature,
               "raster-only PSD importer must not discard exported vector Smart Object semantics");
        return;
    }
    const auto restored = decodeLayeredDocument({reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                                                 std::size_t(bytes.size())});
    expect(restored.ok(), "CLI PSD reimports editable layers: " + restored.result.message);
    if (!restored.ok()) { return; }
    expect(restored.document.layers.size() == source.layers.size(), "PSD preserves each source layer separately");
    expect(renderFrame(restored.document, 0).pixels.pixels == renderFrame(source, 0).pixels.pixels,
           "PSD export frame zero matches the native source pixels");
    for (std::size_t index = 0; index < source.layers.size() && index < restored.document.layers.size(); ++index) {
        expect(layerProperties(restored.document.layers[index]).name == layerProperties(source.layers[index]).name,
               "PSD export preserves layer names and order");
    }
}

void expectCleanTemporaryDirectory(const QString &directory)
{
    const auto leftovers = QDir(directory).entryList({".iisc-export-psd-*", ".iisc-input-*"},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    expect(leftovers.empty(), "CLI removes its private SQLite backup directory after success and failure");
}

void success(const QString &directory)
{
    const auto source = QDir(directory).filePath(QStringLiteral("원본 with spaces.iisc"));
    const auto destination = QDir(directory).filePath(QStringLiteral("결과 with spaces.psd"));
    const auto value = document();
    workingFile(source, value);
    const auto sourceHash = hash(source);
    expect(QFile::setPermissions(source, QFileDevice::ReadOwner | QFileDevice::ReadUser
                                | QFileDevice::ReadGroup | QFileDevice::ReadOther), "make the original source read-only");
    auto result = run({source, destination}, directory, true);
    expect(result.code == 0 && result.output.contains("Exported") && result.output.contains("psd"),
           "read-only working source with Unicode paths exports headlessly: " + result.error.toStdString());
    if (result.code == 0) { expectPsd(destination, value); }
    expect(hash(source) == sourceHash, "working source SHA-256 remains unchanged");
    const auto destinationHash = hash(destination);
    result = run({source, destination}, directory);
    expect(result.code == 1 && result.error.contains("AlreadyExists") && hash(destination) == destinationHash,
           "default collision preserves the existing PSD");
    result = run({"--overwrite", source, destination}, directory);
    expect(result.code == 0 && hash(source) == sourceHash, "explicit overwrite only replaces the PSD destination");
    expectCleanTemporaryDirectory(directory);

    const auto snapshotPath = QDir(directory).filePath("snapshot.iisc");
    snapshot(snapshotPath, value);
    const auto snapshotHash = hash(snapshotPath);
    result = run({snapshotPath, "snapshot.psd"}, directory);
    expect(result.code == 0 && hash(snapshotPath) == snapshotHash, "native snapshot input exports without mutation");
    if (result.code == 0) { expectPsd(QDir(directory).filePath("snapshot.psd"), value); }

    auto animated = document(0xff112233U, false);
    animated.timeline.frameCount = 2;
    animated.assets.emplace_back(RasterAsset{"later", makeRasterLayer(3, 2, 0xff445566U)});
    layerSource(animated.layers[0]) = KeyframedSource{{0, 1}};
    animated.frames = {{0, {{"background-layer", "background"}}}, {1, {{"background-layer", "later"}}}};
    snapshot(QDir(directory).filePath("animated.iisc"), animated);
    result = run({"animated.iisc", "frame-zero.psd"}, directory);
    expect(result.code == 0, "animated source exports documented frame zero: " + result.error.toStdString());
    if (result.code == 0) { expectPsd(QDir(directory).filePath("frame-zero.psd"), animated); }

    snapshot(QDir(directory).filePath("-source.iisc"), value);
    result = run({"--", "-source.iisc", "-output.psd"}, directory);
    expect(result.code == 0, "-- supports dashed source and destination names");
}

bool sql(sqlite3 *database, const std::string &statement)
{
    const auto result = sqlite3_exec(database, statement.c_str(), nullptr, nullptr, nullptr);
    expect(result == SQLITE_OK, "configure WAL fixture: " + std::string(sqlite3_errmsg(database)));
    return result == SQLITE_OK;
}

void walSnapshot(const QString &directory)
{
    const auto source = QDir(directory).filePath("live-wal.iisc");
    const auto updated = QDir(directory).filePath("wal-updated.iisc");
    workingFile(source, document(0xff112233U, false));
    const auto expected = document(0xffaabbccU, false);
    workingFile(updated, expected);
    sqlite3 *database = nullptr;
    expect(sqlite3_open_v2(source.toUtf8().constData(), &database, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
           "open owned WAL test writer");
    if (!database) { return; }
    sql(database, "PRAGMA journal_mode=WAL");
    sql(database, "PRAGMA wal_autocheckpoint=0");
    // The second canonical working file supplies valid record data and hashes.
    // Only the owned fixture writer changes source records; the export process is a reader.
    sqlite3_stmt *attach = nullptr;
    expect(sqlite3_prepare_v2(database, "ATTACH DATABASE ? AS updated", -1, &attach, nullptr) == SQLITE_OK,
           "prepare controlled source attachment");
    if (attach) {
        const auto path = updated.toUtf8();
        sqlite3_bind_text(attach, 1, path.constData(), path.size(), SQLITE_TRANSIENT);
        expect(sqlite3_step(attach) == SQLITE_DONE, "attach updated canonical fixture");
        sqlite3_finalize(attach);
    }
    sql(database, "BEGIN IMMEDIATE");
    sql(database, "UPDATE main.canvas_records AS target SET "
                  "data=(SELECT data FROM updated.canvas_records WHERE kind=target.kind AND id=target.id), "
                  "digest=(SELECT digest FROM updated.canvas_records WHERE kind=target.kind AND id=target.id)");
    sql(database, "UPDATE main.canvas_state SET revision=revision+1");
    sql(database, "COMMIT");
    const auto mainHash = hash(source);
    const auto walHash = hash(source + "-wal");
    const auto result = run({source, "wal-result.psd"}, directory);
    expect(result.code == 0, "read-only online backup exports committed WAL pixels: " + result.error.toStdString());
    if (result.code == 0) { expectPsd(QDir(directory).filePath("wal-result.psd"), expected); }
    expect(hash(source) == mainHash && hash(source + "-wal") == walHash,
           "export does not rewrite the source database or WAL bytes");
    expectCleanTemporaryDirectory(directory);
    sql(database, "BEGIN EXCLUSIVE");
    // WAL readers remain legal while a writer owns a transaction; they see the
    // latest committed frame, not partially written future state.
    const auto activeWriter = run({source, "wal-active-writer.psd"}, directory);
    expect(activeWriter.code == 0, "WAL active writer does not expose uncommitted content to export");
    sql(database, "ROLLBACK");
    sqlite3_close(database);
}

void failure(const QString &directory)
{
    for (const QStringList &arguments : std::vector<QStringList>{
             {}, {"one.iisc"}, {"one.iisc", "two.psd", "extra"}, {"--unknown"},
             {"--overwrite", "--overwrite", "one.iisc", "two.psd"},
             {"--overwrite=yes", "one.iisc", "two.psd"},
             {"--help", "one.iisc", "two.psd"}, {"one.iisc", "two.png"},
             {"-platform", "offscreen", "one.iisc", "two.psd"}}) {
        const auto result = run(arguments, directory);
        expect(result.code == 2 && result.error.contains("Usage:") && result.output.isEmpty(),
               "strict CLI usage errors return 2 without exporting");
    }
    expect(run({"--help"}, directory).code == 0 && run({"-h"}, directory).code == 0,
           "standalone help exits successfully");
    write(QDir(directory).filePath("corrupt.iisc"), "not a canvas");
    auto result = run({"corrupt.iisc", "corrupt.psd"}, directory);
    expect(result.code == 1 && !QFile::exists(QDir(directory).filePath("corrupt.psd")),
           "corrupt source fails without a partial output");

    auto tooWide = document(0xff112233U, false);
    tooWide.extent = {30001, 1};
    snapshot(QDir(directory).filePath("wide.iisc"), tooWide);
    write(QDir(directory).filePath("preserved.psd"), "preserved destination");
    const auto destinationHash = hash(QDir(directory).filePath("preserved.psd"));
    result = run({"--overwrite", "wide.iisc", "preserved.psd"}, directory);
    expect(result.code == 1 && hash(QDir(directory).filePath("preserved.psd")) == destinationHash,
           "failed export leaves the old destination byte-exact even with overwrite");
    result = run({"wide.iisc", "absent.psd"}, directory);
    expect(result.code == 1 && !QFile::exists(QDir(directory).filePath("absent.psd")),
           "failed new export never leaves partial PSD data");

    const auto same = QDir(directory).filePath("same.psd");
    snapshot(same, document());
    const auto sameHash = hash(same);
    result = run({"--overwrite", same, same}, directory);
    expect(result.code == 1 && hash(same) == sameHash, "overwrite cannot target the source itself");
    const auto linked = QDir(directory).filePath("source-link.psd");
    expect(QFile::link(same, linked), "create source alias fixture");
    result = run({"--overwrite", same, linked}, directory);
    expect(result.code == 1 && hash(same) == sameHash, "overwrite cannot follow a source alias");
    result = run({"https://example.com/input.iisc", "remote.psd"}, directory);
    expect(result.code != 0 && !QFile::exists(QDir(directory).filePath("remote.psd")),
           "network input paths are rejected without a fetch");
    expectCleanTemporaryDirectory(directory);

    const auto lockedSource = QDir(directory).filePath("locked.iisc");
    workingFile(lockedSource, document(0xff223344U, false));
    sqlite3 *locker = nullptr;
    expect(sqlite3_open_v2(lockedSource.toUtf8().constData(), &locker, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
           "open owned rollback-journal lock fixture");
    if (locker) {
        sql(locker, "BEGIN EXCLUSIVE");
        const auto rejected = run({lockedSource, "locked.psd"}, directory);
        expect(rejected.code == 1 && !QFile::exists(QDir(directory).filePath("locked.psd")),
               "busy rollback-journal source fails promptly without an output");
        sql(locker, "ROLLBACK");
        sqlite3_close(locker);
    }
    const auto sidecarSource = QDir(directory).filePath("sidecar-source.iisc");
    workingFile(sidecarSource, document(0xff223344U, false));
    const auto sidecarHash = hash(sidecarSource);
    expect(QFile::link(same, sidecarSource + "-wal"), "create rejected symbolic WAL fixture");
    result = run({sidecarSource, "sidecar.psd"}, directory);
    expect(result.code == 1 && hash(sidecarSource) == sidecarHash
           && !QFile::exists(QDir(directory).filePath("sidecar.psd")),
           "untrusted sidecar symlinks are not followed by SQLite snapshot creation");
    expectCleanTemporaryDirectory(directory);
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/psd-export-cli-XXXXXX"));
    expect(directory.isValid(), "create export CLI fixtures under build only");
    if (directory.isValid()) {
        success(directory.path());
        walSnapshot(directory.path());
        failure(directory.path());
    }
    return failures == 0 ? 0 : 1;
}
