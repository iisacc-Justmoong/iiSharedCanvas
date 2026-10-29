#pragma once
#include "Bitmap/BitmapEdit.h"
#include "Bitmap/BitmapCommit_p.hpp"
#include "Bitmap/PixelPreview_p.hpp"
#include "Bitmap/StrokeComposite_p.hpp"
#include "Document/Document.h"
#include <map>
#include <optional>
#include <string>

namespace iiSharedCanvas::detail
{
// Per-binding COW cursor. The journal owns pixels and chunk-presence deltas,
// never input points or dabs. Editors detach this state before mutation.
class BitmapEditJournal final
{
    friend struct BitmapEditSource;
    using Coordinate = std::pair<std::int32_t, std::int32_t>;
    struct Geometry
    {
        bool chunked = false;
        std::int32_t x = 0, y = 0, width = 0, height = 0, chunkSize = 0;
        bool operator==(const Geometry &) const = default;
    };
    RasterEditHistory history_;
    std::optional<RasterEditHistory> previewBase_;
    std::shared_ptr<BitmapStrokeComposite> stroke_;
    std::shared_ptr<BitmapPixelPreview> pixelPreview_;
    std::string previewLabel_;
    std::map<Coordinate, std::uint8_t> originalChunks_;
    std::optional<Geometry> geometry_;
    bool growStrokeCanvas_ = false;
    std::optional<CanvasRegion> strokeCanvas_;
    bool stagedPixels_ = false, growStagedPixels_ = false;
    std::optional<CanvasRegion> pixelCanvas_, stagedRegion_;
    DevicePixelRect stagedClip_{};
    bool previewChanged_ = false;
    RasterHistoryResult previewAdmission_;
    BitmapCommitPort *commitPort_ = nullptr;
    bool checkGeometry(Document &, std::string_view);
    void updateGeometry(Document &, std::string_view);
    RasterHistoryResult apply(Document &, std::string_view, std::span<const BitmapPatch>,
                              std::optional<CanvasExtent>, std::map<Coordinate, std::uint8_t>,
                              std::string_view, const BitmapStrokeComposite *original = nullptr,
                              const BitmapPixelPreview *replacement = nullptr,
                              std::optional<CanvasRegion> canvas = {});

  public:
    explicit BitmapEditJournal(RasterHistoryLimits limits) : history_(limits) {}
    void setCommitPort(BitmapCommitPort *port) noexcept { commitPort_ = port; }
    void setStrokeCanvasGrowth(bool grow) noexcept { growStrokeCanvas_ = grow; }
    [[nodiscard]] bool strokeCanvasGrowth() const noexcept { return strokeCanvas_.has_value(); }
    void setStagedPixels(bool grow) noexcept { stagedPixels_ = true; growStagedPixels_ = grow; }
    [[nodiscard]] bool stagedPixels() const noexcept { return stagedPixels_ && bool(pixelPreview_); }
    [[nodiscard]] bool stagedPixelGrowth() const noexcept { return stagedPixels() && growStagedPixels_; }
    [[nodiscard]] std::vector<BitmapPreviewTile> stagedPixelTiles() const;
    [[nodiscard]] std::optional<CanvasRegion> stagedPixelRegion() const noexcept;
    DevicePixelRect discardStagedPixels() noexcept;
    DevicePixelRect dirty{};
    RasterHistoryResult result{};
    [[nodiscard]] RasterHistoryStatistics statistics() const noexcept
    {
        return history_.statistics();
    }
    [[nodiscard]] bool canUndo() const noexcept { return history_.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return history_.canRedo(); }
    [[nodiscard]] std::size_t previewBytes() const noexcept;
    [[nodiscard]] bool strokeChanged() const noexcept { return previewChanged_; }
    [[nodiscard]] RasterHistoryResult previewAdmission() const noexcept
    {
        return previewAdmission_;
    }
    [[nodiscard]] bool pixelPreviewActive() const noexcept { return bool(pixelPreview_); }
    RasterHistoryResult beginPixels(Document &, std::string_view, std::string_view label);
    RasterHistoryResult replacePixelPreview(Document &, std::string_view,
                                            std::span<const BitmapPatch>);
    RasterHistoryResult commitPixels(Document &, std::string_view);
    RasterHistoryResult replace(Document &, std::string_view, std::span<const BitmapPatch>);
    RasterHistoryResult replacePixels(Document &, std::string_view, const RasterLayer &);
    RasterHistoryResult replaceRegion(Document &, std::string_view, CanvasOrigin,
                                      const RasterLayer &);
    RasterHistoryResult clear(Document &, std::string_view, std::uint32_t = 0);
    RasterHistoryResult step(Document &, std::string_view, bool redo);
    void begin(Document *document = nullptr);
    void finish() noexcept;
    RasterHistoryResult cancel(Document &, std::string_view);
    RasterHistoryResult preview(Document &, std::string_view, const std::vector<RasterSample> &,
                                RasterBlendMode);
    [[nodiscard]] std::uint32_t sourcePixel(Document &, std::string_view, DevicePixelPoint) const;
};
struct BitmapEditSource
{
    CanvasOrigin origin;
    const RasterLayer *raster = nullptr;
    const ChunkedRasterAsset *chunks = nullptr;
    const BitmapStrokeComposite *original = nullptr;
    int chunkSize = 0;
    BitmapEditSource(const BitmapEditJournal &, Document &, std::string_view, CanvasOrigin);
    [[nodiscard]] std::uint32_t sample(DevicePixelPoint local) const;
};
inline std::uint32_t sampleBitmapEditSource(const void *context, DevicePixelPoint local)
{
    return static_cast<const BitmapEditSource *>(context)->sample(local);
}
} // namespace iiSharedCanvas::detail
