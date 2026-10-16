#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"
#include "Serialization/IiscCodec.h"

#include <functional>
#include <memory>
#include <string>

namespace iiSharedCanvas {

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
};

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
    // Inspect only the committed storage revision through a read-only connection.
    // No authoring binding, pixel decoding, journal reconfiguration or writes.
    // Failure preserves the output. This does not validate the document records.
    static DocumentFileResult readRevision(const std::string &path, std::uint64_t &revision);
    void close() noexcept;

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const Document *document() const noexcept;
    [[nodiscard]] const std::string &filePath() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::uint64_t bindingGeneration() const noexcept;
    [[nodiscard]] const DocumentFileResult &lastResult() const noexcept;
    [[nodiscard]] DocumentFileWriteStatistics lastWriteStatistics() const noexcept;

    // The draft is temporary and must not escape the callback. Returning true
    // validates and durably commits all changes together; false/throw discards it.
    DocumentFileResult edit(const std::function<bool(Document &)> &edit);

    // Upgrade only the persisted model version in the normal synchronous
    // transaction. Keep artwork and authorship unchanged; current files are no-ops.
    DocumentFileResult upgradeFormat();

private:
    friend class DocumentEditor;
    friend class BitmapEditor;
    friend class ChunkedBitmapEditor;
    friend class CanvasItem;
    [[nodiscard]] Document *boundDocument() noexcept;
    DocumentFileResult editImpl(const std::function<bool(Document &)> &edit, bool stampAuthorship);
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace iiSharedCanvas
