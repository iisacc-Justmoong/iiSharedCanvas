#pragma once

#include "Bitmap/BitmapEdit.h"
#include "Document/DocumentEditor.h"
#include "iiSharedCanvas/Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

struct StrokePoint;

namespace iiSharedCanvas {
class DocumentFile;
struct BitmapBrush;

enum class DocumentHistoryOverflow { Reject, EvictOldest };
enum class StrokeCanvasMode { Clip, Grow };
enum class PixelPreviewCanvasMode { Clip, Grow };
struct DocumentHistoryLimits {
    std::size_t maximumBytes = 64 * 1024 * 1024;
    std::size_t maximumEntries = 32;
    std::size_t maximumBitmapPatches = 65536;
    std::size_t maximumBitmapInputBytes = 1024ULL * 1024 * 1024;
    DocumentHistoryOverflow overflow = DocumentHistoryOverflow::Reject;
};
enum class DocumentHistoryCode {
    None, NotBound, InvalidArgument, BudgetExceeded, LimitExceeded, Conflict,
    EditRejected, PersistenceFailure, NoUndo, NoRedo, Reentrant, PreviewActive, NoPreview
};
struct DocumentHistoryResult {
    DocumentHistoryCode code = DocumentHistoryCode::None;
    bool changed = false;
    std::size_t entryBytes = 0;
    std::size_t evictedEntries = 0;
    std::size_t discardedRedoEntries = 0;
    [[nodiscard]] bool ok() const noexcept { return code == DocumentHistoryCode::None; }
    [[nodiscard]] std::string_view message() const noexcept;
};
struct DocumentHistoryStatistics {
    std::size_t undoEntries = 0, redoEntries = 0;
    // Logical retained command storage; not allocator overhead or process RSS.
    std::size_t retainedBytes = 0;
};

// Single-threaded coordinator. The document or file must outlive this object.
// Callers must route tracked edits through this instance; reset explicitly
// discards history and rebases after another editor changes the owner.
class IISHAREDCANVAS_EXPORT DocumentHistory final {
public:
    explicit DocumentHistory(Document &document, DocumentHistoryLimits limits = {});
    explicit DocumentHistory(DocumentFile &file, DocumentHistoryLimits limits = {});
    ~DocumentHistory();
    DocumentHistory(const DocumentHistory &) = delete;
    DocumentHistory &operator=(const DocumentHistory &) = delete;
    DocumentHistory(DocumentHistory &&) = delete;
    DocumentHistory &operator=(DocumentHistory &&) = delete;

    [[nodiscard]] bool isBound() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    // Views stay valid until the next mutating history call or destruction.
    [[nodiscard]] std::string_view undoLabel() const noexcept;
    [[nodiscard]] std::string_view redoLabel() const noexcept;
    [[nodiscard]] DocumentHistoryStatistics statistics() const noexcept;
    [[nodiscard]] const DocumentHistoryLimits &limits() const noexcept;

    // All patches address one finite or chunked bitmap asset. They are borrowed
    // only for this call and become one compact, reversible history entry.
    DocumentHistoryResult replaceBitmapPatches(std::string_view assetId,
        std::span<const BitmapPatch> patches, std::string_view label = "Bitmap edit");
    // The live result is written to the owner on every successful replacement.
    // Resolve the session explicitly before destroying this history object.
    // For live replacement and cancellation, changed reports a document
    // revision; for commit it reports publication of a non-empty entry.
    DocumentHistoryResult beginPixelPreview(std::string_view assetId,
        std::string_view label = "Pixel edit");
    // Unlike a live preview, staged replacements leave the owner untouched.
    // Grow is valid only for chunked infinite canvases. Each tile view remains
    // valid until the next mutating history call or destruction; the caller
    // renders final pixels as replacements with the layer transform. Grow uses
    // the untransformed chunk coordinates as canvas coordinates; it does not
    // account for transforms of layers that refer to the asset.
    // Staged replacement/cancellation report changed when the view must redraw.
    DocumentHistoryResult beginStagedPixelPreview(std::string_view assetId,
        PixelPreviewCanvasMode canvasMode = PixelPreviewCanvasMode::Clip,
        std::string_view label = "Pixel edit");
    [[nodiscard]] std::vector<BitmapPreviewTile> stagedPixelPreviewTiles() const;
    [[nodiscard]] std::optional<CanvasRegion> stagedPixelPreviewRegion() const noexcept;
    DocumentHistoryResult replacePixelPreview(std::span<const BitmapPatch> patches);
    DocumentHistoryResult commitPixelPreview();
    DocumentHistoryResult cancelPixelPreview();
    // Recovery for an unresolvable conflict: keep the current document pixels,
    // discard both history stacks, and release the session without a write.
    DocumentHistoryResult abandonPixelPreview();
    [[nodiscard]] bool pixelPreviewActive() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> pixelPreviewSourceAt(int x, int y) const noexcept;
    [[nodiscard]] std::size_t previewStorageBytes() const noexcept;
    [[nodiscard]] std::size_t previewPendingHistoryBytes() const noexcept;
    // Dirty bounds accumulate until cleared and remain readable after cancel.
    [[nodiscard]] DevicePixelRect previewDirtyBounds() const noexcept;
    void clearPreviewDirtyBounds() noexcept;
    // A streaming brush stroke owns the same global cursor as pixel previews.
    // Samples are processed synchronously; the input sequence is not retained.
    // End publishes one entry. Cancel restores the original pixels and keeps
    // the pre-existing redo branch. Resolve explicitly before destruction.
    DocumentHistoryResult beginStroke(std::string_view assetId, const BitmapBrush &brush,
        const StrokePoint &sample, std::string_view label = "Brush stroke");
    // Grow is valid only for chunked infinite canvases. Changed pixels outside
    // the original region grow the region, in the same reversible command.
    DocumentHistoryResult beginStroke(std::string_view assetId, const BitmapBrush &brush,
        const StrokePoint &sample, StrokeCanvasMode canvasMode,
        std::string_view label = "Brush stroke");
    DocumentHistoryResult continueStroke(const StrokePoint &sample);
    DocumentHistoryResult endStroke(const StrokePoint &sample);
    DocumentHistoryResult advanceStrokeTime(double seconds);
    DocumentHistoryResult rebaseStrokeTime(double seconds);
    DocumentHistoryResult cancelStroke();
    // Explicit conflict recovery: preserve the current pixels but discard all
    // history and the unresolvable stroke.
    DocumentHistoryResult abandonStroke();
    [[nodiscard]] bool strokeActive() const noexcept;
    [[nodiscard]] std::size_t strokeStorageBytes() const noexcept;
    [[nodiscard]] std::size_t strokePendingHistoryBytes() const noexcept;
    [[nodiscard]] DevicePixelRect strokeDirtyBounds() const noexcept;
    void clearStrokeDirtyBounds() noexcept;
    DocumentHistoryResult setLayerName(std::string_view id, std::string_view name);
    DocumentHistoryResult setLayerVisible(std::string_view id, bool visible);
    DocumentHistoryResult setLayerOpacity(std::string_view id, double opacity);
    DocumentHistoryResult setLayerTransform(std::string_view id, AffineTransform transform);
    DocumentHistoryResult setLayerBlendMode(std::string_view id, RasterBlendMode mode);
    DocumentHistoryResult moveLayer(std::string_view id, std::size_t destinationIndex);
    // Frame rate and frame count change together as one validated command.
    // Existing frames, motion keys and audio clips are never silently trimmed.
    DocumentHistoryResult setTimeline(Timeline timeline, std::string_view label = "Document timing");
    // A layer references already existing assets. Removing it retains those
    // assets and records its frame-owned keys for an atomic undo.
    DocumentHistoryResult insertLayer(Layer layer, std::size_t index = AppendDocumentIndex,
        std::string_view label = "Add layer");
    DocumentHistoryResult insertKeyframedLayer(Layer layer, std::vector<KeyframePlacement> keyframes,
        std::size_t index = AppendDocumentIndex, std::string_view label = "Add layer");
    DocumentHistoryResult removeLayer(std::string_view id, std::string_view label = "Remove layer");
    // Atomically creates one static layer and its new raster, chunked raster,
    // or vector asset. Undo removes both; redo restores the lossless content.
    DocumentHistoryResult insertLayerWithAsset(Asset asset, Layer layer,
        std::size_t layerIndex = AppendDocumentIndex,
        std::size_t assetIndex = AppendDocumentIndex,
        std::string_view label = "Create layer");
    // Remove a static raster, chunked raster or vector layer and its asset as
    // one command. An asset referenced by any other layer is retained and the
    // command returns Conflict; removeLayer can remove only the layer instead.
    DocumentHistoryResult removeLayerWithAsset(std::string_view id,
        std::string_view label = "Delete layer and asset");
    DocumentHistoryResult undo();
    DocumentHistoryResult redo();
    // Discards both stacks and adopts the owner's current revision without
    // changing the document. A closed/reopened file requires a new instance.
    DocumentHistoryResult reset();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace iiSharedCanvas
