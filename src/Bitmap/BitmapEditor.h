#pragma once

#include "Bitmap/BitmapEdit.h"
#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"

#include <Core/PaintRect.h>
#include <Stroke/Rasterizer.h>
#include <Stroke/StrokePoint.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace iiSharedCanvas
{

namespace detail
{
class BitmapEditJournal;
struct BitmapHistorySeed;
}

class DocumentFile;

struct BitmapBrush
{
    std::uint32_t argb = 0xff000000U;
    double size = 8.0;
    double opacity = 1.0;
    double flow = 1.0;
    double hardness = 1.0;
    double spacing = 0.0;
    double spacingRatio = 0.15;
    bool flowEnabled = true;
    bool opacityEnabled = true;
    bool hardnessEnabled = true;
    bool spacingEnabled = true;
    bool pressureToOpacityEnabled = true;
    bool eraser = false;
    // Optional native engine dynamics/tip/material; scalar fields above remain authoritative.
    std::optional<BrushState> engineState;
};

class IISHAREDCANVAS_EXPORT BitmapEditor final
{
  public:
    BitmapEditor() = default;
    explicit BitmapEditor(RasterHistoryLimits limits) : m_historyLimits(limits) {}
    BitmapEditor(Document &document, const std::string &assetId, RasterHistoryLimits limits = {});
    BitmapEditor(DocumentFile &file, const std::string &assetId, RasterHistoryLimits limits = {});

    bool bind(Document &document, const std::string &assetId);
    bool bind(DocumentFile &file, const std::string &assetId);
    void unbind() noexcept;

    [[nodiscard]] bool isBound() const noexcept;
    [[nodiscard]] const std::string &boundAssetId() const noexcept;
    [[nodiscard]] const RasterLayer *pixels() const noexcept;
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> pixelAt(int x, int y) const noexcept;

    [[nodiscard]] const BitmapBrush &brush() const noexcept;
    bool setBrush(const BitmapBrush &brush);
    // Snapshot the current stroke's validated state and random seed for an
    // authoring replay. Document-bound active strokes only; file-backed
    // editor transactions do not expose their internal transient editor.
    [[nodiscard]] std::optional<BrushState> currentStrokeBrushState() const;

    bool setPixel(int x, int y, std::uint32_t argb);
    bool clear(std::uint32_t argb = 0x00000000U);
    bool replacePatch(DevicePixelRect bounds, const std::vector<std::uint32_t> &argbPixels);
    bool replacePatches(std::span<const BitmapPatch> patches);
    bool replacePixels(const RasterLayer &pixels);
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

    // Document-bound exclusive editing only. The caller owns whole-stroke
    // undo/redo; a changed completed stroke clears this editor's history.
    // Cancellation restores original pixels and preserves prior history.
    bool setExternalStrokeHistory(bool enabled);
    // Preview only touched tiles; create the normal undo entry once on release.
    // History limits are checked at release; a rejected stroke remains active
    // and can be cancelled. Requires an exclusively owned in-memory document.
    bool setDeferredStrokeHistory(bool enabled);
    bool beginStroke(DocumentPoint point, double pressure = 1.0);
    bool continueStroke(DocumentPoint point, double pressure = 1.0);
    bool endStroke(DocumentPoint point, double pressure = 1.0);
    // Current measured sample only; input sequences are never persisted.
    bool beginStrokeInput(const StrokePoint &sample);
    bool continueStrokeInput(const StrokePoint &sample);
    bool endStrokeInput(const StrokePoint &sample);
    // The caller supplies rasterized samples, never a stored trajectory. The
    // temporary tip is replaced on the next update; finalization promotes
    // exactly the pixels already displayed into the same undo entry.
    bool beginProjectedStroke();
    bool updateProjectedStroke(const std::vector<RasterSample> &stable,
                               const std::vector<RasterSample> &transient);
    bool endProjectedStroke();
    [[nodiscard]] std::optional<std::uint32_t> strokeSourceAt(int x, int y) const;
    // Caller clock in monotonic seconds. Rebase excludes suspended time without
    // emitting pixels or changing the stored document.
    bool advanceStrokeTime(double seconds);
    bool rebaseStrokeTime(double seconds);
    // Exact-type deduction keeps legacy beginStroke({x,y}) calls unambiguous.
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

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    bool undo();
    bool redo();

#ifdef IISHAREDCANVAS_NATIVE_API
    // Atomically publish the most recent local bitmap history entry to a
    // separate file without moving this editor's undo/redo cursor. The file
    // must contain the opposite side and the caller's expected metadata and
    // revision. Only bitmap pixels and the source document's authorship are
    // published; unrelated document structure is never copied.
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
    bool editFile(const std::function<bool(BitmapEditor &)> &edit);
    bool editFileDraft(const std::function<bool(BitmapEditor &)> &edit);
    detail::BitmapEditJournal &journal();
    bool finishEdit(RasterHistoryResult result);

    [[nodiscard]] RasterAsset *rasterAsset() noexcept;
    [[nodiscard]] const RasterAsset *rasterAsset() const noexcept;
    [[nodiscard]] RasterLayer *mutablePixels() noexcept;
    [[nodiscard]] bool requireBound();
    [[nodiscard]] bool validPoint(DocumentPoint point, double pressure);
    [[nodiscard]] bool validInput(const StrokePoint &sample);
    [[nodiscard]] BrushState brushState() const;
    [[nodiscard]] DevicePixelRect bitmapBounds() const noexcept;
    void noteChange(DevicePixelRect bounds);
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
