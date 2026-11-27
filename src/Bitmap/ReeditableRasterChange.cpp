#include "Bitmap/ReeditableRasterChange.h"
#include <limits>

namespace iiSharedCanvas {
// Reserve the logical three-buffer budget before copying any source pixels.
ReeditableRasterChange::ReeditableRasterChange(DocumentHistory &history, std::string assetId, std::size_t maximumBytes)
    : history_(&history), assetId_(std::move(assetId))
{
    const auto *document = history.document();
    const auto *asset = document ? findRasterAsset(*document, assetId_) : nullptr;
    if (!asset || asset->pixels.pixels.size() > maximumBytes / (3 * sizeof(std::uint32_t))) return;
    original_ = asset->pixels; expected_ = asset->pixels;
    revision_ = document->authorship.revision(); ready_ = true;
}

// Direct pixel mutations can bypass authorship, so compare both revision and pixels.
bool ReeditableRasterChange::current() const noexcept
{
    const auto *document = history_->document();
    const auto *asset = document ? findRasterAsset(*document, assetId_) : nullptr;
    return ready_ && asset && document->authorship.revision() == revision_ &&
        asset->pixels.width == expected_.width && asset->pixels.height == expected_.height &&
        asset->pixels.pixels == expected_.pixels;
}

DocumentHistoryResult ReeditableRasterChange::apply(const ColorAdjustment &settings)
{
    if (!ready_) return {DocumentHistoryCode::InvalidArgument};
    if (!current()) return {DocumentHistoryCode::Conflict};
    try {
        auto result = publish(adjustRaster(original_, settings));
        if (result.ok()) settings_ = settings;
        return result;
    }
    catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
    catch (...) { return {DocumentHistoryCode::EditRejected}; }
}
DocumentHistoryResult ReeditableRasterChange::apply(const RasterEffect &settings)
{
    if (!ready_) return {DocumentHistoryCode::InvalidArgument};
    if (!current()) return {DocumentHistoryCode::Conflict};
    try {
        auto result = publish(effectRaster(original_, settings));
        if (result.ok()) settings_ = settings;
        return result;
    }
    catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
    catch (...) { return {DocumentHistoryCode::EditRejected}; }
}
// Keep the previous expectation intact on processing, history or persistence failure.
DocumentHistoryResult ReeditableRasterChange::publish(RasterProcessResult processed)
{
    if (!processed.ok()) return {DocumentHistoryCode::InvalidArgument};
    const BitmapPatch patch{{{0, 0}, processed.pixels.width, processed.pixels.height}, processed.pixels.pixels};
    auto result = history_->replaceBitmapPatches(assetId_, std::span{&patch, 1}, "Reedit raster");
    if (result.ok()) {
        expected_ = std::move(processed.pixels);
        revision_ = history_->document()->authorship.revision();
    }
    return result;
}
} // namespace iiSharedCanvas
