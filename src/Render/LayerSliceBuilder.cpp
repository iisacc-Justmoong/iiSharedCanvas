#include "Render/LayerSliceBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace iiSharedCanvas {
namespace {
// Sample straight ARGB through premultiplied channels so invisible color does
// not leak into a scaled slice. Sampling clamps at the supplied image edges.
std::uint32_t sample(const RasterLayer &image,double x,double y,RasterSampling mode)
{
    const auto pixel=[&](int px,int py) {
        px=std::clamp(px,0,image.width-1); py=std::clamp(py,0,image.height-1);
        return image.pixels[std::size_t(py)*image.width+px];
    };
    if (mode==RasterSampling::Nearest) {
        const auto value=pixel(static_cast<int>(std::floor(x+.5)),static_cast<int>(std::floor(y+.5)));
        return value>>24 ? value : 0;
    }
    const int x0=static_cast<int>(std::floor(x)),y0=static_cast<int>(std::floor(y));
    const double fx=x-x0,fy=y-y0;
    const std::array values{pixel(x0,y0),pixel(x0+1,y0),pixel(x0,y0+1),pixel(x0+1,y0+1)};
    const std::array weights{(1-fx)*(1-fy),fx*(1-fy),(1-fx)*fy,fx*fy};
    double alpha=0; std::array<double,3> channels{};
    for (std::size_t i=0;i<values.size();++i) {
        const double coverage=double(values[i]>>24)/255.*weights[i]; alpha+=coverage;
        channels[0]+=coverage*((values[i]>>16)&255U);
        channels[1]+=coverage*((values[i]>>8)&255U);
        channels[2]+=coverage*(values[i]&255U);
    }
    const auto outAlpha=static_cast<std::uint32_t>(std::lround(alpha*255.));
    if (!outAlpha || alpha<=0) return 0;
    const auto channel=[&](std::size_t index) {
        return static_cast<std::uint32_t>(std::clamp(std::lround(channels[index]/alpha),0L,255L));
    };
    return (outAlpha<<24)|(channel(0)<<16)|(channel(1)<<8)|channel(2);
}
} // namespace

FrameRenderResult LayerSliceBuilder::operator()(const FrameRenderResult &rendered,
    CanvasRegion sourceRegion, const LayerSliceDescriptor &descriptor) const
{
    const auto &region=descriptor.region;
    const auto invalid=[&](const char *message) {
        return FrameRenderResult{region.origin,{},FrameRenderStatus::InvalidRegion,message};
    };
    if (!rendered.ok()) return {region.origin,{},rendered.status,rendered.message};
    if ((descriptor.source!=LayerSliceSource::Layer && descriptor.source!=LayerSliceSource::Composite) ||
        (descriptor.sampling!=RasterSampling::Nearest && descriptor.sampling!=RasterSampling::Smooth) ||
        sourceRegion.origin!=rendered.origin || sourceRegion.extent.width<=0 || sourceRegion.extent.height<=0 ||
        region.extent.width<=0 || region.extent.height<=0 || descriptor.outputExtent.width<=0 ||
        descriptor.outputExtent.height<=0 || rendered.pixels.width<=0 || rendered.pixels.height<=0 ||
        std::uint64_t(rendered.pixels.width)*std::uint64_t(rendered.pixels.height)!=rendered.pixels.pixels.size() ||
        std::uint64_t(descriptor.outputExtent.width)*std::uint64_t(descriptor.outputExtent.height)>
            std::numeric_limits<std::size_t>::max()/sizeof(std::uint32_t))
        return invalid("invalid slice source, geometry or sampling");
    const auto right=[](CanvasRegion r) { return std::int64_t(r.origin.x)+r.extent.width; };
    const auto bottom=[](CanvasRegion r) { return std::int64_t(r.origin.y)+r.extent.height; };
    if (region.origin.x<sourceRegion.origin.x || region.origin.y<sourceRegion.origin.y ||
        right(region)>right(sourceRegion) || bottom(region)>bottom(sourceRegion))
        return invalid("slice region lies outside the supplied source");
    if (region==sourceRegion && rendered.pixels.width==descriptor.outputExtent.width &&
        rendered.pixels.height==descriptor.outputExtent.height)
        return {region.origin,rendered.pixels,FrameRenderStatus::Success,{}};
    auto output=makeRasterLayer(descriptor.outputExtent.width,descriptor.outputExtent.height,0);
    for (int y=0;y<output.height;++y) for (int x=0;x<output.width;++x) {
        const double worldX=double(std::int64_t(region.origin.x)-sourceRegion.origin.x)+
            (double(x)+.5)*region.extent.width/output.width;
        const double worldY=double(std::int64_t(region.origin.y)-sourceRegion.origin.y)+
            (double(y)+.5)*region.extent.height/output.height;
        const double px=worldX*rendered.pixels.width/sourceRegion.extent.width-.5;
        const double py=worldY*rendered.pixels.height/sourceRegion.extent.height-.5;
        output.pixels[std::size_t(y)*output.width+x]=sample(rendered.pixels,px,py,descriptor.sampling);
    }
    return {region.origin,std::move(output),FrameRenderStatus::Success,{}};
}
} // namespace iiSharedCanvas
