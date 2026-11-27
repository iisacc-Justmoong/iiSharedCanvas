#include "Render/FrameComposite.h"

#include <cstdint>
#include <new>
#include <string>
#include <unordered_set>

namespace iiSharedCanvas {
namespace {

// Validate all masks and the complete output allocation before invoking the
// compositor, so invalid requests never expose a partially composed result.
FrameTileRenderResult validate(const FrameLayerBatchRenderResult &batch,
    std::span<const RasterMask> masks, std::size_t maximumPixels,
    std::size_t copies)
{
    if (!batch.ok()) return {{}, batch.status, batch.message};
    if (!masks.empty() && masks.size() != batch.requests.size())
        return {{}, FrameRenderStatus::InvalidRegion, "selection must have one mask per requested tile"};
    std::size_t total = 0;
    for (std::size_t i = 0; i < batch.requests.size(); ++i) {
        const auto extent = batch.requests[i].outputExtent;
        if (extent.width <= 0 || extent.height <= 0)
            return {{}, FrameRenderStatus::InvalidRegion, "output extent must be positive"};
        const auto width = static_cast<std::size_t>(extent.width);
        const auto height = static_cast<std::size_t>(extent.height);
        if (height > maximumPixels / width)
            return {{}, FrameRenderStatus::InvalidRegion, "composition exceeds its pixel budget"};
        const auto pixels = width * height;
        if (copies == 0 || pixels > (maximumPixels - total) / copies)
            return {{}, FrameRenderStatus::InvalidRegion, "composition exceeds its pixel budget"};
        total += pixels * copies;
        if (!masks.empty() && (masks[i].extent != extent || masks[i].alpha.size() != pixels))
            return {{}, FrameRenderStatus::InvalidRegion, "selection mask must match its output tile"};
    }
    return {};
}

// Coverage multiplies alpha in straight-alpha ARGB; retaining RGB for a
// nonzero result avoids unintentionally premultiplying the exported image.
void applyMasks(FrameTileRenderResult &result, std::span<const RasterMask> masks)
{
    if (!result.ok() || masks.empty()) return;
    for (std::size_t tile = 0; tile < result.tiles.size(); ++tile) {
        auto &pixels = result.tiles[tile].pixels.pixels;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const auto alpha = ((pixels[i] >> 24) * masks[tile].alpha[i] + 127U) / 255U;
            pixels[i] = alpha ? (alpha << 24) | (pixels[i] & 0x00ffffffU) : 0;
        }
    }
}

} // namespace

FrameTileRenderResult SelectedFrameComposite::operator()(const FrameLayerBatchRenderResult &batch,
    std::span<const std::string_view> selectedLayerIds, std::span<const RasterMask> selection,
    std::size_t maximumPixels) const
{
    try {
        if (selectedLayerIds.empty())
            return {{}, FrameRenderStatus::LayerOutOfRange, "at least one layer must be selected"};
        std::unordered_set<std::string_view> selected;
        for (const auto id : selectedLayerIds) {
            if (id.empty() || !selected.insert(id).second)
                return {{}, FrameRenderStatus::InvalidDocument, "selected layer identifiers must be nonempty and unique"};
        }
        if (selected.size() == static_cast<std::size_t>(-1))
            return {{}, FrameRenderStatus::InvalidRegion, "composition exceeds its pixel budget"};
        auto valid = validate(batch, selection, maximumPixels, selected.size() + 1);
        if (!valid.ok()) return valid;

        std::unordered_set<std::string_view> available;
        for (std::size_t i = 0; i < batch.layers.size(); ++i) {
            const auto &layer = batch.layers[i];
            if (layer.layerId.empty() || !available.insert(layer.layerId).second || layer.layerIndex != i)
                return {{}, FrameRenderStatus::InvalidDocument, "layer batch identifiers or order are invalid"};
            if (selected.contains(layer.layerId) && layer.role != LayerRole::Artwork)
                return {{}, FrameRenderStatus::InvalidDocument, "only artwork layers can be composited"};
        }
        for (const auto id : selected)
            if (!available.contains(id))
                return {{}, FrameRenderStatus::LayerOutOfRange, "selected layer is absent from the batch"};

        for (std::size_t i = 0; i < batch.layers.size(); ++i) {
            const auto &layer = batch.layers[i];
            if (!selected.contains(layer.layerId) || !layer.clippedToBelow) continue;
            const auto group = batch.groups.groupOf(layer.layerId);
            const FrameLayerTileRenderResult *base = nullptr;
            for (std::size_t j = i; j > 0; --j) {
                const auto &candidate = batch.layers[j - 1];
                if (candidate.role == LayerRole::Artwork && candidate.artboardId == layer.artboardId &&
                    batch.groups.groupOf(candidate.layerId) == group && !candidate.clippedToBelow) {
                    base = &candidate;
                    break;
                }
            }
            if (!base || !selected.contains(base->layerId))
                return {{}, FrameRenderStatus::InvalidDocument, "selected clipped layer requires its original base"};
        }

        FrameLayerBatchRenderResult filtered;
        filtered.requests = batch.requests;
        filtered.artboards = batch.artboards;
        filtered.groups = batch.groups;
        for (auto &board : filtered.artboards) board.backgroundArgb = 0;
        filtered.layers.reserve(selected.size());
        for (const auto &layer : batch.layers) {
            if (!selected.contains(layer.layerId)) continue;
            filtered.layers.push_back(layer);
            filtered.layers.back().layerIndex = filtered.layers.size() - 1;
        }
        auto result = composeFrameLayers(filtered);
        applyMasks(result, selection);
        return result;
    } catch (const std::bad_alloc &) {
        return {{}, FrameRenderStatus::AssetResolutionFailed, "insufficient memory for selected composition"};
    }
}

FrameTileRenderResult MergedFrameComposite::operator()(const FrameLayerBatchRenderResult &batch,
    std::span<const RasterMask> selection, std::size_t maximumPixels) const
{
    try {
        auto valid = validate(batch, selection, maximumPixels, 1);
        if (!valid.ok()) return valid;
        auto result = composeFrameLayers(batch);
        applyMasks(result, selection);
        return result;
    } catch (const std::bad_alloc &) {
        return {{}, FrameRenderStatus::AssetResolutionFailed, "insufficient memory for merged composition"};
    }
}

} // namespace iiSharedCanvas
