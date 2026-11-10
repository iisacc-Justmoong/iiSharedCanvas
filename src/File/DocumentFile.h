#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"
#include "Serialization/IiscCodec.h"

#include <functional>
#include <memory>
#include <string>

namespace iiSharedCanvas {

namespace detail { struct PreparedBitmapChange; class IiscInputAccess; class AssetLayerGuard; class BitmapHistoryPublication; }

enum class DocumentFileCode {
    None,
    NotOpen,
    AlreadyOpen,
    InvalidPath,
    AlreadyExists,
    InvalidDocument,
    UnsupportedFormat,
    CorruptFile,
    LimitExceeded,
    EditRejected,
    Conflict,
    IoError,
    InvalidArgument,
};

#ifdef IISHAREDCANVAS_NATIVE_API
// SQLite backup deadline, not a deadline for the complete snapshot read.
inline constexpr int DefaultNativeSnapshotTimeoutMs = 300000;
#endif

struct DocumentFileResult {
    DocumentFileCode code = DocumentFileCode::None;
    bool changed = false;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return code == DocumentFileCode::None; }
};

struct DocumentFileWriteStatistics {
    std::uint64_t recordsWritten = 0;
    // Logical payload bytes, excluding SQLite pages, journal, hashes and indices.
    std::uint64_t payloadBytesWritten = 0;
};

struct DocumentFileInfo {
    std::uint64_t revision = 0;
    std::uint32_t storageVersion = 0;
};

// A single-threaded authoring owner. Renderers receive detached Document copies.
// The file must outlive bound editors/items. No save, debounce, or close-time dump.
class IISHAREDCANVAS_EXPORT DocumentFile final {
public:
    DocumentFile();
    ~DocumentFile();
    DocumentFile(const DocumentFile &) = delete;
    DocumentFile &operator=(const DocumentFile &) = delete;
    DocumentFile(DocumentFile &&) = delete;
    DocumentFile &operator=(DocumentFile &&) = delete;

    DocumentFileResult create(const std::string &path, const Document &document,
                              SerializationLimits limits = {});
    DocumentFileResult open(const std::string &path, SerializationLimits limits = {});
    // Read the complete native snapshot/committed working-file state without
    // authoring bindings, source writes, format migration or preview-size caps.
    // UTF-8 local path; detached output is replaced only after a successful read.
    static DocumentFileResult readSnapshot(const std::string &path, Document &snapshot,
                                           SerializationLimits limits = {});
#ifdef IISHAREDCANVAS_NATIVE_API
    // The positive millisecond limit applies only to the SQLite backup copy.
    // Opening, decoding and validation are not bounded by this value.
    static DocumentFileResult readSnapshot(const std::string &path, Document &snapshot,
                                           SerializationLimits limits, int backupTimeoutMs);
#endif
    // Inspect only the committed storage revision through a read-only connection.
    // No authoring binding, pixel decoding, journal reconfiguration or writes.
    // Failure preserves the output. This does not validate the document records.
    static DocumentFileResult readRevision(const std::string &path, std::uint64_t &revision);
    // The two values describe the same read-only SQL transaction. This validates
    // storage schema/integrity, not the document payloads. Failure preserves info.
    static DocumentFileResult readStorageInfo(const std::string &path, DocumentFileInfo &info);
    void close() noexcept;

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const Document *document() const noexcept;
    [[nodiscard]] const std::string &filePath() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint32_t storageVersion() const noexcept;
    [[nodiscard]] std::uint64_t bindingGeneration() const noexcept;
    [[nodiscard]] const DocumentFileResult &lastResult() const noexcept;
    [[nodiscard]] DocumentFileWriteStatistics lastWriteStatistics() const noexcept;

    // The draft is temporary and must not escape the callback. Returning true
    // validates and durably commits all changes together; false/throw discards it.
    DocumentFileResult edit(const std::function<bool(Document &)> &edit);

    // Upgrade only the persisted model version in the normal synchronous
    // transaction. Keep artwork and authorship unchanged; current files are no-ops.
    DocumentFileResult upgradeFormat();

    // Update private working storage, preserving model, pixels and authorship.
    // Opening or a no-op edit never migrates a legacy working file.
    DocumentFileResult upgradeStorage();

private:
    friend class DocumentEditor;
    friend class DocumentHistory;
    friend class BitmapEditor;
    friend class ChunkedBitmapEditor;
    friend class CanvasItem;
    friend class detail::IiscInputAccess;
    friend class detail::BitmapHistoryPublication;
    [[nodiscard]] Document *boundDocument() noexcept;
    DocumentFileResult editImpl(const std::function<bool(Document &)> &edit, bool stampAuthorship);
#ifdef IISHAREDCANVAS_NATIVE_API
    DocumentFileResult editLayerPropertiesImpl(const std::string &layerId,
        const std::function<bool(Document &)> &edit, bool &useDraftFallback);
    DocumentFileResult editLayerStructureImpl(const std::function<bool(Document &)> &edit,
        bool changesFrames, bool &useDraftFallback);
    DocumentFileResult editAssetLayerImpl(std::string_view changedAsset,
        const std::function<bool(Document &,detail::AssetLayerGuard &)> &edit,
        bool &useDraftFallback);
    DocumentFileResult editTimelineImpl(const std::function<bool(Document &)> &edit,
        bool &useDraftFallback);
    bool beginBitmapEdit(std::uint64_t expectedGeneration);
    bool commitBitmapEdit(const detail::PreparedBitmapChange &change, bool &useDraftFallback);
    void endBitmapEdit(bool accepted) noexcept;
#endif
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace iiSharedCanvas
