#include "Render/LayerRasterizationPass.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace iiSharedCanvas {
FrameRenderResult LayerRasterizationPass::operator()(const FrameLayerTileRenderResult &layer,
    const FrameRenderTileRequest &request, bool groupVisible) const
{
    const auto origin=request.region.origin;
    if (!layer.ok()) return {origin,{},layer.status,layer.message};
    const auto width=request.outputExtent.width, height=request.outputExtent.height;
    if (request.region.extent.width<=0 || request.region.extent.height<=0 || width<=0 || height<=0 ||
        std::uint64_t(width)*std::uint64_t(height)>std::numeric_limits<std::size_t>::max()/sizeof(std::uint32_t) ||
        (request.sampling!=RasterSampling::Nearest && request.sampling!=RasterSampling::Smooth))
        return {origin,{},FrameRenderStatus::InvalidRegion,"invalid rasterization request"};
    if (!std::isfinite(layer.opacity) || layer.opacity<0 || layer.opacity>1 ||
        !std::isfinite(layer.fillOpacity) || layer.fillOpacity<0 || layer.fillOpacity>1)
        return {origin,{},FrameRenderStatus::InvalidDocument,"invalid layer opacity"};
    if (layer.role!=LayerRole::Artwork || !layer.spatial)
        return {origin,{},FrameRenderStatus::AssetResolutionFailed,"layer has no artwork raster"};
    if (layer.clippedToBelow || layer.blendIf || layer.adjustment)
        return {origin,{},FrameRenderStatus::AssetResolutionFailed,"layer requires its backdrop"};
    if (layer.blendMode==RasterBlendMode::DestinationOut ||
        static_cast<unsigned>(layer.blendMode)>=static_cast<unsigned>(RasterBlendMode::Dissolve))
        return {origin,{},FrameRenderStatus::AssetResolutionFailed,"layer blend mode cannot be rasterized independently"};
    if (!layer.visible || !groupVisible)
        return {origin,makeRasterLayer(width,height,0),FrameRenderStatus::Success,{}};
    if (layer.tiles.size()!=1 || layer.tiles[0].region!=request.region ||
        layer.tiles[0].pixels.width!=width || layer.tiles[0].pixels.height!=height ||
        layer.tiles[0].pixels.pixels.size()!=std::uint64_t(width)*std::uint64_t(height))
        return {origin,{},FrameRenderStatus::InvalidRegion,"rendered layer tile does not match the request"};
    auto pixels=layer.tiles[0].pixels;
    const double opacity=layer.opacity*layer.fillOpacity;
    for (auto &pixel:pixels.pixels) {
        const auto alpha=static_cast<std::uint32_t>(std::lround((pixel>>24)*opacity));
        pixel=alpha ? (alpha<<24)|(pixel&0x00ffffffU) : 0;
    }
    return {origin,std::move(pixels),FrameRenderStatus::Success,{}};
}
} // namespace iiSharedCanvas
