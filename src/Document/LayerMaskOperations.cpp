#include "Document/LayerMaskOperations.h"
#include "Document/LayerMaskValidation_p.hpp"

#include <cmath>
#include <utility>

namespace iiSharedCanvas {
namespace detail {
std::string validateLayerMask(const Document &document, const std::optional<LayerMask> &mask, bool vectorOnly)
{
    if (!mask) return {};
    const auto *asset = findAsset(document, mask->assetId);
    if (!asset) return "mask must reference an existing asset";
    const auto *raster = std::get_if<RasterAsset>(asset);
    const auto *vector = std::get_if<VectorAsset>(asset);
    if (vectorOnly ? !vector : !raster)
        return vectorOnly ? "vector mask requires a vector asset" : "pixel/filter mask requires a raster asset";
    const auto unit = [](double value) { return std::isfinite(value) && value >= 0 && value <= 1; };
    const auto &t = mask->transform;
    const double determinant = t.m11 * t.m22 - t.m12 * t.m21;
    if (!unit(mask->density) || !unit(mask->outsideCoverage)
        || !std::isfinite(mask->featherRadius) || mask->featherRadius < 0 || mask->featherRadius > 128
        || static_cast<unsigned>(mask->channel) > static_cast<unsigned>(MaskChannel::Alpha)
        || !std::isfinite(t.m11) || !std::isfinite(t.m12) || !std::isfinite(t.m21)
        || !std::isfinite(t.m22) || !std::isfinite(t.translationX) || !std::isfinite(t.translationY))
        return "mask parameters must be supported and bounded";
    if (!std::isfinite(determinant) || determinant == 0) return "mask transform must be invertible";
    const auto extent = raster ? CanvasExtent{raster->pixels.width, raster->pixels.height} : vector->viewport;
    if (extent.width <= 0 || extent.height <= 0) return "mask asset extent must be positive";
    return {};
}
} // namespace detail

namespace {
// Only constructs a detached candidate; the document and the shared asset
// are never modified, including on validation failure.
LayerMaskOperationResult prepare(const Document &document, std::string_view id,
                                 std::optional<LayerMask> mask, bool vector)
{
    const auto *layer = findLayer(document, std::string(id));
    if (!layer) return {{}, "layer was not found"};
    if (layerRole(*layer) != LayerRole::Artwork) return {{}, "mask requires an artwork layer"};
    if (auto error = detail::validateLayerMask(document, mask, vector); !error.empty())
        return {{}, std::move(error)};
    auto appearance = layerProperties(*layer).appearance;
    if (vector) appearance.vectorMask = std::move(mask);
    else appearance.rasterMask = std::move(mask);
    return {std::move(appearance), {}};
}
} // namespace

LayerMaskOperationResult LayerMaskOperations::operator()(const Document &document,
    std::string_view layerId, std::optional<LayerMask> mask) const
{
    return prepare(document, layerId, std::move(mask), false);
}

LayerMaskOperationResult VectorMaskOperations::operator()(const Document &document,
    std::string_view layerId, std::optional<LayerMask> mask) const
{
    return prepare(document, layerId, std::move(mask), true);
}
} // namespace iiSharedCanvas
