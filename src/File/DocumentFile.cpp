#include "File/DocumentFile.h"

#include "Serialization/DocumentRecords_p.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace iiSharedCanvas {
namespace {

constexpr int ApplicationId = 0x49495343; // IISC, distinct from other SQLite files.
constexpr int StorageVersion = 1;
static_assert(std::is_nothrow_move_assignable_v<Document>);
constexpr const char *StateSchema =
    "CREATE TABLE canvas_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), "
    "revision INTEGER NOT NULL CHECK(revision>=0))";
constexpr const char *RecordSchema =
    "CREATE TABLE canvas_records (kind INTEGER NOT NULL, id TEXT NOT NULL, "
    "position INTEGER NOT NULL CHECK(position>=0), data BLOB NOT NULL, "
    "digest BLOB NOT NULL, UNIQUE(kind,id))";

using RecordKey = std::pair<int, std::string>;
using detail::DocumentRecord;

class FileFailure final : public std::runtime_error {
public:
    FileFailure(DocumentFileCode value, std::string message)
        : std::runtime_error(std::move(message)), code(value) {}
    DocumentFileCode code;
};

void check(int result, sqlite3 *database)
{
    if (result != SQLITE_OK) {
        throw FileFailure(DocumentFileCode::IoError,
                          database ? sqlite3_errmsg(database) : sqlite3_errstr(result));
    }
}

void execute(sqlite3 *database, const char *sql)
{
    check(sqlite3_exec(database, sql, nullptr, nullptr, nullptr), database);
}

class Database final {
public:
    explicit Database(const std::string &path)
    {
        const int result = sqlite3_open_v2(path.c_str(), &handle,
                                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                                           nullptr);
        if (result != SQLITE_OK) {
            const std::string message = handle ? sqlite3_errmsg(handle) : sqlite3_errstr(result);
            sqlite3_close_v2(handle);
            handle = nullptr;
            throw FileFailure(DocumentFileCode::IoError, message);
        }
        // Busy edits fail promptly, preserving the previous document for retry.
        sqlite3_busy_timeout(handle, 0);
    }
    ~Database() { sqlite3_close_v2(handle); }
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    sqlite3 *handle = nullptr;
};

class Statement final {
public:
    Statement(sqlite3 *database, const char *sql) : m_database(database)
    {
        check(sqlite3_prepare_v2(database, sql, -1, &m_statement, nullptr), database);
    }
    ~Statement() { sqlite3_finalize(m_statement); }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    void integer(int index, sqlite3_int64 value)
    {
        check(sqlite3_bind_int64(m_statement, index, value), m_database);
    }
    void text(int index, const std::string &value)
    {
        check(sqlite3_bind_text(m_statement, index, value.data(),
                               static_cast<int>(value.size()), SQLITE_TRANSIENT), m_database);
    }
    void bytes(int index, const void *data, std::size_t size)
    {
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw FileFailure(DocumentFileCode::LimitExceeded, "record exceeds the SQLite BLOB limit");
        }
        check(size == 0 ? sqlite3_bind_zeroblob(m_statement, index, 0)
                       : sqlite3_bind_blob(m_statement, index, data, static_cast<int>(size),
                                           SQLITE_TRANSIENT), m_database);
    }
    bool row()
    {
        const int result = sqlite3_step(m_statement);
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result != SQLITE_DONE) {
            check(result, m_database);
        }
        return false;
    }
    void done()
    {
        if (row()) {
            throw FileFailure(DocumentFileCode::CorruptFile, "unexpected query result");
        }
    }
    sqlite3_int64 integer(int column) const
    {
        requireType(column, SQLITE_INTEGER);
        return sqlite3_column_int64(m_statement, column);
    }
    std::string text(int column) const
    {
        requireType(column, SQLITE_TEXT);
        return {reinterpret_cast<const char *>(sqlite3_column_text(m_statement, column)),
                static_cast<std::size_t>(sqlite3_column_bytes(m_statement, column))};
    }
    std::span<const std::uint8_t> bytes(int column) const
    {
        requireType(column, SQLITE_BLOB);
        return {static_cast<const std::uint8_t *>(sqlite3_column_blob(m_statement, column)),
                static_cast<std::size_t>(sqlite3_column_bytes(m_statement, column))};
    }

private:
    void requireType(int column, int type) const
    {
        if (sqlite3_column_type(m_statement, column) != type) {
            throw FileFailure(DocumentFileCode::CorruptFile, "working-file field type is invalid");
        }
    }
    sqlite3 *m_database;
    sqlite3_stmt *m_statement = nullptr;
};

sqlite3_int64 scalar(sqlite3 *database, const char *sql)
{
    Statement query(database, sql);
    if (!query.row()) {
        throw FileFailure(DocumentFileCode::CorruptFile, "required working-file field is missing");
    }
    const auto value = query.integer(0);
    if (query.row()) {
        throw FileFailure(DocumentFileCode::CorruptFile, "duplicate working-file field");
    }
    return value;
}

class Transaction final {
public:
    Transaction(sqlite3 *database, bool writing) : m_database(database)
    {
        execute(database, writing ? "BEGIN IMMEDIATE" : "BEGIN");
    }
    ~Transaction()
    {
        if (!m_committed) {
            sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    void commit()
    {
        execute(m_database, "COMMIT");
        m_committed = true;
    }
private:
    sqlite3 *m_database;
    bool m_committed = false;
};

QByteArray digest(std::span<const std::uint8_t> bytes)
{
    return QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256);
}

void configure(sqlite3 *database)
{
    check(sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr), database);
    execute(database, "PRAGMA trusted_schema=OFF");
    Statement journal(database, "PRAGMA journal_mode=DELETE");
    if (!journal.row() || journal.text(0) != "delete") {
        throw FileFailure(DocumentFileCode::IoError, "DELETE journaling is required for direct file writes");
    }
    journal.done();
    execute(database, "PRAGMA synchronous=EXTRA");
    execute(database, "PRAGMA fullfsync=ON");
    if (scalar(database, "PRAGMA synchronous") != 3) {
        throw FileFailure(DocumentFileCode::IoError, "durable synchronization is unavailable");
    }
}

void validateSchema(sqlite3 *database)
{
    if (scalar(database, "PRAGMA application_id") != ApplicationId
        || scalar(database, "PRAGMA user_version") != StorageVersion) {
        throw FileFailure(DocumentFileCode::UnsupportedFormat, "not a supported iiSharedCanvas working file");
    }
    Statement schema(database,
        "SELECT type,name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY name");
    if (!schema.row() || schema.text(0) != "table" || schema.text(1) != "canvas_records"
        || schema.text(2) != RecordSchema || !schema.row() || schema.text(0) != "table"
        || schema.text(1) != "canvas_state" || schema.text(2) != StateSchema || schema.row()) {
        throw FileFailure(DocumentFileCode::CorruptFile, "working-file schema is not canonical");
    }
    Statement integrity(database, "PRAGMA quick_check");
    if (!integrity.row() || integrity.text(0) != "ok" || integrity.row()) {
        throw FileFailure(DocumentFileCode::CorruptFile, "working-file database integrity check failed");
    }
}

std::string absolutePath(const std::string &path)
{
    if (path.empty() || path.find('\0') != std::string::npos || path == ":memory:") {
        throw FileFailure(DocumentFileCode::InvalidPath, "a real file path is required");
    }
    const QString name = QString::fromUtf8(path.data(), static_cast<qsizetype>(path.size()));
    if (name.toUtf8().toStdString() != path) {
        throw FileFailure(DocumentFileCode::InvalidPath, "file path must be valid UTF-8");
    }
    return QFileInfo(name).absoluteFilePath().toUtf8().toStdString();
}

std::vector<DocumentRecord> readRecords(sqlite3 *database, SerializationLimits limits)
{
    std::vector<DocumentRecord> records;
    Statement query(database, "SELECT kind,id,position,data,digest FROM canvas_records ORDER BY kind,position");
    std::uint64_t total = IiscHeaderSize - 4;
    const auto maximumRecords = static_cast<std::uint64_t>(limits.maximumAssets) + limits.maximumLayers + 3;
    while (query.row()) {
        const auto kind = query.integer(0);
        const auto position = query.integer(2);
        const auto data = query.bytes(3);
        if (kind < 0 || kind > 4 || position < 0
            || position > std::numeric_limits<std::uint32_t>::max()) {
            throw FileFailure(DocumentFileCode::CorruptFile, "invalid record kind or position");
        }
        if (total > limits.maximumContainerBytes
            || data.size() > limits.maximumContainerBytes - total
            || records.size() >= maximumRecords) {
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file data exceeds configured limits");
        }
        total += data.size();
        const std::string id = query.text(1);
        if (id.size() > limits.maximumStringBytes) {
            throw FileFailure(DocumentFileCode::LimitExceeded, "record identifier exceeds the configured limit");
        }
        const auto storedDigest = query.bytes(4);
        const QByteArray computed = digest(data);
        if (storedDigest.size() != static_cast<std::size_t>(computed.size())
            || !std::equal(storedDigest.begin(), storedDigest.end(),
                           reinterpret_cast<const std::uint8_t *>(computed.data()))) {
            throw FileFailure(DocumentFileCode::CorruptFile, "working-file record checksum does not match");
        }
        records.push_back({static_cast<detail::RecordKind>(kind), id,
                           static_cast<std::uint32_t>(position),
                           std::vector<std::uint8_t>(data.begin(), data.end())});
    }
    return records;
}

struct StoredRecord {
    sqlite3_int64 rowId;
    sqlite3_int64 position;
    std::size_t size;
    QByteArray digest;
};

std::uint64_t patchBlob(sqlite3 *database, sqlite3_int64 rowId,
                        const std::vector<std::uint8_t> &replacement)
{
    sqlite3_blob *raw = nullptr;
    check(sqlite3_blob_open(database, "main", "canvas_records", "data", rowId, 1, &raw), database);
    const auto closeBlob = [](sqlite3_blob *blob) { sqlite3_blob_close(blob); };
    std::unique_ptr<sqlite3_blob, decltype(closeBlob)> blob(raw, closeBlob);
    std::array<std::uint8_t, 4096> previous{};
    std::uint64_t written = 0;
    for (std::size_t offset = 0; offset < replacement.size(); offset += previous.size()) {
        const std::size_t count = std::min(previous.size(), replacement.size() - offset);
        check(sqlite3_blob_read(blob.get(), previous.data(), static_cast<int>(count),
                               static_cast<int>(offset)), database);
        std::size_t index = 0;
        while (index < count) {
            if (previous[index] == replacement[offset + index]) {
                ++index;
                continue;
            }
            const std::size_t first = index;
            std::size_t last = index + 1;
            // Merge nearby differences; large unchanged spans are never rewritten.
            while (++index < count && index - last < 32) {
                if (previous[index] != replacement[offset + index]) {
                    last = index + 1;
                }
            }
            check(sqlite3_blob_write(blob.get(), replacement.data() + offset + first,
                                    static_cast<int>(last - first), static_cast<int>(offset + first)), database);
            written += last - first;
        }
    }
    check(sqlite3_blob_close(blob.release()), database);
    return written;
}

DocumentFileWriteStatistics writeRecords(sqlite3 *database,
                                         const std::vector<DocumentRecord> &records,
                                         SerializationLimits limits)
{
    std::map<RecordKey, StoredRecord> previous;
    Statement query(database, "SELECT rowid,kind,id,position,length(data),digest FROM canvas_records");
    while (query.row()) {
        const auto bytes = query.bytes(5);
        previous.emplace(RecordKey{static_cast<int>(query.integer(1)), query.text(2)},
            StoredRecord{query.integer(0), query.integer(3), static_cast<std::size_t>(query.integer(4)),
                         QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<qsizetype>(bytes.size()))});
    }
    std::uint64_t total = IiscHeaderSize - 4;
    for (const DocumentRecord &record : records) {
        const auto found = previous.find({static_cast<int>(record.kind), record.id});
        if (!record.data && found == previous.end()) {
            throw FileFailure(DocumentFileCode::CorruptFile, "unchanged asset record is missing");
        }
        const auto size = record.data ? record.data->size() : found->second.size;
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())
            || total > limits.maximumContainerBytes || size > limits.maximumContainerBytes - total) {
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file payload exceeds the configured limit");
        }
        total += size;
    }

    DocumentFileWriteStatistics statistics;
    for (const DocumentRecord &record : records) {
        const auto key = RecordKey{static_cast<int>(record.kind), record.id};
        const auto found = previous.find(key);
        if (found == previous.end()) {
            Statement insert(database, "INSERT INTO canvas_records(kind,id,position,data,digest) VALUES(?,?,?,?,?)");
            insert.integer(1, key.first);
            insert.text(2, key.second);
            insert.integer(3, record.position);
            insert.bytes(4, record.data->data(), record.data->size());
            const auto hash = digest(*record.data);
            insert.bytes(5, hash.data(), static_cast<std::size_t>(hash.size()));
            insert.done();
            ++statistics.recordsWritten;
            statistics.payloadBytesWritten += record.data->size();
            continue;
        }
        const StoredRecord prior = found->second;
        previous.erase(found);
        const auto hash = record.data ? digest(*record.data) : prior.digest;
        const bool changed = hash != prior.digest;
        if (!changed && record.position == prior.position) {
            continue;
        }
        if (changed && record.data->size() == prior.size) {
            statistics.payloadBytesWritten += patchBlob(database, prior.rowId, *record.data);
        }
        const bool replace = changed && record.data->size() != prior.size;
        Statement update(database, replace
            ? "UPDATE canvas_records SET position=?,digest=?,data=? WHERE rowid=?"
            : "UPDATE canvas_records SET position=?,digest=? WHERE rowid=?");
        update.integer(1, record.position);
        update.bytes(2, hash.data(), static_cast<std::size_t>(hash.size()));
        if (replace) {
            update.bytes(3, record.data->data(), record.data->size());
            statistics.payloadBytesWritten += record.data->size();
        }
        update.integer(replace ? 4 : 3, prior.rowId);
        update.done();
        ++statistics.recordsWritten;
    }
    for (const auto &[key, record] : previous) {
        Statement remove(database, "DELETE FROM canvas_records WHERE rowid=?");
        remove.integer(1, record.rowId);
        remove.done();
        ++statistics.recordsWritten;
    }
    return statistics;
}

void requireEncoding(const IiscError &error)
{
    if (error.code != IiscErrorCode::None) {
        throw FileFailure(error.code == IiscErrorCode::LimitExceeded
                              ? DocumentFileCode::LimitExceeded : DocumentFileCode::InvalidDocument,
                          error.message);
    }
}

} // namespace

class DocumentFile::Impl final {
public:
    std::unique_ptr<Database> database;
    Document document;
    std::string path;
    SerializationLimits limits;
    DocumentFileResult result;
    DocumentFileWriteStatistics statistics;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    sqlite3_int64 dataVersion = 0;
    bool editing = false;

    DocumentFileResult fail(DocumentFileCode code, std::string message)
    {
        statistics = {};
        result = {code, false, std::move(message)};
        return result;
    }
};

DocumentFile::DocumentFile() : m_impl(std::make_unique<Impl>()) {}
DocumentFile::~DocumentFile() = default;

DocumentFileResult DocumentFile::create(const std::string &path, const Document &document,
                                        SerializationLimits limits)
{
    if (isOpen()) {
        return m_impl->fail(DocumentFileCode::AlreadyOpen, "a working file is already open");
    }
    std::string createdPath;
    try {
        auto encoded = detail::encodeDocumentRecords(document, nullptr, limits);
        requireEncoding(encoded.error);
        // Validate the complete persisted representation before touching a path.
        auto decoded = detail::decodeDocumentRecords(encoded.records, limits);
        requireEncoding(decoded.error);
        const auto target = absolutePath(path);
        QFile destination(QString::fromStdString(target));
        if (!destination.open(QIODevice::ReadWrite | QIODevice::NewOnly)) {
            return m_impl->fail(QFileInfo::exists(destination.fileName())
                                    ? DocumentFileCode::AlreadyExists : DocumentFileCode::IoError,
                                destination.errorString().toStdString());
        }
        destination.close();
        createdPath = target;
        auto database = std::make_unique<Database>(target);
        configure(database->handle);
        Transaction transaction(database->handle, true);
        execute(database->handle, "PRAGMA application_id=1229542211");
        execute(database->handle, "PRAGMA user_version=1");
        execute(database->handle, StateSchema);
        execute(database->handle, RecordSchema);
        execute(database->handle, "INSERT INTO canvas_state VALUES(1,0)");
        const auto statistics = writeRecords(database->handle, encoded.records, limits);
        transaction.commit();
        m_impl->dataVersion = scalar(database->handle, "PRAGMA data_version");
        m_impl->database = std::move(database);
        m_impl->document = std::move(decoded.document);
        m_impl->path = target;
        m_impl->limits = limits;
        m_impl->revision = 0;
        ++m_impl->generation;
        m_impl->statistics = statistics;
        m_impl->result = {DocumentFileCode::None, true, {}};
        return m_impl->result;
    } catch (const FileFailure &error) {
        if (!createdPath.empty()) {
            QFile::remove(QString::fromStdString(createdPath));
        }
        return m_impl->fail(error.code, error.what());
    } catch (const std::exception &error) {
        if (!createdPath.empty()) {
            QFile::remove(QString::fromStdString(createdPath));
        }
        return m_impl->fail(DocumentFileCode::IoError, error.what());
    }
}

DocumentFileResult DocumentFile::open(const std::string &path, SerializationLimits limits)
{
    if (isOpen()) {
        return m_impl->fail(DocumentFileCode::AlreadyOpen, "a working file is already open");
    }
    try {
        const auto target = absolutePath(path);
        QFile input(QString::fromStdString(target));
        if (!input.open(QIODevice::ReadOnly)) {
            return m_impl->fail(DocumentFileCode::IoError, input.errorString().toStdString());
        }
        if (input.read(16) != QByteArray("SQLite format 3\0", 16)) {
            return m_impl->fail(DocumentFileCode::UnsupportedFormat,
                "not a working file; import a legacy .iisc snapshot with decodeIisc and create a new working file");
        }
        input.close();
        auto database = std::make_unique<Database>(target);
        // Do not change unrelated/unknown databases, even their journal mode.
        validateSchema(database->handle);
        configure(database->handle);
        Transaction transaction(database->handle, false);
        const auto revision = scalar(database->handle, "SELECT revision FROM canvas_state WHERE singleton=1");
        if (revision < 0) {
            throw FileFailure(DocumentFileCode::CorruptFile, "negative working-file revision");
        }
        auto decoded = detail::decodeDocumentRecords(readRecords(database->handle, limits), limits);
        if (!decoded.ok()) {
            throw FileFailure(decoded.error.code == IiscErrorCode::LimitExceeded
                                  ? DocumentFileCode::LimitExceeded
                                  : (decoded.error.code == IiscErrorCode::UnsupportedVersion
                                         ? DocumentFileCode::UnsupportedFormat : DocumentFileCode::CorruptFile),
                              decoded.error.message);
        }
        const auto dataVersion = scalar(database->handle, "PRAGMA data_version");
        transaction.commit();
        m_impl->database = std::move(database);
        m_impl->document = std::move(decoded.document);
        m_impl->path = target;
        m_impl->limits = limits;
        m_impl->revision = static_cast<std::uint64_t>(revision);
        m_impl->dataVersion = dataVersion;
        ++m_impl->generation;
        m_impl->statistics = {};
        m_impl->result = {};
        return m_impl->result;
    } catch (const FileFailure &error) {
        return m_impl->fail(error.code, error.what());
    } catch (const std::exception &error) {
        return m_impl->fail(DocumentFileCode::IoError, error.what());
    }
}

void DocumentFile::close() noexcept
{
    if (m_impl->editing) {
        return; // A callback cannot invalidate its own transaction owner.
    }
    m_impl->database.reset();
    m_impl->document = {};
    m_impl->path.clear();
    m_impl->revision = 0;
    ++m_impl->generation;
    m_impl->result = {};
    m_impl->statistics = {};
}

bool DocumentFile::isOpen() const noexcept { return m_impl->database != nullptr; }
const Document *DocumentFile::document() const noexcept { return isOpen() ? &m_impl->document : nullptr; }
Document *DocumentFile::boundDocument() noexcept { return isOpen() ? &m_impl->document : nullptr; }
const std::string &DocumentFile::filePath() const noexcept { return m_impl->path; }
std::uint64_t DocumentFile::revision() const noexcept { return m_impl->revision; }
std::uint64_t DocumentFile::bindingGeneration() const noexcept { return m_impl->generation; }
const DocumentFileResult &DocumentFile::lastResult() const noexcept { return m_impl->result; }
DocumentFileWriteStatistics DocumentFile::lastWriteStatistics() const noexcept { return m_impl->statistics; }

DocumentFileResult DocumentFile::edit(const std::function<bool(Document &)> &edit)
{
    m_impl->statistics = {};
    if (!isOpen()) {
        return m_impl->fail(DocumentFileCode::NotOpen, "no working file is open");
    }
    if (!edit || m_impl->editing) {
        return m_impl->fail(DocumentFileCode::EditRejected, "empty or nested file edits are not allowed");
    }
    struct EditingGuard {
        bool &editing;
        explicit EditingGuard(bool &value) : editing(value) { editing = true; }
        ~EditingGuard() { editing = false; }
    } guard(m_impl->editing);
    try {
        Document draft = m_impl->document;
        if (!edit(draft)) {
            return m_impl->fail(DocumentFileCode::EditRejected, "the edit callback rejected its draft");
        }
        auto encoded = detail::encodeDocumentRecords(draft, &m_impl->document, m_impl->limits);
        requireEncoding(encoded.error);
        auto *database = m_impl->database->handle;
        Transaction transaction(database, true);
        int moved = 0;
        const int movedResult = sqlite3_file_control(database, "main", SQLITE_FCNTL_HAS_MOVED, &moved);
        if ((movedResult == SQLITE_OK && moved)
            || scalar(database, "PRAGMA data_version") != m_impl->dataVersion
            || scalar(database, "SELECT revision FROM canvas_state WHERE singleton=1")
                   != static_cast<sqlite3_int64>(m_impl->revision)) {
            throw FileFailure(DocumentFileCode::Conflict,
                              "the working file changed outside this session; reopen before editing");
        }
        const auto statistics = writeRecords(database, encoded.records, m_impl->limits);
        if (statistics.recordsWritten == 0) {
            transaction.commit();
            m_impl->result = {};
            return m_impl->result;
        }
        if (m_impl->revision == static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file revision is exhausted");
        }
        execute(database, "UPDATE canvas_state SET revision=revision+1 WHERE singleton=1");
        transaction.commit();
        m_impl->document = std::move(draft);
        ++m_impl->revision;
        m_impl->statistics = statistics;
        m_impl->result = {DocumentFileCode::None, true, {}};
        return m_impl->result;
    } catch (const FileFailure &error) {
        // A failed COMMIT normally rolls back. If its outcome cannot be verified,
        // detach the file rather than expose memory as a confirmed disk state.
        try {
            auto *database = m_impl->database->handle;
            if (!sqlite3_get_autocommit(database)
                || scalar(database, "SELECT revision FROM canvas_state WHERE singleton=1")
                       != static_cast<sqlite3_int64>(m_impl->revision)) {
                m_impl->database.reset();
                ++m_impl->generation;
            }
        } catch (...) {
            m_impl->database.reset();
            ++m_impl->generation;
        }
        return m_impl->fail(error.code, error.what());
    } catch (const std::exception &error) {
        return m_impl->fail(DocumentFileCode::EditRejected, error.what());
    } catch (...) {
        return m_impl->fail(DocumentFileCode::EditRejected, "the edit callback threw an exception");
    }
}

} // namespace iiSharedCanvas
