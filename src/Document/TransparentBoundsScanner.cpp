#include "Document/TransparentBoundsScanner_p.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace iiSharedCanvas {
CanvasBoundsResult detail::scanTransparentBounds(const RasterLayer &pixels,
    CanvasOrigin origin,std::uint8_t threshold,std::size_t maximumPixels)
{
    if (pixels.width<=0 || pixels.height<=0)
        return {{},"raster dimensions must be positive"};
    const auto count=std::uint64_t(pixels.width)*std::uint64_t(pixels.height);
    if (count!=pixels.pixels.size() || count>maximumPixels)
        return {{},"raster storage or scan budget is invalid"};
    constexpr auto maximum=std::numeric_limits<std::int32_t>::max();
    if (std::int64_t(origin.x)+pixels.width-1>maximum ||
        std::int64_t(origin.y)+pixels.height-1>maximum)
        return {{},"raster bounds exceed document coordinates"};
    auto left=pixels.width,top=pixels.height,right=-1,bottom=-1;
    for (int y=0;y<pixels.height;++y) for (int x=0;x<pixels.width;++x) {
        if ((pixels.pixels[std::size_t(y)*std::size_t(pixels.width)+std::size_t(x)]>>24)<=threshold) continue;
        left=std::min(left,x); top=std::min(top,y); right=std::max(right,x); bottom=std::max(bottom,y);
    }
    if (right<0) return {};
    return {CanvasRegion{{static_cast<std::int32_t>(std::int64_t(origin.x)+left),
                          static_cast<std::int32_t>(std::int64_t(origin.y)+top)},
                         {right-left+1,bottom-top+1}},{}};
}
CanvasBoundsResult TransparentBoundsScanner::operator()(const RasterLayer &pixels,
    CanvasOrigin origin,std::uint8_t threshold,std::size_t maximumPixels) const
{
    return detail::scanTransparentBounds(pixels,origin,threshold,maximumPixels);
}
} // namespace iiSharedCanvas
