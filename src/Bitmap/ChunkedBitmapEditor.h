#pragma once

#include "Bitmap/BitmapEditor.h"
#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"

#include <Core/PaintRect.h>
#include <Stroke/Rasterizer.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace iiSharedCanvas
{

class IISHAREDCANVAS_EXPORT ChunkedBitmapEditor final
{
  public:
    ChunkedBitmapEditor() = default;
    explicit ChunkedBitmapEditor(RasterHistoryLimits limits) : m_historyLimits(limits) {}
    ChunkedBitmapEditor(Document &document, const std::string &assetId,
                        RasterHistoryLimits limits = {});
    ChunkedBitmapEditor(DocumentFile &file, const std::string &assetId,
                        RasterHistoryLimits limits = {});

    bool bind(Document &document, const std::string &assetId);
    bool bind(DocumentFile &file, const std::string &assetId);
    void unbind() noexcept;
    [[nodiscard]] bool isBound() const noexcept;
    [[nodiscard]] const std::string &boundAssetId() const noexcept;

    [[nodiscard]] const BitmapBrush &brush() const noexcept;
    bool setBrush(const BitmapBrush &brush);
    [[nodiscard]] std::optional<std::uint32_t> pixelAt(std::int32_t x,
                                                       std::int32_t y) const noexcept;
    bool clear();
    bool replaceRegion(CanvasOrigin origin, const RasterLayer &pixels);
    bool replacePatches(std::span<const BitmapPatch> patches);
    // Asset-local coordinates may lie outside the canvas region after a layer
    // transform. These methods do not grow or otherwise change the canvas.
    // Samples and patch bounds must be within [-1,000,000,000, 1,000,000,000).
    [[nodiscard]] std::optional<std::uint32_t> assetPixelAt(std::int32_t x,
                                                            std::int32_t y) const noexcept;
    bool replaceAssetPatches(std::span<const BitmapPatch> patches);
    [[nodiscard]] RasterHistoryStatistics historyStatistics() const noexcept;
    [[nodiscard]] const RasterHistoryLimits &historyLimits() const noexcept
    {
        return m_historyLimits;
    }
    [[nodiscard]] RasterHistoryResult lastHistoryResult() const noexcept { return m_historyResult; }
    [[nodiscard]] std::size_t previewStorageBytes() const noexcept;
    // A complete final-pixel preview replaces its previous footprint. Resolve
    // explicitly before changing a live binding or destroying the editor.
    bool beginPixelPreview(std::string_view label = "Pixel edit");
    bool replacePixelPreview(std::span<const BitmapPatch> patches);
    bool commitPixelPreview();
    bool cancelPixelPreview();
    [[nodiscard]] bool pixelPreviewActive() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> pixelPreviewSourceAt(int x, int y) const noexcept;

    // As for BitmapEditor: document-bound exclusive editing, with whole-stroke
    // undo/redo owned by the caller. Canvas growth is not supported in this mode.
    bool setExternalStrokeHistory(bool enabled);
    // Create the normal undo entry on release instead of on each move.
    // A rejected final history entry leaves the stroke active for cancellation.
    bool setDeferredStrokeHistory(bool enabled);
    bool beginStroke(DocumentPoint point, double pressure = 1.0);
    bool continueStroke(DocumentPoint point, double pressure = 1.0);
    bool endStroke(DocumentPoint point, double pressure = 1.0);
    bool beginStrokeInput(const StrokePoint &sample);
    // Strokes clip by default on both finite and infinite canvases. Growth can
    // only be enabled for an infinite canvas and joins changed outside pixels
    // and canvas geometry into the same undoable edit.
    // Asset-local strokes always clip to their explicit rectangle.
    bool setStrokeCanvasGrowth(bool enabled);
    // Streams a stroke in the specified asset-local rectangle without growing
    // the canvas. The caller maps document coordinates and clips transforms.
    bool beginAssetStrokeInput(const StrokePoint &sample, CanvasRegion assetRegion);
    bool continueStrokeInput(const StrokePoint &sample);
    bool endStrokeInput(const StrokePoint &sample);
    // Projected samples use the same incremental journal and one-entry
    // history as ordinary strokes; no input paths are owned here.
    bool beginProjectedStroke(std::optional<CanvasRegion> assetRegion = std::nullopt);
    bool updateProjectedStroke(const std::vector<RasterSample> &stable,
                               const std::vector<RasterSample> &transient);
    bool endProjectedStroke();
    [[nodiscard]] std::optional<std::uint32_t> strokeSourceAt(std::int32_t x, std::int32_t y) const;
    bool advanceStrokeTime(double seconds);
    bool rebaseStrokeTime(double seconds);
    template <class Sample>
        requires std::same_as<std::remove_cvref_t<Sample>, StrokePoint>
    bool beginStroke(Sample &&sample)
    {
        return beginStrokeInput(sample);
    }
    template <class Sample>
        requires std::same_as<std::remove_cvref_t<Sample>, StrokePoint>
    bool continueStroke(Sample &&sample)
    {
        return continueStrokeInput(sample);
    }
    template <class Sample>
        requires std::same_as<std::remove_cvref_t<Sample>, StrokePoint>
    bool endStroke(Sample &&sample)
    {
        return endStrokeInput(sample);
    }
    void cancelStroke();
    [[nodiscard]] bool strokeActive() const noexcept;
    // Effective settings for replay of a document-bound active stroke.
    [[nodiscard]] std::optional<BrushState> currentStrokeBrushState() const;

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    bool undo();
    bool redo();

#ifdef IISHAREDCANVAS_NATIVE_API
    // As for BitmapEditor, including sparse chunk-presence and canvas deltas.
    bool publishHistoryState(DocumentFile &file, BitmapHistoryState state,
        const DocumentAuthorship &expectedAuthorship, FormatVersion expectedVersion,
        std::uint64_t expectedFileRevision);
#endif
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] DevicePixelRect dirtyBounds() const noexcept;
    void clearDirtyBounds() noexcept;
    [[nodiscard]] const std::string &lastError() const noexcept;

  private:
    friend struct detail::BitmapHistorySeed;
    bool editFile(const std::function<bool(ChunkedBitmapEditor &)> &edit);
    bool editFileDraft(const std::function<bool(ChunkedBitmapEditor &)> &edit);
    detail::BitmapEditJournal &journal();
    bool finishEdit(RasterHistoryResult result);

    [[nodiscard]] ChunkedRasterAsset *asset() noexcept;
    [[nodiscard]] const ChunkedRasterAsset *asset() const noexcept;
    [[nodiscard]] bool requireBound();
    [[nodiscard]] bool validPoint(DocumentPoint point, double pressure);
    [[nodiscard]] bool validInput(const StrokePoint &sample);
    [[nodiscard]] BrushState brushState() const;
    [[nodiscard]] bool contains(std::int32_t x, std::int32_t y) const noexcept;
    void noteChange(DevicePixelRect bounds);
    bool beginStrokeInRegion(const StrokePoint &sample, std::optional<CanvasRegion> assetRegion);
    bool appendStrokePoint(const StrokePoint &sample, bool finish);
    bool configureStrokeHistory(bool enabled, bool deferred);
    void setError(std::string message);
    void clearError() noexcept;

    Document *m_document = nullptr;
    DocumentFile *m_file = nullptr;
    std::uint64_t m_fileGeneration = 0;
    std::string m_assetId;
    BitmapBrush m_brush;
    RasterDabStream m_dabStream;
    RasterHistoryLimits m_historyLimits;
    RasterHistoryResult m_historyResult;
    std::shared_ptr<detail::BitmapEditJournal> m_journal;
    DevicePixelRect m_dirtyBounds{};
    std::uint64_t m_revision = 0;
    std::uint32_t m_nextStrokeSeed = 1;
    double m_strokeTime = 0.0;
    bool m_strokeActive = false;
    bool m_projectedStroke = false;
    bool m_strokeChanged = false;
    std::string m_lastError;
};

} // namespace iiSharedCanvas
