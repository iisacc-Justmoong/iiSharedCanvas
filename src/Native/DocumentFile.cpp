#include "File/DocumentFile.h"

#include "Serialization/DocumentRecords_p.hpp"
#include "Document/DocumentCommit_p.hpp"
#include "Document/AssetLayerGuard_p.hpp"
#include "Document/TimelineGuard_p.hpp"
#include "File/IiscInput_p.hpp"

#include "Native/Storage_p.hpp"
#include "Native/WorkingFileTiles_p.hpp"
#include "Bitmap/BitmapCommit_p.hpp"
#include <iiFileProviderNative.h>
#include <iiFileProviderPresentation.h>
#include <iiSDKCore/Core/Text/Utf8.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace iiSharedCanvas {
namespace {

constexpr int ApplicationId = 0x49495343; // IISC, distinct from other SQLite files.
constexpr int StorageVersion = 2;
static_assert(noexcept(std::declval<detail::PreparedDocumentCommit&>().commit()));
constexpr const char *StateSchema =
    "CREATE TABLE canvas_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), "
    "revision INTEGER NOT NULL CHECK(revision>=0))";
constexpr const char *RecordSchema =
    "CREATE TABLE canvas_records (kind INTEGER NOT NULL, id TEXT NOT NULL, "
    "position INTEGER NOT NULL CHECK(position>=0), data BLOB NOT NULL, "
    "digest BLOB NOT NULL, UNIQUE(kind,id))";

using RecordKey = std::pair<int, std::string>;
using detail::DocumentRecord;

using FileFailure = detail::NativeStorageFailure;

using Database=detail::NativeDatabase;
using Statement=detail::NativeStatement;
using Transaction=detail::NativeTransaction;
using iiFileProvider::native::SourceAccess;

DocumentFileCode storageCode(iiFileProvider::native::FileCode code)
{
    using iiFileProvider::native::FileCode;
    switch (code) {
    case FileCode::InvalidPath: return DocumentFileCode::InvalidPath;
    case FileCode::AlreadyExists: return DocumentFileCode::AlreadyExists;
    case FileCode::Conflict: return DocumentFileCode::Conflict;
    case FileCode::LimitExceeded: return DocumentFileCode::LimitExceeded;
    default: return DocumentFileCode::IoError;
    }
}

void removeCreated(const std::string &path) noexcept
{
    if (path.empty()) return;
    try { iiFileProvider::native::File::remove(std::filesystem::u8path(path)); } catch (...) {}
}

std::vector<std::uint8_t> digest(std::span<const std::uint8_t> bytes)
{
    const auto hex=iiFileProvider::native::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
    auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
    std::vector<std::uint8_t> result(32);
    for(std::size_t i=0;i<32;++i)result[i]=std::uint8_t((nibble(hex[i*2])<<4)|nibble(hex[i*2+1]));
    return result;
}
std::string readPrefix(const std::filesystem::path& path,std::size_t count)
{
    auto file=iiFileProvider::native::File::openRead(path);return file->read(count);
}

int validateSchema(Database *database)
{
    const auto version = database->scalar("PRAGMA user_version");
    if (database->scalar("PRAGMA application_id") != ApplicationId
        || (version != 1 && version != StorageVersion)) {
        throw FileFailure(DocumentFileCode::UnsupportedFormat, "not a supported iiSharedCanvas working file");
    }
    Statement schema(database,
        "SELECT type,name,sql FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY name");
    if (!schema.row() || schema.text(0) != "table" || schema.text(1) != "canvas_records"
        || schema.text(2) != RecordSchema || !schema.row() || schema.text(0) != "table"
        || schema.text(1) != "canvas_state" || schema.text(2) != StateSchema) {
        throw FileFailure(DocumentFileCode::CorruptFile, "working-file schema is not canonical");
    }
    if ((version == 2 && (!schema.row() || schema.text(0) != "table"
            || schema.text(1) != "canvas_tile_state" || schema.text(2) != detail::WorkingTileStateSchema
            || !schema.row() || schema.text(0) != "table"
            || schema.text(1) != "canvas_tiles" || schema.text(2) != detail::WorkingTileSchema))
        || schema.row()) {
        throw FileFailure(DocumentFileCode::CorruptFile, "working-file tile schema is not canonical");
    }
    Statement integrity(database, "PRAGMA quick_check");
    if (!integrity.row() || integrity.text(0) != "ok" || integrity.row()) {
        throw FileFailure(DocumentFileCode::CorruptFile, "working-file database integrity check failed");
    }
    return static_cast<int>(version);
}

std::string absolutePath(const std::string &path)
{
    if (path.empty() || path.find('\0') != std::string::npos || path == ":memory:" || !ii::core::valid_utf8(path)) {
        throw FileFailure(DocumentFileCode::InvalidPath, "a real file path is required");
    }
    const auto value=std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal().u8string();
    return std::string(value.begin(),value.end());
}

std::vector<DocumentRecord> readRecords(Database *database, SerializationLimits limits)
{
    std::vector<DocumentRecord> records;
    Statement query(database, "SELECT kind,id,position,length(data),length(digest),length(CAST(id AS BLOB)),data,digest FROM canvas_records ORDER BY kind,position");
    std::uint64_t total = IiscHeaderSize - 4;
    const auto maximumRecords = static_cast<std::uint64_t>(limits.maximumAssets)
        + limits.maximumLayers + limits.maximumAudioAssets + limits.maximumAudioTracks + limits.maximumArtboards + 8;
    while (query.row()) {
        const auto kind = query.integer(0);
        const auto position = query.integer(2);
        const auto length = query.integer(3);
        if (length < 0 || query.integer(4) != 32 || query.integer(5) < 0)
            throw FileFailure(DocumentFileCode::CorruptFile, "invalid record field length");
        if (kind < 0 || kind > static_cast<int>(detail::RecordKind::PublicationMetadata) || position < 0
            || position > std::numeric_limits<std::uint32_t>::max()) {
            throw FileFailure(DocumentFileCode::CorruptFile, "invalid record kind or position");
        }
        if (total > limits.maximumContainerBytes
            || std::uint64_t(length) > limits.maximumContainerBytes - total
            || records.size() >= maximumRecords) {
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file data exceeds configured limits");
        }
        if (std::uint64_t(query.integer(5)) > limits.maximumStringBytes)
            throw FileFailure(DocumentFileCode::LimitExceeded, "record identifier exceeds the configured limit");
        const auto data = query.bytes(6);
        total += data.size();
        const std::string id = query.text(1);
        if (id.size() > limits.maximumStringBytes) {
            throw FileFailure(DocumentFileCode::LimitExceeded, "record identifier exceeds the configured limit");
        }
        const auto storedDigest = query.bytes(7);
        const std::vector<std::uint8_t> computed = digest(data);
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

IiscDecodeResult readWorkingDocument(Database *database, int storageVersion, SerializationLimits limits)
{
    auto records = readRecords(database, limits);
    if (storageVersion == 1)
        return detail::decodeDocumentRecords(records, limits);
    detail::WorkingTileReader tiles(*database, limits);
    auto decoded = detail::decodeDocumentRecords(records, limits,
        [&](std::string_view id, std::int32_t width, std::int32_t height,
            std::optional<CanvasOrigin> chunk) { return tiles.read(id, width, height, chunk); });
    if (decoded.ok()) {
        std::uint64_t bytes = IiscHeaderSize - 4;
        for (const auto &record : records) bytes += record.data->size();
        tiles.finish(bytes);
    }
    return decoded;
}

void addStatistics(DocumentFileWriteStatistics &target, DocumentFileWriteStatistics added)
{
    target.recordsWritten += added.recordsWritten;
    target.payloadBytesWritten += added.payloadBytesWritten;
}

struct StoredRecord {
    std::int64_t rowId;
    std::int64_t position;
    std::size_t size;
    std::vector<std::uint8_t> digest;
};

struct RecordPayloadDelta {
    std::uint64_t removedBytes = 0;
    std::uint64_t addedBytes = 0;
};

DocumentFileWriteStatistics writeRecords(Database *database,
                                         const std::vector<DocumentRecord> &records,
                                         SerializationLimits limits,
                                         RecordPayloadDelta *payloadDelta = nullptr)
{
    std::map<RecordKey, StoredRecord> previous;
    std::uint64_t priorBytes = 0;
    Statement query(database, "SELECT rowid,kind,id,position,length(data),digest FROM canvas_records");
    while (query.row()) {
        const auto length = query.integer(4);
        if (payloadDelta) {
            if (length < 0 || std::uint64_t(length) > std::numeric_limits<std::uint64_t>::max() - priorBytes)
                throw FileFailure(DocumentFileCode::CorruptFile, "invalid stored metadata payload size");
            priorBytes += std::uint64_t(length);
        }
        const auto bytes = query.bytes(5);
        previous.emplace(RecordKey{static_cast<int>(query.integer(1)), query.text(2)},
            StoredRecord{query.integer(0), query.integer(3), static_cast<std::size_t>(length),
                         std::vector<std::uint8_t>(bytes.begin(),bytes.end())});
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
    if (payloadDelta) {
        payloadDelta->removedBytes = priorBytes;
        payloadDelta->addedBytes = total - (IiscHeaderSize - 4);
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

std::uint64_t storedPayloadBytes(Database *database, int version, SerializationLimits limits)
{
    const auto records = database->scalar("SELECT coalesce(sum(length(data)),0) FROM canvas_records");
    const auto tiles = version == 2
        ? database->scalar("SELECT coalesce(sum(length(data)),0) FROM canvas_tiles") : 0;
    if (records < 0 || tiles < 0)
        throw FileFailure(DocumentFileCode::CorruptFile, "negative working-file payload size");
    std::uint64_t total = IiscHeaderSize - 4;
    for (const auto bytes : {std::uint64_t(records), std::uint64_t(tiles)}) {
        if (total > limits.maximumContainerBytes || bytes > limits.maximumContainerBytes - total)
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file payload exceeds limit");
        total += bytes;
    }
    return total;
}

void changePayload(std::uint64_t &total, std::uint64_t removed, std::uint64_t added,
                   const SerializationLimits &limits)
{
    if (removed > total) throw FileFailure(DocumentFileCode::CorruptFile, "invalid payload accounting");
    total -= removed;
    if (added > limits.maximumContainerBytes || total > limits.maximumContainerBytes - added)
        throw FileFailure(DocumentFileCode::LimitExceeded, "working-file payload exceeds limit");
    total += added;
}

void replacePreparedRecord(Database *database, detail::RecordKind kind, const std::string &id,
                           std::span<const std::uint8_t> before,
                           std::span<const std::uint8_t> after,
                           std::uint64_t &removedBytes, std::uint64_t &addedBytes,
                           DocumentFileWriteStatistics &statistics)
{
    std::int64_t rowId = 0;
    {
        Statement query(database,
            "SELECT rowid,length(data),length(digest),data,digest FROM canvas_records WHERE kind=? AND id=?");
        query.integer(1, static_cast<int>(kind)); query.text(2, id);
        if (!query.row() || query.integer(1) != std::int64_t(before.size()) || query.integer(2) != 32)
            throw FileFailure(DocumentFileCode::CorruptFile, "prepared record is missing or has an invalid size");
        rowId = query.integer(0);
        const auto stored = query.bytes(3), expectedDigest = query.bytes(4);
        if (!std::ranges::equal(stored, before) || !std::ranges::equal(digest(stored), expectedDigest))
            throw FileFailure(DocumentFileCode::Conflict, "stored record differs from the live document");
        if (query.row()) throw FileFailure(DocumentFileCode::CorruptFile, "duplicate prepared record");
    }
    if (std::ranges::equal(before, after)) return;
    if (before.size() > std::numeric_limits<std::uint64_t>::max() - removedBytes ||
        after.size() > std::numeric_limits<std::uint64_t>::max() - addedBytes)
        throw FileFailure(DocumentFileCode::LimitExceeded, "working-file payload accounting overflow");
    removedBytes += before.size();
    addedBytes += after.size();
    const auto hash = digest(after);
    const bool resized = before.size() != after.size();
    if (!resized)
        statistics.payloadBytesWritten += database->patchBlob("canvas_records", "data", rowId, after);
    Statement update(database, resized
        ? "UPDATE canvas_records SET digest=?,data=? WHERE rowid=?"
        : "UPDATE canvas_records SET digest=? WHERE rowid=?");
    update.bytes(1, hash.data(), hash.size());
    if (resized) {
        update.bytes(2, after.data(), after.size());
        statistics.payloadBytesWritten += after.size();
    }
    update.integer(resized ? 3 : 2, rowId); update.done();
    ++statistics.recordsWritten;
}

std::int64_t verifyPreparedLayer(Database *database, const DocumentRecord &record)
{
    if (!record.data) throw FileFailure(DocumentFileCode::CorruptFile, "layer record has no payload");
    Statement query(database,
        "SELECT rowid,position,length(data),length(digest),data,digest FROM canvas_records WHERE kind=? AND id=?");
    query.integer(1, static_cast<int>(detail::RecordKind::Layer)); query.text(2, record.id);
    if (!query.row() || query.integer(1) != record.position ||
        query.integer(2) != std::int64_t(record.data->size()) || query.integer(3) != 32)
        throw FileFailure(DocumentFileCode::Conflict, "stored layer position or size differs from the live document");
    const auto rowId = query.integer(0);
    const auto stored = query.bytes(4), storedDigest = query.bytes(5);
    if (!std::ranges::equal(stored, *record.data) || !std::ranges::equal(digest(stored), storedDigest))
        throw FileFailure(DocumentFileCode::Conflict, "stored layer differs from the live document");
    if (query.row()) throw FileFailure(DocumentFileCode::CorruptFile, "duplicate layer record");
    return rowId;
}

} // namespace

class DocumentFile::Impl final {
public:
    std::unique_ptr<SourceAccess> access;
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
    int storageVersion = 0;
    detail::WorkingResourceTotals resources;
    std::uint64_t payloadBytes = 0;
    std::filesystem::path notificationPath;
    bool bitmapCommitted = false;
    bool bitmapNotification = false;

    DocumentFileResult fail(DocumentFileCode code, std::string message)
    {
        statistics = {};
        result = {code, false, std::move(message)};
        return result;
    }

    bool failBitmap(DocumentFileCode code, std::string_view message) noexcept
    {
        statistics = {};
        result.code = code;
        result.changed = false;
        try { result.message.assign(message); } catch (...) { result.message.clear(); }
        return false;
    }

    void verifyOutcome() noexcept
    {
        if (!database) return;
        try {
            if (database->inTransaction()
                || database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                       != static_cast<std::int64_t>(revision)) {
                database.reset();
                ++generation;
            }
        } catch (...) {
            database.reset();
            ++generation;
        }
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
        auto encoded = detail::encodeDocumentRecords(document, nullptr, limits, true);
        requireEncoding(encoded.error);
        // Validate the complete persisted representation before touching a path.
        auto decoded = detail::decodeDocumentRecords(encoded.records, limits,
            [&](std::string_view id, std::int32_t, std::int32_t, std::optional<CanvasOrigin> chunk) -> RasterLayer {
                for (const auto &asset : document.assets) {
                    if (assetId(asset) != id) continue;
                    if (const auto *raster = std::get_if<RasterAsset>(&asset)) return raster->pixels;
                    if (const auto *chunks = std::get_if<ChunkedRasterAsset>(&asset))
                        for (const auto &value : chunks->chunks)
                            if (chunk && value.column == chunk->x && value.row == chunk->y) return value.pixels;
                }
                throw FileFailure(DocumentFileCode::InvalidDocument, "missing source raster");
            });
        requireEncoding(decoded.error);
        const auto resources = detail::workingResourceTotals(decoded.document, limits);
        requireEncoding(resources.error);
        detail::PreparedDocumentCommit prepared(m_impl->document, decoded.document);
        const auto target = absolutePath(path);
        iiFileProvider::native::File::create(std::filesystem::u8path(target), {});
        createdPath = target;
        auto access = std::make_unique<SourceAccess>(std::filesystem::u8path(target));
        std::unique_lock sourceLock(*access);
        auto database = std::make_unique<Database>(target);
        database->configureDurable();
        Transaction transaction(database.get(), true);
        database->execute("PRAGMA application_id=1229542211");
        database->execute("PRAGMA user_version=2");
        database->execute(StateSchema);
        database->execute(RecordSchema);
        database->execute(detail::WorkingTileStateSchema);
        database->execute("INSERT INTO canvas_tile_state VALUES(1,0)");
        database->execute(detail::WorkingTileSchema);
        database->execute("INSERT INTO canvas_state VALUES(1,0)");
        auto statistics = writeRecords(database.get(), encoded.records, limits);
        addStatistics(statistics, detail::writeWorkingTiles(*database, decoded.document, nullptr, limits));
        const auto payloadBytes = storedPayloadBytes(database.get(), StorageVersion, limits);
        transaction.commit();
        m_impl->dataVersion = database->scalar("PRAGMA data_version");
        m_impl->access = std::move(access);
        m_impl->database = std::move(database);
        prepared.commit();
        m_impl->path = target;
        m_impl->limits = limits;
        m_impl->revision = 0;
        m_impl->storageVersion = StorageVersion;
        m_impl->resources = resources.totals;
        m_impl->payloadBytes = payloadBytes;
        ++m_impl->generation;
        m_impl->statistics = statistics;
        m_impl->result = {DocumentFileCode::None, true, {}};
        sourceLock.unlock();
        iiFileProvider::native::Presentation::enqueue(std::filesystem::u8path(target));
        return m_impl->result;
    } catch (const FileFailure &error) {
        if (!createdPath.empty()) {
            removeCreated(createdPath);
        }
        return m_impl->fail(error.code, error.what());
    } catch (const iiFileProvider::native::FileError &error) {
        removeCreated(createdPath);
        return m_impl->fail(storageCode(error.code()), error.what());
    } catch (const std::exception &error) {
        if (!createdPath.empty()) {
            removeCreated(createdPath);
        }
        return m_impl->fail(DocumentFileCode::IoError, error.what());
    }
}

DocumentFileResult DocumentFile::readSnapshot(const std::string &path, Document &snapshot,
                                             SerializationLimits limits)
{
    return readSnapshot(path, snapshot, limits, DefaultNativeSnapshotTimeoutMs);
}

DocumentFileResult DocumentFile::readSnapshot(const std::string &path, Document &snapshot,
                                             SerializationLimits limits, int backupTimeoutMs)
{
    if (backupTimeoutMs <= 0)
        return {DocumentFileCode::InvalidArgument, false, "snapshot backup timeout must be positive"};
    if (path.empty() || path.find('\0') != std::string::npos || path.find("://") != std::string::npos)
        return {DocumentFileCode::InvalidPath, false, "a UTF-8 local canvas file path is required"};
    try {
        const auto temporary = std::filesystem::temp_directory_path().u8string();
        auto document = detail::loadIiscDocumentReadOnly(path,
            std::string(temporary.begin(), temporary.end()), limits, backupTimeoutMs);
        detail::PreparedDocumentCommit(snapshot, document).commit();
        return {};
    } catch (const detail::IiscInputFailure &error) {
        return {error.code, false, error.what()};
    } catch (const std::bad_alloc &) {
        return {DocumentFileCode::LimitExceeded, false, "not enough memory to read a native canvas snapshot"};
    } catch (const std::exception &error) {
        return {DocumentFileCode::IoError, false, error.what()};
    }
}

DocumentFileResult DocumentFile::readRevision(const std::string &path, std::uint64_t &revision)
{
    DocumentFileInfo info;
    auto result = readStorageInfo(path, info);
    if (result.ok()) revision = info.revision;
    return result;
}

DocumentFileResult DocumentFile::readStorageInfo(const std::string &path, DocumentFileInfo &info)
{
    try {
        const auto target = absolutePath(path);
        SourceAccess access(std::filesystem::u8path(target));
        std::lock_guard sourceLock(access);
        if (readPrefix(std::filesystem::u8path(target), 16)
            != std::string("SQLite format 3\0",16)) {
            return {DocumentFileCode::UnsupportedFormat, false, "not a working file"};
        }
        Database database(target, true);
        Transaction transaction(&database, false);
        const auto storageVersion = validateSchema(&database);
        const auto stored = database.scalar("SELECT revision FROM canvas_state WHERE singleton=1");
        if (stored < 0)
            return {DocumentFileCode::CorruptFile, false, "negative working-file revision"};
        transaction.commit();
        info = {static_cast<std::uint64_t>(stored), static_cast<std::uint32_t>(storageVersion)};
        return {};
    } catch (const FileFailure &error) {
        return {error.code, false, error.what()};
    } catch (const iiFileProvider::native::FileError &error) {
        return {storageCode(error.code()), false, error.what()};
    } catch (const std::exception &error) {
        return {DocumentFileCode::IoError, false, error.what()};
    }
}

DocumentFileResult DocumentFile::open(const std::string &path, SerializationLimits limits)
{
    if (isOpen()) {
        return m_impl->fail(DocumentFileCode::AlreadyOpen, "a working file is already open");
    }
    try {
        const auto target = absolutePath(path);
        auto access = std::make_unique<SourceAccess>(std::filesystem::u8path(target));
        std::lock_guard sourceLock(*access);
        if (readPrefix(std::filesystem::u8path(target), 16)
            != std::string("SQLite format 3\0",16)) {
            return m_impl->fail(DocumentFileCode::UnsupportedFormat,
                "not a working file; import a legacy .iisc snapshot with decodeIisc and create a new working file");
        }
        auto database = std::make_unique<Database>(target);
        // Do not change unrelated/unknown databases, even their journal mode.
        const auto storageVersion = validateSchema(database.get());
        Transaction transaction(database.get(), false);
        const auto revision = database->scalar("SELECT revision FROM canvas_state WHERE singleton=1");
        if (revision < 0) {
            throw FileFailure(DocumentFileCode::CorruptFile, "negative working-file revision");
        }
        auto decoded = readWorkingDocument(database.get(), storageVersion, limits);
        if (!decoded.ok()) {
            throw FileFailure(decoded.error.code == IiscErrorCode::LimitExceeded
                                  ? DocumentFileCode::LimitExceeded
                                  : (decoded.error.code == IiscErrorCode::UnsupportedVersion
                                         ? DocumentFileCode::UnsupportedFormat : DocumentFileCode::CorruptFile),
                              decoded.error.message);
        }
        const auto resources = detail::workingResourceTotals(decoded.document, limits);
        requireEncoding(resources.error);
        const auto payloadBytes = storedPayloadBytes(database.get(), storageVersion, limits);
        detail::PreparedDocumentCommit prepared(m_impl->document, decoded.document);
        const auto dataVersion = database->scalar("PRAGMA data_version");
        transaction.commit();
        // Reject corrupt/future model records before changing the source's
        // journal mode. Only a fully decoded working file becomes an authoring owner.
        database->configureDurable();
        m_impl->access = std::move(access);
        m_impl->database = std::move(database);
        prepared.commit();
        m_impl->path = target;
        m_impl->limits = limits;
        m_impl->revision = static_cast<std::uint64_t>(revision);
        m_impl->storageVersion = storageVersion;
        m_impl->resources = resources.totals;
        m_impl->payloadBytes = payloadBytes;
        m_impl->dataVersion = dataVersion;
        ++m_impl->generation;
        m_impl->statistics = {};
        m_impl->result = {};
        return m_impl->result;
    } catch (const FileFailure &error) {
        return m_impl->fail(error.code, error.what());
    } catch (const iiFileProvider::native::FileError &error) {
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
    m_impl->access.reset();
    m_impl->document = {};
    m_impl->path.clear();
    m_impl->revision = 0;
    m_impl->storageVersion = 0;
    m_impl->resources = {};
    m_impl->payloadBytes = 0;
    ++m_impl->generation;
    m_impl->result = {};
    m_impl->statistics = {};
}

bool DocumentFile::isOpen() const noexcept { return m_impl->database != nullptr; }
const Document *DocumentFile::document() const noexcept { return isOpen() ? &m_impl->document : nullptr; }
Document *DocumentFile::boundDocument() noexcept { return isOpen() ? &m_impl->document : nullptr; }
const std::string &DocumentFile::filePath() const noexcept { return m_impl->path; }
std::uint64_t DocumentFile::revision() const noexcept { return m_impl->revision; }
std::uint32_t DocumentFile::storageVersion() const noexcept { return isOpen() ? m_impl->storageVersion : 0; }
std::uint64_t DocumentFile::bindingGeneration() const noexcept { return m_impl->generation; }
const DocumentFileResult &DocumentFile::lastResult() const noexcept { return m_impl->result; }
DocumentFileWriteStatistics DocumentFile::lastWriteStatistics() const noexcept { return m_impl->statistics; }

DocumentFileResult DocumentFile::edit(const std::function<bool(Document &)> &edit)
{
    return editImpl(edit, true);
}

DocumentFileResult DocumentFile::upgradeFormat()
{
    return editImpl([](Document &draft) {
        draft.formatVersion = {CurrentFormatMajor, CurrentFormatMinor};
        return true;
    }, false);
}

DocumentFileResult DocumentFile::upgradeStorage()
{
    m_impl->statistics = {};
    if (!isOpen()) return m_impl->fail(DocumentFileCode::NotOpen, "no working file is open");
    if (m_impl->editing) return m_impl->fail(DocumentFileCode::EditRejected, "nested storage upgrade");
    struct Guard {
        bool &value;
        Guard(bool &v) : value(v) { value = true; }
        ~Guard() { value = false; }
    } guard(m_impl->editing);
    std::unique_lock<SourceAccess> sourceLock;
    try {
        sourceLock = std::unique_lock(*m_impl->access);
        auto *database = m_impl->database.get();
        Transaction transaction(database, true);
        if (database->hasMoved()
            || database->scalar("PRAGMA data_version") != m_impl->dataVersion
            || database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                   != static_cast<std::int64_t>(m_impl->revision))
            throw FileFailure(DocumentFileCode::Conflict, "working file changed before storage upgrade");
        if (m_impl->storageVersion == StorageVersion) {
            transaction.commit();
            return m_impl->result = {};
        }
        if (m_impl->revision == std::uint64_t(INT64_MAX))
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file revision is exhausted");
        auto encoded = detail::encodeDocumentRecords(m_impl->document, nullptr, m_impl->limits, true);
        requireEncoding(encoded.error);
        database->execute(detail::WorkingTileStateSchema);
        database->execute("INSERT INTO canvas_tile_state VALUES(1,0)");
        database->execute(detail::WorkingTileSchema);
        database->execute("PRAGMA user_version=2");
        auto statistics = writeRecords(database, encoded.records, m_impl->limits);
        addStatistics(statistics, detail::writeWorkingTiles(*database, m_impl->document, nullptr, m_impl->limits));
        const auto payloadBytes = storedPayloadBytes(database, StorageVersion, m_impl->limits);
        database->execute("UPDATE canvas_state SET revision=revision+1 WHERE singleton=1");
        transaction.commit();
        m_impl->storageVersion = StorageVersion;
        m_impl->payloadBytes = payloadBytes;
        ++m_impl->revision;
        m_impl->statistics = statistics;
        m_impl->result = {DocumentFileCode::None, true, {}};
        sourceLock.unlock();
        iiFileProvider::native::Presentation::enqueue(std::filesystem::u8path(m_impl->path));
        return m_impl->result;
    } catch (const FileFailure &error) {
        m_impl->verifyOutcome();
        return m_impl->fail(error.code, error.what());
    } catch (const iiFileProvider::native::FileError &error) {
        m_impl->verifyOutcome();
        return m_impl->fail(storageCode(error.code()), error.what());
    } catch (const std::bad_alloc &) {
        m_impl->verifyOutcome();
        return m_impl->fail(DocumentFileCode::LimitExceeded, "out of memory");
    } catch (const std::exception &error) {
        m_impl->verifyOutcome();
        return m_impl->fail(DocumentFileCode::EditRejected, error.what());
    }
}

bool DocumentFile::beginBitmapEdit(std::uint64_t expectedGeneration)
{
    if (!isOpen() || expectedGeneration != m_impl->generation)
        return m_impl->failBitmap(DocumentFileCode::Conflict, "the working-file binding is no longer valid");
    if (m_impl->editing)
        return m_impl->failBitmap(DocumentFileCode::EditRejected, "nested working-file edits are not allowed");
    m_impl->statistics = {};
    m_impl->editing = true;
    m_impl->bitmapCommitted = false;
    m_impl->bitmapNotification = false;
    try {
        m_impl->notificationPath = std::filesystem::u8path(m_impl->path);
        std::unique_lock sourceLock(*m_impl->access);
        auto *database = m_impl->database.get();
        Transaction transaction(database, false);
        if (database->hasMoved()
            || database->scalar("PRAGMA data_version") != m_impl->dataVersion
            || database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                   != static_cast<std::int64_t>(m_impl->revision))
            throw FileFailure(DocumentFileCode::Conflict, "the working file changed outside this session");
        transaction.commit();
        m_impl->result = {};
        return true;
    } catch (const FileFailure &error) {
        m_impl->verifyOutcome(); m_impl->editing = false;
        return m_impl->failBitmap(error.code, error.what());
    } catch (const iiFileProvider::native::FileError &error) {
        m_impl->verifyOutcome(); m_impl->editing = false;
        return m_impl->failBitmap(storageCode(error.code()), error.what());
    } catch (const std::bad_alloc &) {
        m_impl->verifyOutcome(); m_impl->editing = false;
        return m_impl->failBitmap(DocumentFileCode::LimitExceeded, "out of memory");
    } catch (const std::exception &error) {
        m_impl->verifyOutcome(); m_impl->editing = false;
        return m_impl->failBitmap(DocumentFileCode::IoError, error.what());
    } catch (...) {
        m_impl->verifyOutcome(); m_impl->editing = false;
        return m_impl->failBitmap(DocumentFileCode::IoError, "unknown bitmap preflight failure");
    }
}

bool DocumentFile::commitBitmapEdit(const detail::PreparedBitmapChange &change, bool &useDraftFallback)
{
    if (!isOpen() || !m_impl->editing || m_impl->bitmapCommitted ||
        change.document != &m_impl->document || !change.authorship || !change.readPixel ||
        bool(change.raster) == bool(change.chunks))
        return m_impl->failBitmap(DocumentFileCode::EditRejected, "invalid prepared bitmap transaction");
    const auto asset = std::find_if(m_impl->document.assets.begin(), m_impl->document.assets.end(),
        [&](const auto &item) { return assetId(item) == change.assetId; });
    if (asset == m_impl->document.assets.end() ||
        (change.raster && std::get_if<RasterAsset>(&*asset) != change.raster) ||
        (change.chunks && (std::get_if<ChunkedRasterAsset>(&*asset) != change.chunks ||
                           change.chunkSize != m_impl->document.infiniteCanvas.chunkSize)) ||
        (change.raster && (!change.presence.empty() || change.canvasAfter)))
        return m_impl->failBitmap(DocumentFileCode::EditRejected, "prepared bitmap asset does not match its owner");
    if (m_impl->document.formatVersion.major != CurrentFormatMajor ||
        m_impl->document.formatVersion.minor != CurrentFormatMinor) {
        // Existing older model versions still use the fully validated model upgrade.
        useDraftFallback = true;
        return false;
    }
    std::unique_lock<SourceAccess> sourceLock;
    try {
        auto resources = m_impl->resources;
        std::optional<std::vector<std::uint8_t>> headerBefore, headerAfter;
        if (change.canvasAfter && *change.canvasAfter != canvasRegion(m_impl->document)) {
            const auto &region = *change.canvasAfter;
            if (!change.chunks || m_impl->document.canvasMode != CanvasMode::Infinite ||
                region.extent.width <= 0 || region.extent.height <= 0 ||
                std::int64_t(region.origin.x) + region.extent.width > INT32_MAX ||
                std::int64_t(region.origin.y) + region.extent.height > INT32_MAX)
                throw FileFailure(DocumentFileCode::EditRejected, "invalid prepared canvas region");
            headerBefore = detail::encodeWorkingHeader(m_impl->document);
            headerAfter = detail::encodeWorkingHeader(m_impl->document, region);
        }
        const auto oldAuthor = authorshipDump(m_impl->document.authorship);
        const auto newAuthor = authorshipDump(*change.authorship);
        if (oldAuthor.size() != resources.authorshipBytes || resources.stringBytes < oldAuthor.size())
            throw FileFailure(DocumentFileCode::Conflict, "authorship no longer matches resource accounting");
        if (newAuthor.size() > DocumentAuthorship::MaximumBytes ||
            newAuthor.size() > std::numeric_limits<std::uint32_t>::max())
            throw FileFailure(DocumentFileCode::LimitExceeded, "authorship exceeds the configured limit");
        const auto stringsWithoutAuthor = resources.stringBytes - oldAuthor.size();
        if (newAuthor.size() > m_impl->limits.maximumTotalStringBytes ||
            stringsWithoutAuthor > m_impl->limits.maximumTotalStringBytes - newAuthor.size())
            throw FileFailure(DocumentFileCode::LimitExceeded, "document strings exceed the configured limit");
        resources.stringBytes = stringsWithoutAuthor + newAuthor.size();
        resources.authorshipBytes = newAuthor.size();
        auto authorBefore = detail::encodeWorkingAuthorship(oldAuthor);
        auto authorAfter = detail::encodeWorkingAuthorship(newAuthor);

        std::vector<CanvasOrigin> beforeChunks, afterChunks;
        if (!change.presence.empty()) {
            if (!change.chunks || change.chunkSize <= 0)
                throw FileFailure(DocumentFileCode::EditRejected, "chunk presence requires a chunked bitmap");
            beforeChunks.reserve(change.chunks->chunks.size());
            for (const auto &chunk : change.chunks->chunks) beforeChunks.push_back({chunk.column, chunk.row});
            afterChunks = beforeChunks;
            const auto less = [](CanvasOrigin a, CanvasOrigin b) {
                return std::pair{a.y, a.x} < std::pair{b.y, b.x};
            };
            for (const auto &presence : change.presence) {
                const CanvasOrigin key{presence.column, presence.row};
                auto at = std::lower_bound(afterChunks.begin(), afterChunks.end(), key, less);
                const bool exists = at != afterChunks.end() && at->x == key.x && at->y == key.y;
                if (exists != presence.before || presence.before == presence.after)
                    throw FileFailure(DocumentFileCode::Conflict, "chunk presence differs from the prepared edit");
                if (presence.after) afterChunks.insert(at, key); else afterChunks.erase(at);
            }
            if (beforeChunks.size() > resources.rasterChunks ||
                afterChunks.size() > std::numeric_limits<std::uint32_t>::max())
                throw FileFailure(DocumentFileCode::LimitExceeded, "bitmap chunk count exceeds limit");
            resources.rasterChunks -= beforeChunks.size();
            if (afterChunks.size() > m_impl->limits.maximumRasterChunks ||
                resources.rasterChunks > m_impl->limits.maximumRasterChunks - afterChunks.size())
                throw FileFailure(DocumentFileCode::LimitExceeded, "bitmap chunk count exceeds limit");
            resources.rasterChunks += afterChunks.size();
            const auto pixelsPerChunk = std::uint64_t(change.chunkSize) * change.chunkSize;
            const auto oldPixels = std::uint64_t(beforeChunks.size()) * pixelsPerChunk;
            const auto nextPixels = std::uint64_t(afterChunks.size()) * pixelsPerChunk;
            if (oldPixels > resources.rasterPixels)
                throw FileFailure(DocumentFileCode::CorruptFile, "invalid raster resource accounting");
            resources.rasterPixels -= oldPixels;
            const auto maximum = std::min(m_impl->limits.maximumTotalRasterPixels,
                                          m_impl->limits.maximumContainerBytes / 4);
            if (nextPixels > maximum || resources.rasterPixels > maximum - nextPixels)
                throw FileFailure(DocumentFileCode::LimitExceeded, "raster pixel count exceeds limit");
            resources.rasterPixels += nextPixels;
        }

        sourceLock = std::unique_lock(*m_impl->access);
        auto *database = m_impl->database.get();
        Transaction transaction(database, true);
        if (database->hasMoved()
            || database->scalar("PRAGMA data_version") != m_impl->dataVersion
            || database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                   != static_cast<std::int64_t>(m_impl->revision))
            throw FileFailure(DocumentFileCode::Conflict, "the working file changed outside this session");
        if (m_impl->revision == std::uint64_t(INT64_MAX))
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file revision is exhausted");
        auto payload = m_impl->payloadBytes;
        DocumentFileWriteStatistics statistics;
        if (m_impl->storageVersion == 1) {
            auto records = detail::encodeDocumentRecords(m_impl->document, nullptr, m_impl->limits, true);
            requireEncoding(records.error);
            database->execute(detail::WorkingTileStateSchema);
            database->execute("INSERT INTO canvas_tile_state VALUES(1,0)");
            database->execute(detail::WorkingTileSchema);
            database->execute("PRAGMA user_version=2");
            addStatistics(statistics, writeRecords(database, records.records, m_impl->limits));
            auto migrationLimits = m_impl->limits;
            migrationLimits.maximumContainerBytes = std::numeric_limits<std::uint64_t>::max();
            addStatistics(statistics, detail::writeWorkingTiles(*database, m_impl->document, nullptr, migrationLimits));
            payload = storedPayloadBytes(database, StorageVersion, migrationLimits);
        }
        const auto count = database->scalar("SELECT tile_count FROM canvas_tile_state WHERE singleton=1");
        if (count < 0) throw FileFailure(DocumentFileCode::CorruptFile, "invalid stored tile count");
        const auto tiles = detail::writeWorkingTilePatches(*database, change);
        auto removedBytes = tiles.removedBytes, addedBytes = tiles.addedBytes;
        addStatistics(statistics, tiles.statistics);
        if (tiles.removedTiles > std::uint64_t(count) ||
            tiles.addedTiles > std::uint64_t(INT64_MAX) - (std::uint64_t(count) - tiles.removedTiles))
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file tile count exceeds limit");
        if (tiles.addedTiles || tiles.removedTiles) {
            Statement update(database, "UPDATE canvas_tile_state SET tile_count=? WHERE singleton=1");
            update.integer(1, std::int64_t(std::uint64_t(count) - tiles.removedTiles + tiles.addedTiles));
            update.done();
        }
        if (!change.presence.empty()) {
            const auto before = detail::encodeWorkingChunkDescriptor(change.assetId, beforeChunks, change.chunkSize);
            const auto after = detail::encodeWorkingChunkDescriptor(change.assetId, afterChunks, change.chunkSize);
            replacePreparedRecord(database, detail::RecordKind::Asset, std::string(change.assetId),
                                  before, after, removedBytes, addedBytes, statistics);
        }
        if (headerBefore && headerAfter)
            replacePreparedRecord(database,detail::RecordKind::Header,"",*headerBefore,*headerAfter,
                                  removedBytes,addedBytes,statistics);
        replacePreparedRecord(database, detail::RecordKind::Authorship, "", authorBefore, authorAfter,
                              removedBytes, addedBytes, statistics);
        changePayload(payload, removedBytes, addedBytes, m_impl->limits);
        database->execute("UPDATE canvas_state SET revision=revision+1 WHERE singleton=1");
        transaction.commit();
        ++m_impl->revision;
        m_impl->storageVersion = StorageVersion;
        m_impl->resources = resources;
        m_impl->payloadBytes = payload;
        m_impl->statistics = statistics;
        m_impl->result = {DocumentFileCode::None, true, {}};
        m_impl->bitmapCommitted = true;
        m_impl->bitmapNotification = true;
        return true;
    } catch (const FileFailure &error) {
        m_impl->verifyOutcome(); return m_impl->failBitmap(error.code, error.what());
    } catch (const iiFileProvider::native::FileError &error) {
        m_impl->verifyOutcome(); return m_impl->failBitmap(storageCode(error.code()), error.what());
    } catch (const std::bad_alloc &) {
        m_impl->verifyOutcome(); return m_impl->failBitmap(DocumentFileCode::LimitExceeded, "out of memory");
    } catch (const std::exception &error) {
        m_impl->verifyOutcome(); return m_impl->failBitmap(DocumentFileCode::EditRejected, error.what());
    } catch (...) {
        m_impl->verifyOutcome(); return m_impl->failBitmap(DocumentFileCode::EditRejected, "unknown bitmap commit failure");
    }
}

void DocumentFile::endBitmapEdit(bool accepted) noexcept
{
    if (!accepted && m_impl->bitmapCommitted) {
        // The disk state is durable but the owner did not publish its prepared
        // cursor. Detach rather than advertise a synchronized binding.
        m_impl->database.reset();
        ++m_impl->generation;
        m_impl->failBitmap(DocumentFileCode::IoError, "prepared bitmap publication failed");
    } else if (!accepted && m_impl->result.ok()) {
        m_impl->failBitmap(DocumentFileCode::EditRejected, "bitmap edit was rejected");
    }
    m_impl->editing = false;
    if (m_impl->bitmapNotification) {
        m_impl->bitmapNotification = false;
        iiFileProvider::native::Presentation::enqueue(m_impl->notificationPath);
    }
}

DocumentFileResult DocumentFile::editLayerPropertiesImpl(
    const std::string &layerId, const std::function<bool(Document &)> &edit,
    bool &useDraftFallback)
{
    useDraftFallback = false;
    m_impl->statistics = {};
    if (!isOpen()) return m_impl->fail(DocumentFileCode::NotOpen, "no working file is open");
    if (!edit || m_impl->editing)
        return m_impl->fail(DocumentFileCode::EditRejected, "empty or nested file edits are not allowed");
    if (m_impl->storageVersion != StorageVersion ||
        m_impl->document.formatVersion.major != CurrentFormatMajor ||
        m_impl->document.formatVersion.minor != CurrentFormatMinor) {
        useDraftFallback = true;
        return {};
    }
    Layer *layer = findLayer(m_impl->document, layerId);
    if (!layer) {
        // Preserve the existing DocumentEditor error and legacy validation order.
        useDraftFallback = true;
        return {};
    }
    static_assert(std::is_nothrow_swappable_v<LayerProperties>);
    static_assert(std::is_nothrow_swappable_v<DocumentAuthorship>);
    struct EditingGuard {
        bool &editing;
        explicit EditingGuard(bool &value) : editing(value) { editing = true; }
        ~EditingGuard() { editing = false; }
    } editingGuard(m_impl->editing);
    std::unique_lock<SourceAccess> sourceLock;
    try {
        auto before = detail::encodeDocumentRecords(m_impl->document, &m_impl->document,
                                                     m_impl->limits, true);
        requireEncoding(before.error);
        // The only callbacks supplied here are the private DocumentEditor
        // scalar-property operations. No user callback or notification runs
        // while the candidate is temporarily installed for validation.
        struct CandidateGuard {
            Document &document;
            LayerProperties &properties;
            LayerProperties savedProperties;
            DocumentAuthorship savedAuthorship;
            FormatVersion savedVersion;
            bool active = true;
            CandidateGuard(Document &value, LayerProperties &target)
                : document(value), properties(target), savedProperties(target),
                  savedAuthorship(value.authorship), savedVersion(value.formatVersion) {
                // Edit the copies. On rejection or a no-op, swapping the
                // originals back also preserves their owned buffer identity.
                using std::swap;
                swap(properties, savedProperties);
                swap(document.authorship, savedAuthorship);
            }
            void restore() noexcept {
                if (!active) return;
                using std::swap;
                swap(properties, savedProperties);
                swap(document.authorship, savedAuthorship);
                document.formatVersion = savedVersion;
                active = false;
            }
            ~CandidateGuard() { restore(); }
        } candidate(m_impl->document, layerProperties(*layer));
        if (!edit(m_impl->document))
            return m_impl->fail(DocumentFileCode::EditRejected, "the layer edit was rejected");
        if (m_impl->document.formatVersion.major != candidate.savedVersion.major ||
            m_impl->document.formatVersion.minor != candidate.savedVersion.minor)
            throw FileFailure(DocumentFileCode::EditRejected, "a layer property edit changed the document version");
        auto after = detail::encodeDocumentRecords(m_impl->document, &m_impl->document,
                                                    m_impl->limits, true);
        requireEncoding(after.error);
        const auto resources = detail::workingResourceTotals(m_impl->document, m_impl->limits);
        requireEncoding(resources.error);
        candidate.restore();

        if (before.records.size() != after.records.size())
            throw FileFailure(DocumentFileCode::EditRejected, "a layer property edit changed record structure");
        const detail::DocumentRecord *layerBefore = nullptr, *layerAfter = nullptr;
        const detail::DocumentRecord *authorBefore = nullptr, *authorAfter = nullptr;
        for (std::size_t i = 0; i < before.records.size(); ++i) {
            const auto &a = before.records[i], &b = after.records[i];
            if (a.kind != b.kind || a.id != b.id || a.position != b.position)
                throw FileFailure(DocumentFileCode::EditRejected, "a layer property edit changed record identity");
            if (a.kind == detail::RecordKind::Layer && a.id == layerId) {
                layerBefore = &a; layerAfter = &b;
            } else if (a.kind == detail::RecordKind::Authorship) {
                authorBefore = &a; authorAfter = &b;
            } else if (a.data != b.data) {
                throw FileFailure(DocumentFileCode::EditRejected, "a layer property edit changed another record");
            }
        }
        if (!layerBefore || !layerAfter || !authorBefore || !authorAfter ||
            !layerBefore->data || !layerAfter->data || !authorBefore->data || !authorAfter->data)
            throw FileFailure(DocumentFileCode::CorruptFile, "layer or authorship record is missing");

        // Prepare the notification path before durable publication: no
        // allocation may turn an already-committed edit into a reported error.
        const auto notificationPath = std::filesystem::u8path(m_impl->path);
        sourceLock = std::unique_lock(*m_impl->access);
        auto *database = m_impl->database.get();
        Transaction transaction(database, true);
        if (database->hasMoved() || database->scalar("PRAGMA data_version") != m_impl->dataVersion ||
            database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                != static_cast<std::int64_t>(m_impl->revision))
            throw FileFailure(DocumentFileCode::Conflict, "the working file changed outside this session");
        std::uint64_t removedBytes = 0, addedBytes = 0;
        DocumentFileWriteStatistics statistics;
        replacePreparedRecord(database, detail::RecordKind::Layer, layerId, *layerBefore->data,
                              *layerAfter->data, removedBytes, addedBytes, statistics);
        replacePreparedRecord(database, detail::RecordKind::Authorship, "", *authorBefore->data,
                              *authorAfter->data, removedBytes, addedBytes, statistics);
        if (!statistics.recordsWritten) {
            transaction.commit();
            m_impl->result = {};
            return m_impl->result;
        }
        if (m_impl->revision == std::uint64_t(INT64_MAX))
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file revision is exhausted");
        auto payload = m_impl->payloadBytes;
        changePayload(payload, removedBytes, addedBytes, m_impl->limits);
        database->execute("UPDATE canvas_state SET revision=revision+1 WHERE singleton=1");
        transaction.commit();
        using std::swap;
        swap(candidate.properties, candidate.savedProperties);
        swap(m_impl->document.authorship, candidate.savedAuthorship);
        ++m_impl->revision;
        m_impl->resources = resources.totals;
        m_impl->payloadBytes = payload;
        m_impl->statistics = statistics;
        m_impl->result = {DocumentFileCode::None, true, {}};
        sourceLock.unlock();
        iiFileProvider::native::Presentation::enqueue(notificationPath);
        return m_impl->result;
    } catch (const FileFailure &error) {
        m_impl->verifyOutcome(); return m_impl->fail(error.code, error.what());
    } catch (const iiFileProvider::native::FileError &error) {
        m_impl->verifyOutcome(); return m_impl->fail(storageCode(error.code()), error.what());
    } catch (const std::bad_alloc &) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::LimitExceeded, "out of memory");
    } catch (const std::exception &error) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::EditRejected, error.what());
    } catch (...) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::EditRejected, "unknown layer edit failure");
    }
}

DocumentFileResult DocumentFile::editLayerStructureImpl(
    const std::function<bool(Document &)> &edit, bool changesFrames, bool &useDraftFallback)
{
    useDraftFallback = false;
    m_impl->statistics = {};
    if (!isOpen()) return m_impl->fail(DocumentFileCode::NotOpen, "no working file is open");
    if (!edit || m_impl->editing)
        return m_impl->fail(DocumentFileCode::EditRejected, "empty or nested file edits are not allowed");
    if (m_impl->storageVersion != StorageVersion ||
        m_impl->document.formatVersion.major != CurrentFormatMajor ||
        m_impl->document.formatVersion.minor != CurrentFormatMinor) {
        useDraftFallback = true;
        return {};
    }
    static_assert(std::is_nothrow_swappable_v<std::vector<Layer>>);
    static_assert(std::is_nothrow_swappable_v<std::vector<Frame>>);
    static_assert(std::is_nothrow_swappable_v<DocumentAuthorship>);
    struct EditingGuard {
        bool &editing;
        explicit EditingGuard(bool &value) : editing(value) { editing = true; }
        ~EditingGuard() { editing = false; }
    } editingGuard(m_impl->editing);
    std::unique_lock<SourceAccess> sourceLock;
    try {
        auto before = detail::encodeDocumentRecords(m_impl->document, &m_impl->document,
                                                     m_impl->limits, true);
        requireEncoding(before.error);
        // Only the private layer insert/remove/move callbacks enter here.
        // Their candidate owns structural values, never any raster asset.
        struct CandidateGuard {
            Document &document;
            std::vector<Layer> savedLayers;
            std::optional<std::vector<Frame>> savedFrames;
            DocumentAuthorship savedAuthorship;
            FormatVersion savedVersion;
            bool active = true;
            CandidateGuard(Document &value, bool frames)
                : document(value), savedLayers(value.layers), savedAuthorship(value.authorship),
                  savedVersion(value.formatVersion) {
                if (frames) savedFrames.emplace(value.frames);
                using std::swap;
                swap(document.layers, savedLayers);
                if (savedFrames) swap(document.frames, *savedFrames);
                swap(document.authorship, savedAuthorship);
            }
            void restore() noexcept {
                if (!active) return;
                using std::swap;
                swap(document.layers, savedLayers);
                if (savedFrames) swap(document.frames, *savedFrames);
                swap(document.authorship, savedAuthorship);
                document.formatVersion = savedVersion;
                active = false;
            }
            ~CandidateGuard() { restore(); }
        } candidate(m_impl->document, changesFrames);
        if (!edit(m_impl->document))
            return m_impl->fail(DocumentFileCode::EditRejected, "the layer structure edit was rejected");
        if (m_impl->document.formatVersion.major != candidate.savedVersion.major ||
            m_impl->document.formatVersion.minor != candidate.savedVersion.minor)
            throw FileFailure(DocumentFileCode::EditRejected, "a layer structure edit changed the document version");
        auto after = detail::encodeDocumentRecords(m_impl->document, &m_impl->document,
                                                    m_impl->limits, true);
        requireEncoding(after.error);
        const auto resources = detail::workingResourceTotals(m_impl->document, m_impl->limits);
        requireEncoding(resources.error);
        candidate.restore();

        using RecordMap = std::map<RecordKey, const DocumentRecord *>;
        const auto index = [](const std::vector<DocumentRecord> &records) {
            RecordMap result;
            for (const auto &record : records) {
                if (!result.emplace(RecordKey{static_cast<int>(record.kind), record.id}, &record).second)
                    throw FileFailure(DocumentFileCode::CorruptFile, "duplicate generated record identity");
            }
            return result;
        };
        const auto original = index(before.records), replacement = index(after.records);
        const auto *layerCountBefore = static_cast<const DocumentRecord *>(nullptr);
        const auto *layerCountAfter = static_cast<const DocumentRecord *>(nullptr);
        const auto *authorBefore = static_cast<const DocumentRecord *>(nullptr);
        const auto *authorAfter = static_cast<const DocumentRecord *>(nullptr);
        std::vector<std::pair<const DocumentRecord *, const DocumentRecord *>> changedLayers;
        std::size_t added = 0, removed = 0;
        for (const auto &[key, oldRecord] : original) {
            const auto next = replacement.find(key);
            if (next == replacement.end()) {
                if (oldRecord->kind != detail::RecordKind::Layer)
                    throw FileFailure(DocumentFileCode::EditRejected, "a structural layer edit removed another record");
                ++removed; changedLayers.emplace_back(oldRecord, nullptr); continue;
            }
            const auto *newRecord = next->second;
            if (oldRecord->kind == detail::RecordKind::Layer) {
                if (!oldRecord->data || !newRecord->data || oldRecord->data != newRecord->data)
                    throw FileFailure(DocumentFileCode::EditRejected, "a structural layer edit changed another layer's content");
                if (oldRecord->position != newRecord->position)
                    changedLayers.emplace_back(oldRecord, newRecord);
            } else if (oldRecord->kind == detail::RecordKind::LayerCount) {
                layerCountBefore=oldRecord; layerCountAfter=newRecord;
            } else if (oldRecord->kind == detail::RecordKind::Authorship) {
                authorBefore=oldRecord; authorAfter=newRecord;
            } else if (oldRecord->position != newRecord->position || oldRecord->data != newRecord->data) {
                throw FileFailure(DocumentFileCode::EditRejected, "a structural layer edit changed another record");
            }
        }
        for (const auto &[key, newRecord] : replacement) {
            if (original.find(key) == original.end()) {
                if (newRecord->kind != detail::RecordKind::Layer || !newRecord->data)
                    throw FileFailure(DocumentFileCode::EditRejected, "a structural layer edit inserted another record");
                ++added; changedLayers.emplace_back(nullptr, newRecord);
            }
        }
        if (added > 1 || removed > 1 || (added && removed) || !layerCountBefore || !layerCountAfter ||
            !authorBefore || !authorAfter || !layerCountBefore->data || !layerCountAfter->data ||
            !authorBefore->data || !authorAfter->data ||
            layerCountBefore->position != layerCountAfter->position || authorBefore->position != authorAfter->position)
            throw FileFailure(DocumentFileCode::EditRejected, "invalid layer structure record transition");

        const auto notificationPath = std::filesystem::u8path(m_impl->path);
        sourceLock = std::unique_lock(*m_impl->access);
        auto *database = m_impl->database.get();
        Transaction transaction(database, true);
        if (database->hasMoved() || database->scalar("PRAGMA data_version") != m_impl->dataVersion ||
            database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                != static_cast<std::int64_t>(m_impl->revision))
            throw FileFailure(DocumentFileCode::Conflict, "the working file changed outside this session");

        std::uint64_t removedBytes = 0, addedBytes = 0;
        DocumentFileWriteStatistics statistics;
        // Both singleton records are verified even for a no-op. Every moved or
        // removed layer is checked against its original position, bytes and hash.
        replacePreparedRecord(database, detail::RecordKind::LayerCount, "", *layerCountBefore->data,
                              *layerCountAfter->data, removedBytes, addedBytes, statistics);
        replacePreparedRecord(database, detail::RecordKind::Authorship, "", *authorBefore->data,
                              *authorAfter->data, removedBytes, addedBytes, statistics);
        for (const auto &[oldRecord, newRecord] : changedLayers) {
            if (oldRecord) {
                const auto rowId=verifyPreparedLayer(database, *oldRecord);
                if (newRecord) {
                    Statement update(database, "UPDATE canvas_records SET position=? WHERE rowid=?");
                    update.integer(1,newRecord->position); update.integer(2,rowId); update.done();
                } else {
                    if (oldRecord->data->size() > std::numeric_limits<std::uint64_t>::max()-removedBytes)
                        throw FileFailure(DocumentFileCode::LimitExceeded, "layer payload accounting overflow");
                    removedBytes+=oldRecord->data->size();
                    Statement remove(database, "DELETE FROM canvas_records WHERE rowid=?");
                    remove.integer(1,rowId); remove.done();
                }
                ++statistics.recordsWritten;
            } else {
                Statement existing(database, "SELECT rowid FROM canvas_records WHERE kind=? AND id=?");
                existing.integer(1,static_cast<int>(detail::RecordKind::Layer)); existing.text(2,newRecord->id);
                if (existing.row()) throw FileFailure(DocumentFileCode::Conflict, "inserted layer already exists in storage");
                if (newRecord->data->size() > std::numeric_limits<std::uint64_t>::max()-addedBytes)
                    throw FileFailure(DocumentFileCode::LimitExceeded, "layer payload accounting overflow");
                addedBytes+=newRecord->data->size();
                const auto hash=digest(*newRecord->data);
                Statement insert(database, "INSERT INTO canvas_records(kind,id,position,data,digest) VALUES(?,?,?,?,?)");
                insert.integer(1,static_cast<int>(detail::RecordKind::Layer)); insert.text(2,newRecord->id);
                insert.integer(3,newRecord->position); insert.bytes(4,newRecord->data->data(),newRecord->data->size());
                insert.bytes(5,hash.data(),hash.size()); insert.done();
                statistics.payloadBytesWritten+=newRecord->data->size(); ++statistics.recordsWritten;
            }
        }
        if (!statistics.recordsWritten) {
            transaction.commit(); m_impl->result = {}; return m_impl->result;
        }
        if (m_impl->revision == std::uint64_t(INT64_MAX))
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file revision is exhausted");
        auto payload=m_impl->payloadBytes;
        changePayload(payload, removedBytes, addedBytes, m_impl->limits);
        database->execute("UPDATE canvas_state SET revision=revision+1 WHERE singleton=1");
        transaction.commit();
        using std::swap;
        swap(m_impl->document.layers,candidate.savedLayers);
        if (candidate.savedFrames) swap(m_impl->document.frames,*candidate.savedFrames);
        swap(m_impl->document.authorship,candidate.savedAuthorship);
        ++m_impl->revision;
        m_impl->resources=resources.totals;
        m_impl->payloadBytes=payload;
        m_impl->statistics=statistics;
        m_impl->result={DocumentFileCode::None,true,{}};
        sourceLock.unlock();
        iiFileProvider::native::Presentation::enqueue(notificationPath);
        return m_impl->result;
    } catch (const FileFailure &error) {
        m_impl->verifyOutcome(); return m_impl->fail(error.code,error.what());
    } catch (const iiFileProvider::native::FileError &error) {
        m_impl->verifyOutcome(); return m_impl->fail(storageCode(error.code()),error.what());
    } catch (const std::bad_alloc &) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::LimitExceeded,"out of memory");
    } catch (const std::exception &error) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::EditRejected,error.what());
    } catch (...) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::EditRejected,"unknown layer structure edit failure");
    }
}

DocumentFileResult DocumentFile::editTimelineImpl(
    const std::function<bool(Document &)> &edit, bool &useDraftFallback)
{
    useDraftFallback=false;
    m_impl->statistics={};
    if (!isOpen()) return m_impl->fail(DocumentFileCode::NotOpen,"no working file is open");
    if (!edit || m_impl->editing)
        return m_impl->fail(DocumentFileCode::EditRejected,"empty or nested file edits are not allowed");
    if (m_impl->storageVersion!=StorageVersion ||
        m_impl->document.formatVersion.major!=CurrentFormatMajor ||
        m_impl->document.formatVersion.minor!=CurrentFormatMinor) {
        useDraftFallback=true;
        return {};
    }
    struct EditingGuard {
        bool &editing;
        explicit EditingGuard(bool &value):editing(value) { editing=true; }
        ~EditingGuard() { editing=false; }
    } editing(m_impl->editing);
    std::unique_lock<SourceAccess> sourceLock;
    try {
        auto before=detail::encodeDocumentRecords(m_impl->document,&m_impl->document,m_impl->limits,true);
        requireEncoding(before.error);
        detail::TimelineGuard candidate(m_impl->document);
        if (!edit(m_impl->document))
            return m_impl->fail(DocumentFileCode::EditRejected,"the timeline edit was rejected");
        if (m_impl->document.formatVersion.major!=CurrentFormatMajor ||
            m_impl->document.formatVersion.minor!=CurrentFormatMinor)
            throw FileFailure(DocumentFileCode::EditRejected,"a timeline edit changed the document version");
        auto after=detail::encodeDocumentRecords(m_impl->document,&m_impl->document,m_impl->limits,true);
        requireEncoding(after.error);
        const auto resources=detail::workingResourceTotals(m_impl->document,m_impl->limits);
        requireEncoding(resources.error);
        candidate.restore();
        if (before.records.size()!=after.records.size())
            throw FileFailure(DocumentFileCode::EditRejected,"a timeline edit changed record structure");
        const detail::DocumentRecord *headerBefore=nullptr,*headerAfter=nullptr;
        const detail::DocumentRecord *authorBefore=nullptr,*authorAfter=nullptr;
        for (std::size_t i=0;i<before.records.size();++i) {
            const auto &a=before.records[i], &b=after.records[i];
            if (a.kind!=b.kind || a.id!=b.id || a.position!=b.position)
                throw FileFailure(DocumentFileCode::EditRejected,"a timeline edit changed record identity");
            if (a.kind==detail::RecordKind::Header) { headerBefore=&a; headerAfter=&b; }
            else if (a.kind==detail::RecordKind::Authorship) { authorBefore=&a; authorAfter=&b; }
            else if (a.data!=b.data)
                throw FileFailure(DocumentFileCode::EditRejected,"a timeline edit changed another record");
        }
        if (!headerBefore || !headerAfter || !authorBefore || !authorAfter ||
            !headerBefore->data || !headerAfter->data || !authorBefore->data || !authorAfter->data)
            throw FileFailure(DocumentFileCode::CorruptFile,"timeline or authorship record is missing");
        const auto notificationPath=std::filesystem::u8path(m_impl->path);
        sourceLock=std::unique_lock(*m_impl->access);
        auto *database=m_impl->database.get();
        Transaction transaction(database,true);
        if (database->hasMoved() || database->scalar("PRAGMA data_version")!=m_impl->dataVersion ||
            database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                !=static_cast<std::int64_t>(m_impl->revision))
            throw FileFailure(DocumentFileCode::Conflict,"the working file changed outside this session");
        std::uint64_t removedBytes=0,addedBytes=0;
        DocumentFileWriteStatistics statistics;
        replacePreparedRecord(database,detail::RecordKind::Header,"",*headerBefore->data,*headerAfter->data,
                              removedBytes,addedBytes,statistics);
        replacePreparedRecord(database,detail::RecordKind::Authorship,"",*authorBefore->data,*authorAfter->data,
                              removedBytes,addedBytes,statistics);
        if (!statistics.recordsWritten) {
            transaction.commit(); m_impl->result={}; return m_impl->result;
        }
        if (m_impl->revision==std::uint64_t(INT64_MAX))
            throw FileFailure(DocumentFileCode::LimitExceeded,"working-file revision is exhausted");
        auto payload=m_impl->payloadBytes;
        changePayload(payload,removedBytes,addedBytes,m_impl->limits);
        database->execute("UPDATE canvas_state SET revision=revision+1 WHERE singleton=1");
        transaction.commit();
        candidate.publish();
        ++m_impl->revision;
        m_impl->resources=resources.totals; m_impl->payloadBytes=payload; m_impl->statistics=statistics;
        m_impl->result={DocumentFileCode::None,true,{}};
        sourceLock.unlock();
        iiFileProvider::native::Presentation::enqueue(notificationPath);
        return m_impl->result;
    } catch (const FileFailure &error) {
        m_impl->verifyOutcome(); return m_impl->fail(error.code,error.what());
    } catch (const iiFileProvider::native::FileError &error) {
        m_impl->verifyOutcome(); return m_impl->fail(storageCode(error.code()),error.what());
    } catch (const std::bad_alloc &) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::LimitExceeded,"out of memory");
    } catch (const std::exception &error) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::EditRejected,error.what());
    } catch (...) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::EditRejected,"unknown timeline edit failure");
    }
}

DocumentFileResult DocumentFile::editAssetLayerImpl(
    std::string_view changedAsset,
    const std::function<bool(Document &,detail::AssetLayerGuard &)> &edit,
    bool &useDraftFallback)
{
    useDraftFallback=false;
    m_impl->statistics={};
    if (!isOpen()) return m_impl->fail(DocumentFileCode::NotOpen,"no working file is open");
    if (!edit || m_impl->editing)
        return m_impl->fail(DocumentFileCode::EditRejected,"empty or nested file edits are not allowed");
    if (m_impl->storageVersion!=StorageVersion ||
        m_impl->document.formatVersion.major!=CurrentFormatMajor ||
        m_impl->document.formatVersion.minor!=CurrentFormatMinor) {
        useDraftFallback=true;
        return {};
    }
    struct EditingGuard {
        bool &editing;
        explicit EditingGuard(bool &value):editing(value) { editing=true; }
        ~EditingGuard() { editing=false; }
    } editing(m_impl->editing);
    std::unique_lock<SourceAccess> sourceLock;
    try {
        // The private caller can change only one asset and one layer. The
        // candidate moves existing payloads, so large raster/audio buffers are
        // never cloned. Its guard restores memory on any precommit failure.
        detail::AssetLayerGuard candidate(m_impl->document);
        if (!edit(m_impl->document,candidate))
            return m_impl->fail(DocumentFileCode::EditRejected,"the asset-layer edit was rejected");
        auto records=detail::encodeDocumentRecords(m_impl->document,&m_impl->document,
                                                    m_impl->limits,true,changedAsset);
        requireEncoding(records.error);
        const auto resources=detail::workingResourceTotals(m_impl->document,m_impl->limits);
        requireEncoding(resources.error);
        const auto notificationPath=std::filesystem::u8path(m_impl->path);
        sourceLock=std::unique_lock(*m_impl->access);
        auto *database=m_impl->database.get();
        Transaction transaction(database,true);
        if (database->hasMoved() || database->scalar("PRAGMA data_version")!=m_impl->dataVersion ||
            database->scalar("SELECT revision FROM canvas_state WHERE singleton=1")
                !=static_cast<std::int64_t>(m_impl->revision))
            throw FileFailure(DocumentFileCode::Conflict,"the working file changed outside this session");
        RecordPayloadDelta metadata;
        auto statistics=writeRecords(database,records.records,m_impl->limits,&metadata);
        const auto action = findAsset(m_impl->document, std::string(changedAsset))
            ? detail::WorkingAssetTileAction::Insert : detail::WorkingAssetTileAction::Remove;
        const auto tiles = detail::writeWorkingAssetLayerTiles(*database,m_impl->document,changedAsset,action);
        addStatistics(statistics, tiles.statistics);
        if (!statistics.recordsWritten)
            throw FileFailure(DocumentFileCode::EditRejected,"the asset-layer edit did not change storage");
        if (m_impl->revision==std::uint64_t(INT64_MAX))
            throw FileFailure(DocumentFileCode::LimitExceeded,"working-file revision is exhausted");
        auto payload = m_impl->payloadBytes;
        changePayload(payload,metadata.removedBytes,0,m_impl->limits);
        changePayload(payload,tiles.removedBytes,0,m_impl->limits);
        changePayload(payload,0,metadata.addedBytes,m_impl->limits);
        changePayload(payload,0,tiles.addedBytes,m_impl->limits);
        database->execute("UPDATE canvas_state SET revision=revision+1 WHERE singleton=1");
        transaction.commit();
        candidate.publish();
        ++m_impl->revision;
        m_impl->resources=resources.totals;
        m_impl->payloadBytes=payload;
        m_impl->statistics=statistics;
        m_impl->result={DocumentFileCode::None,true,{}};
        sourceLock.unlock();
        iiFileProvider::native::Presentation::enqueue(notificationPath);
        return m_impl->result;
    } catch (const FileFailure &error) {
        m_impl->verifyOutcome(); return m_impl->fail(error.code,error.what());
    } catch (const iiFileProvider::native::FileError &error) {
        m_impl->verifyOutcome(); return m_impl->fail(storageCode(error.code()),error.what());
    } catch (const std::bad_alloc &) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::LimitExceeded,"out of memory");
    } catch (const std::exception &error) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::EditRejected,error.what());
    } catch (...) {
        m_impl->verifyOutcome(); return m_impl->fail(DocumentFileCode::EditRejected,"unknown asset-layer edit failure");
    }
}

DocumentFileResult DocumentFile::editImpl(const std::function<bool(Document &)> &edit, bool stampAuthorship)
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
    // Keep rollback and outcome verification inside the same source scope.
    std::unique_lock<SourceAccess> sourceLock;
    try {
        Document draft = m_impl->document;
        if (!edit(draft)) {
            return m_impl->fail(DocumentFileCode::EditRejected, "the edit callback rejected its draft");
        }
        auto encoded = detail::encodeDocumentRecords(draft, &m_impl->document, m_impl->limits,
                                                      m_impl->storageVersion == 2);
        requireEncoding(encoded.error);
        sourceLock = std::unique_lock(*m_impl->access);
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
        if (m_impl->storageVersion == 2)
            addStatistics(statistics, detail::writeWorkingTiles(*database, draft, &m_impl->document, m_impl->limits));
        if (statistics.recordsWritten == 0) {
            transaction.commit();
            // Selecting an already recorded author changes only local edit context.
            m_impl->document.authorship = std::move(draft.authorship);
            m_impl->result = {};
            return m_impl->result;
        }
        if (stampAuthorship && draft.authorship.dump() == m_impl->document.authorship.dump()) {
            recordDocumentChange(draft);
            auto stamped = detail::encodeDocumentRecords(draft, &m_impl->document, m_impl->limits,
                                                          m_impl->storageVersion == 2);
            requireEncoding(stamped.error);
            const auto metadataWrites = writeRecords(database, stamped.records, m_impl->limits);
            statistics.recordsWritten += metadataWrites.recordsWritten;
            statistics.payloadBytesWritten += metadataWrites.payloadBytesWritten;
        }
        if (m_impl->revision == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw FileFailure(DocumentFileCode::LimitExceeded, "working-file revision is exhausted");
        }
        if (m_impl->storageVersion == 1) {
            auto separated = detail::encodeDocumentRecords(draft, nullptr, m_impl->limits, true);
            requireEncoding(separated.error);
            database->execute(detail::WorkingTileStateSchema);
        database->execute("INSERT INTO canvas_tile_state VALUES(1,0)");
        database->execute(detail::WorkingTileSchema);
            database->execute("PRAGMA user_version=2");
            addStatistics(statistics, writeRecords(database, separated.records, m_impl->limits));
            addStatistics(statistics, detail::writeWorkingTiles(*database, draft, nullptr, m_impl->limits));
        }
        const auto resources = detail::workingResourceTotals(draft, m_impl->limits);
        requireEncoding(resources.error);
        const auto payloadBytes = storedPayloadBytes(database, StorageVersion, m_impl->limits);
        detail::PreparedDocumentCommit prepared(m_impl->document, draft);
        database->execute("UPDATE canvas_state SET revision=revision+1 WHERE singleton=1");
        transaction.commit();
        prepared.commit();
        ++m_impl->revision;
        m_impl->storageVersion = StorageVersion;
        m_impl->resources = resources.totals;
        m_impl->payloadBytes = payloadBytes;
        m_impl->statistics = statistics;
        m_impl->result = {DocumentFileCode::None, true, {}};
        // Queue backpressure may wait for the worker that needs this source.
        sourceLock.unlock();
        iiFileProvider::native::Presentation::enqueue(std::filesystem::u8path(m_impl->path));
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
    } catch (const iiFileProvider::native::FileError &error) {
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
