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
#include <vector>

namespace iiSharedCanvas {

class IISHAREDCANVAS_EXPORT ChunkedBitmapEditor final {
public:
    ChunkedBitmapEditor() = default;
    ChunkedBitmapEditor(Document &document, const std::string &assetId);
    ChunkedBitmapEditor(DocumentFile &file, const std::string &assetId);

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

    bool beginStroke(DocumentPoint point, double pressure = 1.0);
    bool continueStroke(DocumentPoint point, double pressure = 1.0);
    bool endStroke(DocumentPoint point, double pressure = 1.0);
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
    bool editFile(const std::function<bool(ChunkedBitmapEditor &)> &edit);
    static constexpr std::size_t HistoryLimit = 32;

    [[nodiscard]] ChunkedRasterAsset *asset() noexcept;
    [[nodiscard]] const ChunkedRasterAsset *asset() const noexcept;
    [[nodiscard]] bool requireBound();
    [[nodiscard]] bool validPoint(DocumentPoint point, double pressure);
    [[nodiscard]] BrushState brushState() const;
    [[nodiscard]] bool contains(std::int32_t x, std::int32_t y) const noexcept;
    RasterChunk &ensureChunk(std::int32_t column, std::int32_t row);
    void recordSnapshot(bool clearRedo = true);
    void noteChange(DevicePixelRect bounds);
    bool appendStrokePoint(DocumentPoint point, double pressure, bool finish);
    void setError(std::string message);
    void clearError() noexcept;

    Document *m_document = nullptr;
    DocumentFile *m_file = nullptr;
    std::uint64_t m_fileGeneration = 0;
    std::string m_assetId;
    BitmapBrush m_brush;
    RasterDabStream m_dabStream;
    std::vector<std::shared_ptr<const std::vector<RasterChunk>>> m_undoHistory;
    std::vector<std::shared_ptr<const std::vector<RasterChunk>>> m_redoHistory;
    DevicePixelRect m_dirtyBounds{};
    std::uint64_t m_revision = 0;
    std::uint32_t m_nextStrokeSeed = 1;
    double m_strokeTime = 0.0;
    bool m_strokeActive = false;
    bool m_strokeChanged = false;
    std::string m_lastError;
};

} // namespace iiSharedCanvas
