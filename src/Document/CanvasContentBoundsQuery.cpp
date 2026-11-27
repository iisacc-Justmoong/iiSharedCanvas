#include "Document/CanvasContentBoundsQuery.h"
#include "Document/TransparentBoundsScanner_p.hpp"
#include "Validation/Validation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <unordered_set>

namespace iiSharedCanvas {
namespace {
bool unite(std::optional<CanvasRegion> &result,CanvasRegion next)
{
    if (!result) { result=next; return true; }
    const auto left=std::min<std::int64_t>(result->origin.x,next.origin.x);
    const auto top=std::min<std::int64_t>(result->origin.y,next.origin.y);
    const auto right=std::max(std::int64_t(result->origin.x)+result->extent.width,
                              std::int64_t(next.origin.x)+next.extent.width);
    const auto bottom=std::max(std::int64_t(result->origin.y)+result->extent.height,
                               std::int64_t(next.origin.y)+next.extent.height);
    if (right-left>std::numeric_limits<std::int32_t>::max() ||
        bottom-top>std::numeric_limits<std::int32_t>::max()) return false;
    result=CanvasRegion{{static_cast<std::int32_t>(left),static_cast<std::int32_t>(top)},
                        {static_cast<std::int32_t>(right-left),static_cast<std::int32_t>(bottom-top)}};
    return true;
}
std::optional<CanvasRegion> intersection(CanvasRegion a,CanvasRegion b)
{
    const auto left=std::max<std::int64_t>(a.origin.x,b.origin.x);
    const auto top=std::max<std::int64_t>(a.origin.y,b.origin.y);
    const auto right=std::min(std::int64_t(a.origin.x)+a.extent.width,std::int64_t(b.origin.x)+b.extent.width);
    const auto bottom=std::min(std::int64_t(a.origin.y)+a.extent.height,std::int64_t(b.origin.y)+b.extent.height);
    if (right<=left || bottom<=top) return std::nullopt;
    return CanvasRegion{{static_cast<std::int32_t>(left),static_cast<std::int32_t>(top)},
                        {static_cast<std::int32_t>(right-left),static_cast<std::int32_t>(bottom-top)}};
}
}
CanvasBoundsResult CanvasContentBoundsQuery::operator()(std::span<const FrameRenderResult> samples,
    std::uint8_t threshold,std::size_t maximumPixels) const
{
    CanvasBoundsResult result;
    std::size_t used=0;
    for (const auto &sample:samples) {
        if (!sample.ok()) return {{},sample.message.empty() ? "render sample failed" : sample.message};
        const auto width=sample.pixels.width,height=sample.pixels.height;
        if (width<=0 || height<=0) return {{},"sample dimensions must be positive"};
        const auto pixels=std::uint64_t(width)*std::uint64_t(height);
        if (used>maximumPixels || pixels>maximumPixels-used) return {{},"content scan exceeds the work budget"};
        used+=static_cast<std::size_t>(pixels);
        auto bounds=detail::scanTransparentBounds(sample.pixels,sample.origin,threshold,maximumPixels);
        if (!bounds.ok()) return bounds;
        if (bounds.bounds && !unite(result.bounds,*bounds.bounds)) return {{},"content bounds exceed the coordinate range"};
    }
    return result;
}
CanvasBoundsResult queryCanvasContentBounds(const Document &document,const CanvasContentBoundsRequest &request)
{
    if (!validate(document).ok() || static_cast<unsigned>(request.scope)>static_cast<unsigned>(CanvasContentBoundsScope::AllLayers))
        return {{},"invalid document or bounds scope"};
    if (request.scope==CanvasContentBoundsScope::AllLayers && !request.region)
        return {{},"all-layer bounds require an explicit search region"};
    const auto region=request.region.value_or(documentViewRegion(document));
    if (region.extent.width<=0 || region.extent.height<=0 ||
        std::int64_t(region.origin.x)+region.extent.width>std::numeric_limits<std::int32_t>::max() ||
        std::int64_t(region.origin.y)+region.extent.height>std::numeric_limits<std::int32_t>::max())
        return {{},"invalid search region"};
    const auto pixels=std::uint64_t(region.extent.width)*std::uint64_t(region.extent.height);
    if (pixels>request.maximumPixels) return {{},"content search exceeds the work budget"};
    const std::uint64_t frameCount=request.frames.empty() ? document.timeline.frameCount : request.frames.size();
    std::uint64_t layerCount=1,boards=0;
    if (request.scope==CanvasContentBoundsScope::AllLayers) {
        layerCount=std::count_if(document.layers.begin(),document.layers.end(),
            [](const Layer &layer) { return layerRole(layer)==LayerRole::Artwork; });
        boards=std::count_if(document.artboards.begin(),document.artboards.end(),
            [&](const Artboard &board) { return board.geometry && (board.backgroundArgb>>24)>request.threshold; });
    }
    const auto workLimit=request.maximumPixels/pixels;
    if (boards>workLimit || (layerCount && frameCount>(workLimit-boards)/layerCount))
        return {{},"content search exceeds the work budget"};
    std::vector<FrameIndex> frames=request.frames;
    if (frames.empty()) {
        if (layerCount) {
            frames.reserve(document.timeline.frameCount);
            for (FrameIndex i=0;i<document.timeline.frameCount;++i) frames.push_back(i);
        }
    } else {
        std::unordered_set<FrameIndex> seen;
        for (auto frame:frames) if (frame>=document.timeline.frameCount || !seen.insert(frame).second)
            return {{},"frames must be distinct and within the timeline"};
    }
    CanvasBoundsResult result; std::size_t used=0;
    const auto consume=[&](FrameRenderResult sample)->bool {
        if (used>request.maximumPixels || pixels>request.maximumPixels-used) {
            result.error="content search exceeds the work budget"; return false;
        }
        used+=static_cast<std::size_t>(pixels);
        const std::array<FrameRenderResult,1> samples{std::move(sample)};
        auto bounds=CanvasContentBoundsQuery{}(samples,request.threshold,request.maximumPixels);
        if (!bounds.ok()) { result.error=std::move(bounds.error); return false; }
        if (bounds.bounds && !unite(result.bounds,*bounds.bounds)) { result.error="content bounds exceed the coordinate range"; return false; }
        return true;
    };
    if (request.scope==CanvasContentBoundsScope::VisibleComposite) {
        for (auto frame:frames)
            if (!consume(renderFrameRegion(document,frame,region,region.extent,RasterSampling::Nearest))) return result;
        return result;
    }
    auto copy=document;
    copy.canvasMode=CanvasMode::Infinite; copy.infiniteCanvas.origin=region.origin; copy.extent=region.extent;
    copy.formatVersion.minor=CurrentFormatMinor;
    for (auto &layer:copy.layers) layerProperties(layer).visible=true;
    for (auto &board:copy.artboards) board.visible=true;
    for (const auto &group:copy.relations.groups.groups()) copy.relations.groups.setVisible(group.id,true);
    const FrameRenderTileRequest tile{region,region.extent,RasterSampling::Nearest};
    for (auto frame:frames) for (std::size_t i=0;i<copy.layers.size();++i) {
        if (layerRole(copy.layers[i])!=LayerRole::Artwork) continue;
        auto layer=renderFrameLayerTiles(copy,frame,i,{tile});
        if (!layer.ok()) return {{},layer.message.empty() ? "layer render failed" : layer.message};
        if (!layer.visible || layer.tiles.empty()) continue;
        if (!consume(FrameRenderResult{region.origin,std::move(layer.tiles.front().pixels),FrameRenderStatus::Success,{}})) return result;
    }
    for (const auto &board:copy.artboards) if ((board.backgroundArgb>>24)>request.threshold) {
        if (!board.geometry) {
            const auto clipped=intersection(region,board.region);
            if (clipped && !unite(result.bounds,*clipped)) return {{},"content bounds exceed the coordinate range"};
        } else {
            if (used>request.maximumPixels || pixels>request.maximumPixels-used)
                return {{},"content search exceeds the work budget"};
            FrameLayerBatchRenderResult batch; batch.requests={tile}; batch.artboards={board};
            auto rendered=composeFrameLayers(batch);
            if (!rendered.ok() || rendered.tiles.size()!=1) return {{},"artboard background render failed"};
            if (!consume(FrameRenderResult{region.origin,std::move(rendered.tiles.front().pixels),FrameRenderStatus::Success,{}})) return result;
        }
    }
    return result;
}
} // namespace iiSharedCanvas
