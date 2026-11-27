#pragma once
#include "Bitmap/BitmapProcessing.h"
#include "Document/DocumentHistory.h"

namespace iiSharedCanvas {
// Owns the starting pixels and the last committed output for conflict checks.
// The history and its document/file must outlive this single-threaded session.
class IISHAREDCANVAS_EXPORT ReeditableRasterChange final {
public:
    // maximumBytes bounds three full pixel buffers: original, last result and
    // next result. Processing internals and history have their own budgets.
    ReeditableRasterChange(DocumentHistory &, std::string assetId, std::size_t maximumBytes);
    // Report whether an existing finite raster fit the caller's buffer budget.
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    // The last successfully accepted settings; empty until the first apply.
    [[nodiscard]] const std::optional<std::variant<ColorAdjustment, RasterEffect>> &settings() const noexcept { return settings_; }
    // Apply a complete color adjustment from the captured original. Publication
    // is one history entry and rejects stale owners without replacing pixels.
    DocumentHistoryResult apply(const ColorAdjustment &);
    // Apply an effect from the same original, with the same atomicity contract.
    DocumentHistoryResult apply(const RasterEffect &);
private:
    // Publish only a successfully processed raster through the bound history.
    DocumentHistoryResult publish(RasterProcessResult);
    // Verify both the owner's revision and the last accepted pixel contents.
    [[nodiscard]] bool current() const noexcept;
    DocumentHistory *history_;
    std::string assetId_;
    RasterLayer original_, expected_;
    std::uint64_t revision_ = 0;
    bool ready_ = false;
    std::optional<std::variant<ColorAdjustment, RasterEffect>> settings_;
};
} // namespace iiSharedCanvas
