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

    bool beginStroke(DocumentPoint point, double pressure = 1.0);
    bool continueStroke(DocumentPoint point, double pressure = 1.0);
    bool endStroke(DocumentPoint point, double pressure = 1.0);
    bool beginStrokeInput(const StrokePoint &sample);
    bool continueStrokeInput(const StrokePoint &sample);
    bool endStrokeInput(const StrokePoint &sample);
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

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    bool undo();
    bool redo();
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
    bool appendStrokePoint(const StrokePoint &sample, bool finish);
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
    bool m_strokeChanged = false;
    std::string m_lastError;
};

} // namespace iiSharedCanvas
