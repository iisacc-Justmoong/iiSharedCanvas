#include <iiSharedCanvas.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <sqlite3.h>

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
    process.start(QStringLiteral(IISHAREDCANVAS_EXPORT_TIMELINE_EXECUTABLE), arguments);
    const bool finished = process.waitForStarted(10000) && process.waitForFinished(40000);
    expect(finished, "timeline export CLI finishes: " + process.errorString().toStdString());
    if (!finished) { process.kill(); process.waitForFinished(1000); }
    expect(process.exitStatus() == QProcess::NormalExit, "timeline export CLI must not crash");
    return {finished ? process.exitCode() : -1, process.readAllStandardOutput(), process.readAllStandardError()};
}

Document document(std::uint32_t color = 0xff112233U, bool withVector = true)
{
    Document result;
    result.extent = {3, 2};
    result.timeline.frameCount = 6;
    result.assets.emplace_back(RasterAsset{"first", makeRasterLayer(3, 2, color)});
    result.assets.emplace_back(RasterAsset{"later", makeRasterLayer(3, 2, 0xff445566U)});
    BitmapLayer bitmap;
    bitmap.properties.id = "bitmap";
    bitmap.properties.name = "Bitmap <one> & two";
    bitmap.source = KeyframedSource{{0, 3}};
    result.layers.emplace_back(bitmap);
    result.frames = {{0, {{"bitmap", "first"}}}, {3, {{"bitmap", "later"}}}};
    if (withVector) {
        VectorPath path;
        path.commands = {MoveTo{{0, 0}}, LineTo{{1, 0}}, LineTo{{1, 2}}, LineTo{{0, 2}}, ClosePath{}};
        path.fill = SolidPaint{0xffff0000U};
        result.assets.emplace_back(VectorAsset{"shape", {1, 2}, {path}});
        VectorLayer vector;
        vector.properties.id = "vector";
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

void expectPackage(const QString &directory, const Document &source, const QString &name = "iisc Timeline")
{
    // QProcess 6.8 encodes Unix arguments with QFile::encodeName. On macOS
    // that decomposes Unicode to NFD before the utility receives argv.
#if defined(Q_OS_DARWIN)
    const auto argumentName = name.normalized(QString::NormalizationForm_D);
#else
    const auto &argumentName = name;
#endif
    for (const auto &filename : {"timeline.xml", "timeline.fcpxml"}) {
        QXmlStreamReader xml(read(QDir(directory).filePath(filename)));
        QStringList elementPath;
        QString sequenceName;
        int sequenceNames = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                elementPath.push_back(xml.name().toString());
                if (elementPath == QStringList{"xmeml", "sequence", "name"}) {
                    sequenceName = xml.readElementText();
                    ++sequenceNames;
                    elementPath.removeLast(); // readElementText consumed this end element.
                } else if (elementPath == QStringList{"fcpxml", "project"}) {
                    sequenceName = xml.attributes().value("name").toString();
                    ++sequenceNames;
                }
            } else if (xml.isEndElement() && !elementPath.empty()) { elementPath.removeLast(); }
        }
        expect(!xml.hasError() && sequenceNames == 1 && sequenceName == argumentName,
               std::string(filename) + " preserves the received sequence name in its canonical XML location: "
               + xml.errorString().toStdString());
    }
    QJsonParseError error;
    const auto manifest = QJsonDocument::fromJson(read(directory + "/manifest.json"), &error);
    expect(error.error == QJsonParseError::NoError && manifest.isObject()
           && manifest.object()["frameCount"].toInt() == int(source.timeline.frameCount)
           && manifest.object()["sequenceName"].toString() == argumentName,
           "package manifest preserves the complete native timeline duration and received sequence name");
    const auto nativeBytes = read(directory + "/source.iisc");
    const auto native = decodeIisc({reinterpret_cast<const std::uint8_t *>(nativeBytes.constData()),
                                    std::size_t(nativeBytes.size())});
    expect(native.ok(), "package includes a valid native editable source snapshot");
    if (native.ok()) {
        expect(encodeIisc(native.document).bytes == encodeIisc(source).bytes,
               "source snapshot preserves all native assets, layers, names, and keyframes");
    }
    const auto media = QDir(directory + "/media").entryList({"*.png"}, QDir::Files);
    expect(media.size() >= (source.layers.size() == 2 ? 3 : 2), "keyed bitmap states and vector tracks have independent PNG media");
    for (const auto &filename : media) {
        BitmapImportOptions options;
        options.extendedCodecs = false;
        const auto decoded = importBitmap((directory + "/media/" + filename).toStdString(), options);
        expect(decoded.ok() && decoded.asset.pixels.width == source.extent.width
               && decoded.asset.pixels.height == source.extent.height,
               "each generated timeline image is a complete canvas-sized bounded PNG");
    }
}

void expectCleanTemporaryDirectory(const QString &directory)
{
    const auto leftovers = QDir(directory).entryList({".iisc-input-*", ".iisc-timeline-*"},
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    expect(leftovers.empty(), "CLI removes private SQLite backups and package staging after success and failure");
}

void success(const QString &directory)
{
    const auto value = document();
    const auto source = QDir(directory).filePath(QStringLiteral("원본 with spaces.iisc"));
    const auto output = QDir(directory).filePath(QStringLiteral("결과 with spaces"));
    const auto name = QStringLiteral("Sequence 한글 & <clips>");
    snapshot(source, value);
    const auto sourceHash = hash(source);
    auto result = run({source, output, "--name", name}, directory, true);
    expect(result.code == 0 && result.output.contains("Exported") && result.error.contains("vector"),
           "native timeline exports headlessly with Unicode paths and explicit vector warning: " + result.error.toStdString());
    if (result.code == 0) { expectPackage(output, value, name); }
    expect(hash(source) == sourceHash, "binary snapshot source SHA-256 remains unchanged");

    const auto manifestHash = hash(output + "/manifest.json");
    result = run({source, output}, directory);
    expect(result.code == 1 && result.error.contains("AlreadyExists")
           && hash(output + "/manifest.json") == manifestHash && hash(source) == sourceHash,
           "existing package collision preserves both destination and source");

    const auto workingSource = QDir(directory).filePath("readonly-working.iisc");
    workingFile(workingSource, value);
    const auto workingHash = hash(workingSource);
    expect(QFile::setPermissions(workingSource, QFileDevice::ReadOwner | QFileDevice::ReadUser
                                | QFileDevice::ReadGroup | QFileDevice::ReadOther), "make the source working file read-only");
    result = run({workingSource, "working-package"}, directory);
    expect(result.code == 0 && hash(workingSource) == workingHash, "read-only working-file export preserves source bytes");
    if (result.code == 0) { expectPackage(QDir(directory).filePath("working-package"), value); }
    snapshot(QDir(directory).filePath("-source.iisc"), value);
    result = run({"--name", "-sequence", "--", "-source.iisc", "-package"}, directory);
    expect(result.code == 0, "-- supports dashed paths and a dashed --name value");
    if (result.code == 0) { expectPackage(QDir(directory).filePath("-package"), value, "-sequence"); }
    result = run({source, "trailing-separator/"}, directory);
    expect(result.code == 0, "new output directory may end in a path separator");
    if (result.code == 0) { expectPackage(QDir(directory).filePath("trailing-separator"), value); }
    expectCleanTemporaryDirectory(directory);
}

bool sql(sqlite3 *database, const std::string &statement)
{
    const auto result = sqlite3_exec(database, statement.c_str(), nullptr, nullptr, nullptr);
    expect(result == SQLITE_OK, "configure owned SQLite fixture: " + std::string(sqlite3_errmsg(database)));
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
           "open owned WAL writer fixture");
    if (!database) { return; }
    sql(database, "PRAGMA journal_mode=WAL");
    sql(database, "PRAGMA wal_autocheckpoint=0");
    sqlite3_stmt *attach = nullptr;
    expect(sqlite3_prepare_v2(database, "ATTACH DATABASE ? AS updated", -1, &attach, nullptr) == SQLITE_OK,
           "prepare canonical updated fixture attachment");
    if (attach) {
        const auto path = updated.toUtf8();
        sqlite3_bind_text(attach, 1, path.constData(), path.size(), SQLITE_TRANSIENT);
        expect(sqlite3_step(attach) == SQLITE_DONE, "attach canonical updated source");
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
    const auto result = run({source, "wal-package"}, directory);
    expect(result.code == 0, "online backup includes committed WAL timeline state: " + result.error.toStdString());
    if (result.code == 0) { expectPackage(QDir(directory).filePath("wal-package"), expected); }
    expect(hash(source) == mainHash && hash(source + "-wal") == walHash,
           "timeline export never rewrites source database or WAL payload");
    sql(database, "BEGIN EXCLUSIVE");
    const auto activeWriter = run({source, "wal-active-writer"}, directory);
    expect(activeWriter.code == 0, "WAL writer can remain active during the read-only export");
    if (activeWriter.code == 0) { expectPackage(QDir(directory).filePath("wal-active-writer"), expected); }
    sql(database, "ROLLBACK");
    sqlite3_close(database);
    expectCleanTemporaryDirectory(directory);
}

void failure(const QString &directory)
{
    for (const QStringList &arguments : std::vector<QStringList>{
             {}, {"one.iisc"}, {"one.iisc", "package", "extra"}, {"--unknown"},
             {"--name"}, {"--name", "", "one.iisc", "package"},
             {"--name", "one", "--name", "two", "one.iisc", "package"},
             {"--name=one", "one.iisc", "package"}, {"--overwrite", "one.iisc", "package"},
             {"--help", "one.iisc", "package"}, {"-platform", "offscreen", "one.iisc", "package"},
             {"https://example.com/source.iisc", "package"}, {"one.iisc", "https://example.com/package"}}) {
        const auto result = run(arguments, directory);
        expect(result.code == 2 && result.error.contains("Usage:") && result.output.isEmpty(),
               "invalid timeline CLI grammar returns 2 without creating an output");
    }
    expect(run({"--help"}, directory).code == 0 && run({"-h"}, directory).code == 0,
           "standalone help succeeds");
    write(QDir(directory).filePath("corrupt.iisc"), "not a native canvas");
    auto result = run({"corrupt.iisc", "corrupt-package"}, directory);
    expect(result.code == 1 && !QFile::exists(QDir(directory).filePath("corrupt-package")),
           "corrupt source never publishes a partial package");
    snapshot(QDir(directory).filePath("valid.iisc"), document());
    const auto sourceHash = hash(QDir(directory).filePath("valid.iisc"));
    auto corruptSnapshot = read(QDir(directory).filePath("valid.iisc"));
    corruptSnapshot.back() = char(std::uint8_t(corruptSnapshot.back()) ^ 1);
    write(QDir(directory).filePath("invalid-checksum.iisc"), corruptSnapshot);
    result = run({"invalid-checksum.iisc", "invalid-checksum-package"}, directory);
    expect(result.code == 1 && result.error.contains("InvalidData")
           && !QFile::exists(QDir(directory).filePath("invalid-checksum-package")),
           "native input checksum failure never publishes a partial package");
    expect(QDir(directory).mkdir("existing-empty"), "create existing empty destination fixture");
    result = run({"valid.iisc", "existing-empty"}, directory);
    expect(result.code == 1 && result.error.contains("AlreadyExists")
           && QDir(QDir(directory).filePath("existing-empty")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).empty(),
           "even an existing empty output directory is preserved");
    result = run({"valid.iisc", "missing-parent/package"}, directory);
    expect(result.code == 1 && result.error.contains("IoError")
           && !QFile::exists(QDir(directory).filePath("missing-parent")), "missing parent is not implicitly created");
    result = run({"valid.iisc", "valid.iisc"}, directory);
    expect(result.code == 1 && result.error.contains("AlreadyExists")
           && hash(QDir(directory).filePath("valid.iisc")) == sourceHash, "source cannot become the output package");
    expect(QFile::link(QDir(directory).filePath("valid.iisc"), QDir(directory).filePath("source-alias")),
           "create source alias fixture");
    result = run({"valid.iisc", "source-alias"}, directory);
    expect(result.code == 1 && hash(QDir(directory).filePath("valid.iisc")) == sourceHash,
           "symbolic output cannot redirect package publication to source");
    auto unsupported = document();
    unsupported.timeline.frameRate = {123, 7};
    snapshot(QDir(directory).filePath("unsupported.iisc"), unsupported);
    result = run({"unsupported.iisc", "unsupported-package"}, directory);
    expect(result.code == 1 && result.error.contains("UnsupportedFeature")
           && !QFile::exists(QDir(directory).filePath("unsupported-package")),
           "unsupported interchange rate fails without publishing a directory");
    auto invalidXml = document();
    layerProperties(invalidXml.layers[0]).name = std::string("bad\x01", 4);
    snapshot(QDir(directory).filePath("invalid-xml.iisc"), invalidXml);
    result = run({"invalid-xml.iisc", "invalid-xml-package"}, directory);
    expect(result.code == 1 && result.error.contains("InvalidArgument")
           && !QFile::exists(QDir(directory).filePath("invalid-xml-package")),
           "invalid XML metadata fails without a partial package");
    const auto lockedSource = QDir(directory).filePath("locked.iisc");
    workingFile(lockedSource, document(0xff223344U, false));
    sqlite3 *locker = nullptr;
    expect(sqlite3_open_v2(lockedSource.toUtf8().constData(), &locker, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
           "open owned rollback journal lock fixture");
    if (locker) {
        sql(locker, "BEGIN EXCLUSIVE");
        const auto locked = run({lockedSource, "locked-package"}, directory);
        expect(locked.code == 1 && !QFile::exists(QDir(directory).filePath("locked-package")),
               "unavailable read snapshot fails promptly without output");
        sql(locker, "ROLLBACK");
        sqlite3_close(locker);
    }
    expectCleanTemporaryDirectory(directory);
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    QDir().mkpath(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR));
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/timeline-export-cli-XXXXXX"));
    expect(directory.isValid(), "create timeline CLI fixtures under build only");
    if (directory.isValid()) {
        success(directory.path());
        walSnapshot(directory.path());
        failure(directory.path());
    }
    return failures == 0 ? 0 : 1;
}
