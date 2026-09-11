#include "File/DocumentFile.h"

#include "Serialization/DocumentRecords_p.hpp"

#include <QCryptographicHash>
#include <iiFileProvider.h>

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

using iiFileProvider::Database;
using iiFileProvider::Statement;
using iiFileProvider::Transaction;

DocumentFileCode storageCode(iiFileProvider::FileCode code)
{
    using iiFileProvider::FileCode;
    switch (code) {
    case FileCode::InvalidPath: return DocumentFileCode::InvalidPath;
    case FileCode::AlreadyExists: return DocumentFileCode::AlreadyExists;
    case FileCode::Conflict: return DocumentFileCode::Conflict;
    case FileCode::LimitExceeded: return DocumentFileCode::LimitExceeded;
    case FileCode::CorruptFile: return DocumentFileCode::CorruptFile;
    default: return DocumentFileCode::IoError;
    }
}

void removeCreated(const std::string &path) noexcept
{
    if (path.empty()) return;
    try { iiFileProvider::File::remove(QString::fromStdString(path)); } catch (...) {}
}

QByteArray digest(std::span<const std::uint8_t> bytes)
{
    return QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char *>(bytes.data()),
                       static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256);
}

void validateSchema(Database *database)
{
    if (database->scalar("PRAGMA application_id") != ApplicationId
        || database->scalar("PRAGMA user_version") != StorageVersion) {
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
    return iiFileProvider::File::absolutePath(name).toUtf8().toStdString();
}

std::vector<DocumentRecord> readRecords(Database *database, SerializationLimits limits)
{
    std::vector<DocumentRecord> records;
    Statement query(database, "SELECT kind,id,position,data,digest FROM canvas_records ORDER BY kind,position");
    std::uint64_t total = IiscHeaderSize - 4;
    const auto maximumRecords = static_cast<std::uint64_t>(limits.maximumAssets)
        + limits.maximumLayers + limits.maximumAudioAssets + limits.maximumAudioTracks + 6;
    while (query.row()) {
        const auto kind = query.integer(0);
        const auto position = query.integer(2);
        const auto data = query.bytes(3);
        if (kind < 0 || kind > static_cast<int>(detail::RecordKind::Authorship) || position < 0
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
    std::int64_t rowId;
    std::int64_t position;
    std::size_t size;
    QByteArray digest;
};

DocumentFileWriteStatistics writeRecords(Database *database,
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
            statistics.payloadBytesWritten += database->patchBlob("canvas_records", "data", prior.rowId, *record.data);
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
    std::int64_t dataVersion = 0;
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
        iiFileProvider::File::create(QString::fromStdString(target), {});
        createdPath = target;
        auto database = std::make_unique<Database>(target);
        database->configureDurable();
        Transaction transaction(database.get(), true);
        database->execute("PRAGMA application_id=1229542211");
        database->execute("PRAGMA user_version=1");
        database->execute(StateSchema);
        database->execute(RecordSchema);
        database->execute("INSERT INTO canvas_state VALUES(1,0)");
        const auto statistics = writeRecords(database.get(), encoded.records, limits);
        transaction.commit();
        m_impl->dataVersion = database->scalar("PRAGMA data_version");
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
            removeCreated(createdPath);
        }
        return m_impl->fail(error.code, error.what());
    } catch (const iiFileProvider::FileError &error) {
        removeCreated(createdPath);
        return m_impl->fail(storageCode(error.code()), error.what());
    } catch (const std::exception &error) {
        if (!createdPath.empty()) {
            removeCreated(createdPath);
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
        if (iiFileProvider::File::readPrefix(QString::fromStdString(target), 16)
            != QByteArray("SQLite format 3\0", 16)) {
            return m_impl->fail(DocumentFileCode::UnsupportedFormat,
                "not a working file; import a legacy .iisc snapshot with decodeIisc and create a new working file");
        }
        auto database = std::make_unique<Database>(target);
        // Do not change unrelated/unknown databases, even their journal mode.
        validateSchema(database.get());
        database->configureDurable();
        Transaction transaction(database.get(), false);
        const auto revision = database->scalar("SELECT revision FROM canvas_state WHERE singleton=1");
        if (revision < 0) {
            throw FileFailure(DocumentFileCode::CorruptFile, "negative working-file revision");
        }
        auto decoded = detail::decodeDocumentRecords(readRecords(database.get(), limits), limits);
        if (!decoded.ok()) {
            throw FileFailure(decoded.error.code == IiscErrorCode::LimitExceeded
                                  ? DocumentFileCode::LimitExceeded
                                  : (decoded.error.code == IiscErrorCode::UnsupportedVersion
                                         ? DocumentFileCode::UnsupportedFormat : DocumentFileCode::CorruptFile),
                              decoded.error.message);
        }
        const auto dataVersion = database->scalar("PRAGMA data_version");
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
    } catch (const iiFileProvider::FileError &error) {
        return m_impl->fail(storageCode(error.code()), error.what());
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
        auto *database = m_impl->database.get();
        Transaction transaction(database, true);
        if (database->hasMoved()
            || database->scalar("PRAGMA data_version") != m_impl->dataVersion
            || database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                   != static_cast<std::int64_t>(m_impl->revision)) {
            throw FileFailure(DocumentFileCode::Conflict,
                              "the working file changed outside this session; reopen before editing");
        }
        auto statistics = writeRecords(database, encoded.records, m_impl->limits);
        if (statistics.recordsWritten == 0) {
            transaction.commit();
            // Selecting an already recorded author changes only local edit context.
            m_impl->document.authorship = std::move(draft.authorship);
            m_impl->result = {};
            return m_impl->result;
        }
        if (draft.authorship.dump() == m_impl->document.authorship.dump()) {
            recordDocumentChange(draft);
            auto stamped = detail::encodeDocumentRecords(draft, &m_impl->document, m_impl->limits);
            requireEncoding(stamped.error);
            const auto metadataWrites = writeRecords(database, stamped.records, m_impl->limits);
            statistics.recordsWritten += metadataWrites.recordsWritten;
            statistics.payloadBytesWritten += metadataWrites.payloadBytesWritten;
        }
        if (m_impl->revision == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file revision is exhausted");
        }
        database->execute("UPDATE canvas_state SET revision=revision+1 WHERE singleton=1");
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
            auto *database = m_impl->database.get();
            if (database->inTransaction()
                || database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                       != static_cast<std::int64_t>(m_impl->revision)) {
                m_impl->database.reset();
                ++m_impl->generation;
            }
        } catch (...) {
            m_impl->database.reset();
            ++m_impl->generation;
        }
        return m_impl->fail(error.code, error.what());
    } catch (const iiFileProvider::FileError &error) {
        // A failed COMMIT normally rolls back. If its outcome cannot be verified,
        // detach the file rather than expose memory as a confirmed disk state.
        try {
            auto *database = m_impl->database.get();
            if (database->inTransaction()
                || database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                       != static_cast<std::int64_t>(m_impl->revision)) {
                m_impl->database.reset();
                ++m_impl->generation;
            }
        } catch (...) {
            m_impl->database.reset();
            ++m_impl->generation;
        }
        return m_impl->fail(storageCode(error.code()), error.what());
    } catch (const std::exception &error) {
        return m_impl->fail(DocumentFileCode::EditRejected, error.what());
    } catch (...) {
        return m_impl->fail(DocumentFileCode::EditRejected, "the edit callback threw an exception");
    }
}

} // namespace iiSharedCanvas
