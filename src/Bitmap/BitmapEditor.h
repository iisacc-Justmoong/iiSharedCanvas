#pragma once

#include "Document/Document.h"
#include "Export.h"

#include <Core/PaintRect.h>
#include <Stroke/Rasterizer.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iiSharedCanvas {

struct BitmapBrush {
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
};

class IISHAREDCANVAS_EXPORT BitmapEditor final {
public:
    BitmapEditor() = default;
    BitmapEditor(Document &document, const std::string &assetId);

    bool bind(Document &document, const std::string &assetId);
    void unbind() noexcept;

    [[nodiscard]] bool isBound() const noexcept;
    [[nodiscard]] const std::string &boundAssetId() const noexcept;
    [[nodiscard]] const RasterLayer *pixels() const noexcept;
    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> pixelAt(int x, int y) const noexcept;

    [[nodiscard]] const BitmapBrush &brush() const noexcept;
    bool setBrush(const BitmapBrush &brush);

    bool setPixel(int x, int y, std::uint32_t argb);
    bool clear(std::uint32_t argb = 0x00000000U);
    bool replacePatch(DevicePixelRect bounds,
                      const std::vector<std::uint32_t> &argbPixels);
    bool replacePixels(const RasterLayer &pixels);

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
    static constexpr std::size_t HistoryLimit = 32;

    [[nodiscard]] RasterAsset *rasterAsset() noexcept;
    [[nodiscard]] const RasterAsset *rasterAsset() const noexcept;
    [[nodiscard]] RasterLayer *mutablePixels() noexcept;
    [[nodiscard]] bool requireBound();
    [[nodiscard]] bool validPoint(DocumentPoint point, double pressure);
    [[nodiscard]] BrushState brushState() const;
    [[nodiscard]] DevicePixelRect bitmapBounds() const noexcept;
    void recordSnapshot(bool clearRedo = true);
    void noteChange(DevicePixelRect bounds);
    bool appendStrokePoint(DocumentPoint point, double pressure, bool finish);
    void setError(std::string message);
    void clearError() noexcept;

    Document *m_document = nullptr;
    std::string m_assetId;
    BitmapBrush m_brush;
    RasterDabStream m_dabStream;
    std::vector<RasterLayer> m_undoHistory;
    std::vector<RasterLayer> m_redoHistory;
    DevicePixelRect m_dirtyBounds{};
    std::uint64_t m_revision = 0;
    std::uint32_t m_nextStrokeSeed = 1;
    double m_strokeTime = 0.0;
    bool m_strokeActive = false;
    bool m_strokeChanged = false;
    std::string m_lastError;
};

} // namespace iiSharedCanvas
