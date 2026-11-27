#include "Document/CanvasGeometry.h"
#include "Document/CanvasAffine_p.hpp"
#include "Validation/Validation.h"

#include <cstdint>
#include <limits>
#include <numbers>

namespace iiSharedCanvas {
namespace {
bool validRegion(CanvasRegion region) noexcept
{
    constexpr auto maximum=std::numeric_limits<std::int32_t>::max();
    return region.extent.width>0 && region.extent.height>0 &&
        std::int64_t(region.origin.x)+region.extent.width<=maximum &&
        std::int64_t(region.origin.y)+region.extent.height<=maximum;
}
std::int64_t anchored(std::int64_t difference,int anchor) noexcept
{
    return anchor==0 ? 0 : anchor==1 ? difference/2 : difference;
}
}
CanvasGeometryResult CanvasResizePlan::operator()(const Document &document,CanvasExtent extent,
    CanvasAnchor anchor) const
{
    if (!validate(document).ok() || extent.width<=0 || extent.height<=0 ||
        static_cast<unsigned>(anchor)>static_cast<unsigned>(CanvasAnchor::BottomRight))
        return {{},"invalid document, extent or anchor"};
    const auto prior=canvasRegion(document);
    const int index=static_cast<int>(anchor);
    const auto dx=anchored(std::int64_t(extent.width)-prior.extent.width,index%3);
    const auto dy=anchored(std::int64_t(extent.height)-prior.extent.height,index/3);
    CanvasRegion target{{0,0},extent}; CanvasOrigin offset{};
    if (document.canvasMode==CanvasMode::Finite) {
        offset={static_cast<std::int32_t>(dx),static_cast<std::int32_t>(dy)};
    } else {
        const auto x=std::int64_t(prior.origin.x)-dx,y=std::int64_t(prior.origin.y)-dy;
        if (x<std::numeric_limits<std::int32_t>::min() || x>std::numeric_limits<std::int32_t>::max() ||
            y<std::numeric_limits<std::int32_t>::min() || y>std::numeric_limits<std::int32_t>::max())
            return {{},"resized viewport origin overflows"};
        target.origin={static_cast<std::int32_t>(x),static_cast<std::int32_t>(y)};
    }
    if (!validRegion(target)) return {{},"resized canvas is out of range"};
    return {{document.authorship.revision(),document.canvasMode,prior,target,offset},{}};
}
CanvasGeometryResult CanvasCropTransaction::operator()(const Document &document,CanvasRegion region) const
{
    if (!validate(document).ok() || !validRegion(region)) return {{},"invalid document or crop region"};
    const auto prior=canvasRegion(document);
    if (region.origin.x<prior.origin.x || region.origin.y<prior.origin.y ||
        std::int64_t(region.origin.x)+region.extent.width>std::int64_t(prior.origin.x)+prior.extent.width ||
        std::int64_t(region.origin.y)+region.extent.height>std::int64_t(prior.origin.y)+prior.extent.height)
        return {{},"crop must lie inside the current canvas"};
    auto target=region; CanvasOrigin offset{};
    if (document.canvasMode==CanvasMode::Finite) {
        target.origin={}; offset={-region.origin.x,-region.origin.y};
    }
    return {{document.authorship.revision(),document.canvasMode,prior,target,offset},{}};
}
CanvasAffineResult DocumentResamplingPlan::operator()(const Document &document,CanvasExtent extent,
    RasterResamplingMode mode) const
{
    if (!validate(document).ok() || extent.width<=0 || extent.height<=0 ||
        (mode!=RasterResamplingMode::PreserveSource && mode!=RasterResamplingMode::ReplacePixels))
        return {{},"invalid document, extent or raster resampling mode"};
    const auto source=canvasRegion(document);
    CanvasRegion target{source.origin,extent};
    if (!validRegion(target)) return {{},"resampled canvas is out of range"};
    AffineTransform t;
    t.m11=double(extent.width)/source.extent.width; t.m22=double(extent.height)/source.extent.height;
    t.translationX=source.origin.x*(1-t.m11); t.translationY=source.origin.y*(1-t.m22);
    return {{document.authorship.revision(),document.canvasMode,source,target,t,mode},{}};
}
CanvasAffineResult ImageRotationPlan::operator()(const Document &document,double degrees) const
{
    if (!validate(document).ok() || !std::isfinite(degrees)) return {{},"invalid document or angle"};
    const auto source=canvasRegion(document);
    const double radians=std::remainder(degrees,360.0)*std::numbers::pi/180;
    auto c=std::cos(radians),s=std::sin(radians);
    if (std::abs(c)<1e-15) c=0; if (std::abs(s)<1e-15) s=0;
    // Pixel coordinates are converted to physical space for the rotation and
    // then converted back, so a nonsquare pixel aspect is not treated as 1:1.
    const double aspect=double(document.pixelAspectRatio.numerator)/document.pixelAspectRatio.denominator;
    const double a=c,b=s*aspect,cc=-s/aspect,d=c;
    const double cx=source.origin.x+source.extent.width/2.0,cy=source.origin.y+source.extent.height/2.0;
    AffineTransform t{a,b,cc,d,cx-a*cx-cc*cy,cy-b*cx-d*cy};
    auto target=canvas_affine_detail::bounds(source,t);
    if (!target) return {{},"rotated canvas is out of range"};
    if (document.canvasMode==CanvasMode::Finite) {
        const double spanX=std::abs(a)*source.extent.width+std::abs(cc)*source.extent.height;
        const double spanY=std::abs(b)*source.extent.width+std::abs(d)*source.extent.height;
        const double width=std::ceil(spanX),height=std::ceil(spanY);
        if (width>std::numeric_limits<std::int32_t>::max() || height>std::numeric_limits<std::int32_t>::max())
            return {{},"rotated canvas is out of range"};
        const double left=cx-spanX/2,top=cy-spanY/2;
        t.translationX+=-left+(width-spanX)/2; t.translationY+=-top+(height-spanY)/2;
        *target={{},{std::int32_t(width),std::int32_t(height)}};
    }
    return {{document.authorship.revision(),document.canvasMode,source,*target,t,RasterResamplingMode::PreserveSource},{}};
}
} // namespace iiSharedCanvas
