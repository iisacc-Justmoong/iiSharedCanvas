#include "IiscInput_p.hpp"

#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <sqlite3.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace iisc_tools {
using namespace iiSharedCanvas;

const char *codeName(MediaIoCode code)
{
    switch (code) {
    case MediaIoCode::None: return "None";
    case MediaIoCode::InvalidArgument: return "InvalidArgument";
    case MediaIoCode::UnsupportedFormat: return "UnsupportedFormat";
    case MediaIoCode::UnsupportedFeature: return "UnsupportedFeature";
    case MediaIoCode::DependencyUnavailable: return "DependencyUnavailable";
    case MediaIoCode::InvalidData: return "InvalidData";
    case MediaIoCode::LimitExceeded: return "LimitExceeded";
    case MediaIoCode::AlreadyExists: return "AlreadyExists";
    case MediaIoCode::IoError: return "IoError";
    case MediaIoCode::Cancelled: return "Cancelled";
    case MediaIoCode::TimedOut: return "TimedOut";
    }
    return "UnknownError";
}

namespace {
struct CloseDatabase {
    void operator()(sqlite3 *database) const { sqlite3_close_v2(database); }
};
using Database = std::unique_ptr<sqlite3, CloseDatabase>;

struct FinishBackup {
    void operator()(sqlite3_backup *backup) const { sqlite3_backup_finish(backup); }
};

void checkSql(int status, sqlite3 *database)
{
    if (status != SQLITE_OK) {
        throw Failure(MediaIoCode::IoError, database ? sqlite3_errmsg(database) : sqlite3_errstr(status));
    }
}

Database openDatabase(const QString &path, int flags)
{
    sqlite3 *raw = nullptr;
    const auto status = sqlite3_open_v2(path.toUtf8().constData(), &raw, flags | SQLITE_OPEN_FULLMUTEX, nullptr);
    Database database(raw);
    checkSql(status, raw);
    checkSql(sqlite3_busy_timeout(raw, 250), raw);
    return database;
}

std::uint64_t scalar(sqlite3 *database, const char *query)
{
    sqlite3_stmt *raw = nullptr;
    checkSql(sqlite3_prepare_v2(database, query, -1, &raw, nullptr), database);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    const auto status = sqlite3_step(raw);
    if (status != SQLITE_ROW || sqlite3_column_type(raw, 0) != SQLITE_INTEGER
        || sqlite3_column_int64(raw, 0) < 0) {
        throw Failure(MediaIoCode::InvalidData, "cannot read bounded SQLite source dimensions");
    }
    return std::uint64_t(sqlite3_column_int64(raw, 0));
}

void backupWorkingFile(const QString &sourcePath, const QString &destinationPath,
                        const MediaLimits &mediaLimits)
{
    std::uint64_t encodedBytes = std::uint64_t(QFileInfo(sourcePath).size());
    for (const auto suffix : {"-wal", "-journal", "-shm"}) {
        const QFileInfo sidecar(sourcePath + suffix);
        if (!sidecar.exists() && !sidecar.isSymLink()) { continue; }
        if (sidecar.isSymLink() || !sidecar.isFile() || sidecar.size() < 0) {
            throw Failure(MediaIoCode::InvalidData, "SQLite source sidecars must be regular local files, not symbolic links");
        }
        if (encodedBytes > mediaLimits.maxInputBytes
            || std::uint64_t(sidecar.size()) > mediaLimits.maxInputBytes - encodedBytes) {
            throw Failure(MediaIoCode::LimitExceeded, "SQLite source and sidecars exceed the input byte budget");
        }
        encodedBytes += std::uint64_t(sidecar.size());
    }
    // No URI flag or immutable mode: the read-only connection must include
    // committed WAL content and may not force a checkpoint or journal-mode change.
    auto source = openDatabase(sourcePath, SQLITE_OPEN_READONLY);
    checkSql(sqlite3_exec(source.get(), "PRAGMA query_only=ON; PRAGMA trusted_schema=OFF; BEGIN",
                         nullptr, nullptr, nullptr), source.get());
    const auto pageSize = scalar(source.get(), "PRAGMA page_size");
    const auto pageCount = scalar(source.get(), "PRAGMA page_count");
    const auto byteLimit = std::min(mediaLimits.maxInputBytes, mediaLimits.maxDecodedBytes);
    if (pageSize == 0 || pageSize > byteLimit || pageCount > byteLimit / pageSize) {
        throw Failure(MediaIoCode::LimitExceeded, "SQLite source snapshot exceeds the input/decoded byte budget");
    }
    QFile placeholder(destinationPath);
    if (!placeholder.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        throw Failure(MediaIoCode::IoError, "cannot create the private working-file snapshot");
    }
    placeholder.close();
    auto destination = openDatabase(destinationPath, SQLITE_OPEN_READWRITE);
    std::unique_ptr<sqlite3_backup, FinishBackup> backup(
        sqlite3_backup_init(destination.get(), "main", source.get(), "main"));
    if (!backup) { throw Failure(MediaIoCode::IoError, sqlite3_errmsg(destination.get())); }
    QElapsedTimer timer;
    timer.start();
    while (true) {
        if (timer.elapsed() >= 30000) {
            throw Failure(MediaIoCode::TimedOut, "SQLite read-only backup exceeded its 30-second time budget");
        }
        const auto status = sqlite3_backup_step(backup.get(), 128);
        const auto currentPages = sqlite3_backup_pagecount(backup.get());
        if (currentPages < 0 || std::uint64_t(currentPages) > byteLimit / pageSize) {
            throw Failure(MediaIoCode::LimitExceeded, "SQLite backup grew beyond its byte budget");
        }
        if (status == SQLITE_DONE) { break; }
        if (status != SQLITE_OK) {
            throw Failure(MediaIoCode::IoError, "cannot obtain a consistent read-only SQLite snapshot: "
                          + std::string(sqlite3_errstr(status)));
        }
    }
    checkSql(sqlite3_backup_finish(backup.release()), destination.get());
    checkSql(sqlite3_exec(source.get(), "ROLLBACK", nullptr, nullptr, nullptr), source.get());
    destination.reset();
    if (QFileInfo(destinationPath).size() < 0
        || std::uint64_t(QFileInfo(destinationPath).size()) > byteLimit) {
        throw Failure(MediaIoCode::LimitExceeded, "private SQLite backup exceeds its byte budget");
    }
}

SerializationLimits serializationLimits(const MediaLimits &mediaLimits, std::uint32_t maxLayers)
{
    SerializationLimits limits;
    limits.maximumContainerBytes = mediaLimits.maxInputBytes;
    limits.maximumCanvasPixels = mediaLimits.maxPixelsPerFrame;
    limits.maximumTotalRasterPixels = mediaLimits.maxDecodedBytes / sizeof(std::uint32_t);
    limits.maximumLayers = maxLayers;
    return limits;
}

} // namespace

Document loadDocument(const QString &path, const QString &outputParent,
                       const MediaLimits &mediaLimits, std::uint32_t maxLayers)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { throw Failure(MediaIoCode::IoError, file.errorString().toStdString()); }
    if (file.size() <= 0) { throw Failure(MediaIoCode::InvalidData, "native input is empty"); }
    if (std::uint64_t(file.size()) > mediaLimits.maxInputBytes) {
        throw Failure(MediaIoCode::LimitExceeded, "native input exceeds the input byte budget");
    }
    const auto prefix = file.peek(16);
    const auto limits = serializationLimits(mediaLimits, maxLayers);
    if (prefix == QByteArray("SQLite format 3\0", 16)) {
        file.close();
        QTemporaryDir directory(QDir(outputParent).filePath(".iisc-input-XXXXXX"));
        if (!directory.isValid()) {
            throw Failure(MediaIoCode::IoError, "cannot create a private snapshot directory beside the destination");
        }
        const auto backup = directory.filePath("source.iisc");
        backupWorkingFile(path, backup, mediaLimits);
        // DocumentFile's authoring connection only ever touches the private
        // consistent backup, not the source database or its journal/WAL.
        DocumentFile reader;
        const auto opened = reader.open(backup.toStdString(), limits);
        if (!opened.ok() || !reader.document()) {
            throw Failure(opened.code == DocumentFileCode::LimitExceeded ? MediaIoCode::LimitExceeded
                          : opened.code == DocumentFileCode::UnsupportedFormat ? MediaIoCode::UnsupportedFormat
                          : MediaIoCode::InvalidData, opened.message);
        }
        return *reader.document();
    }
    if (!prefix.startsWith(QByteArray("IISC\r\n\x1a\n", 8))) {
        throw Failure(MediaIoCode::UnsupportedFormat, "input is not a native iisc snapshot or working file");
    }
    std::vector<std::uint8_t> bytes;
    while (!file.atEnd()) {
        const auto block = file.read(64 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            throw Failure(MediaIoCode::IoError, file.errorString().toStdString());
        }
        if (std::uint64_t(block.size()) > mediaLimits.maxInputBytes - bytes.size()) {
            throw Failure(MediaIoCode::LimitExceeded, "native input grew beyond the input byte budget");
        }
        bytes.insert(bytes.end(), block.begin(), block.end());
    }
    auto decoded = decodeIisc(bytes, limits);
    if (!decoded.ok()) {
        throw Failure(decoded.error.code == IiscErrorCode::LimitExceeded ? MediaIoCode::LimitExceeded
                      : decoded.error.code == IiscErrorCode::UnsupportedVersion ? MediaIoCode::UnsupportedFormat
                      : MediaIoCode::InvalidData, decoded.error.message);
    }
    return std::move(decoded.document);
}

} // namespace iisc_tools
