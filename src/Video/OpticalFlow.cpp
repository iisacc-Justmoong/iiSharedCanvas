#include "Video/OpticalFlow.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>

namespace iiSharedCanvas {
namespace {
bool valid(const RasterLayer &image, std::size_t limit) {
    if(image.width<=0 || image.height<=0)return false;
    const auto w=std::size_t(image.width),h=std::size_t(image.height);
    return w<=limit/h && image.pixels.size()==w*h;
}
bool aligned(const RasterLayer &a,const RasterLayer &b,std::size_t limit) {
    return valid(a,limit) && valid(b,limit) && a.width==b.width && a.height==b.height;
}
std::array<int,4> color(std::uint32_t pixel) {
    const int a=int(pixel>>24);
    return {int((pixel>>16)&255U)*a/255,int((pixel>>8)&255U)*a/255,int(pixel&255U)*a/255,a};
}
// A comparison is one pair of input pixels, not a hidden time or I/O limit.
std::uint64_t score(const RasterLayer &a,const RasterLayer &b,int x,int y,int dx,int dy,int radius) {
    std::uint64_t total=0;
    for(int py=-radius;py<=radius;++py)for(int px=-radius;px<=radius;++px){
        const int ax=x+px,ay=y+py,bx=ax+dx,by=ay+dy;
        if(ax<0||ay<0||ax>=a.width||ay>=a.height)continue;
        const auto u=color(a.pixels[std::size_t(ay)*a.width+ax]);
        const auto v=bx<0||by<0||bx>=b.width||by>=b.height ? color(0) :
                     color(b.pixels[std::size_t(by)*b.width+bx]);
        for(size_t i=0;i<u.size();++i){const int d=u[i]-v[i];total+=std::uint64_t(d*d);}
    }
    return total;
}
OpticalFlowField estimate(const RasterLayer &a,const RasterLayer &b,int search,int patch) {
    OpticalFlowField result{a.width,a.height,{}};
    result.vectors.resize(a.pixels.size());
    for(int y=0;y<a.height;++y)for(int x=0;x<a.width;++x){
        std::uint64_t best=std::numeric_limits<std::uint64_t>::max();int bestX=0,bestY=0,bestLength=0;
        for(int dy=-search;dy<=search;++dy)for(int dx=-search;dx<=search;++dx){
            if(x+dx<0||y+dy<0||x+dx>=a.width||y+dy>=a.height)continue;
            const auto current=score(a,b,x,y,dx,dy,patch);const int length=dx*dx+dy*dy;
            if(current<best || (current==best && length<bestLength)){
                best=current;bestX=dx;bestY=dy;bestLength=length;
            }
        }
        result.vectors[std::size_t(y)*a.width+x]={double(bestX),double(bestY)};
    }
    return result;
}
bool validField(const OpticalFlowField &field,const RasterLayer &image) {
    return field.width==image.width && field.height==image.height && field.vectors.size()==image.pixels.size() &&
        std::all_of(field.vectors.begin(),field.vectors.end(),[](const auto &v){return std::isfinite(v.x)&&std::isfinite(v.y);});
}
struct Premultiplied { double a=0,r=0,g=0,b=0; };
Premultiplied at(const RasterLayer &image,int x,int y) {
    if(x<0 || y<0 || x>=image.width || y>=image.height)return {};
    const auto p=image.pixels[std::size_t(y)*image.width+x];const double a=double(p>>24)/255.;
    return {a,double((p>>16)&255U)/255.*a,double((p>>8)&255U)/255.*a,double(p&255U)/255.*a};
}
Premultiplied sample(const RasterLayer &image,double x,double y) {
    if(x < -1 || y < -1 || x >= image.width || y >= image.height)return {};
    const int x0=int(std::floor(x)),y0=int(std::floor(y));const double fx=x-x0,fy=y-y0;
    Premultiplied result;
    for(int oy=0;oy<2;++oy)for(int ox=0;ox<2;++ox){
        const double w=(ox?fx:1-fx)*(oy?fy:1-fy);const auto p=at(image,x0+ox,y0+oy);
        result.a+=p.a*w;result.r+=p.r*w;result.g+=p.g*w;result.b+=p.b*w;
    }
    return result;
}
OpticalFlowVector vectorAt(const OpticalFlowField &field,double x,double y) {
    // Clamp before integer conversion so arbitrary finite injected vectors
    // cannot cause an out-of-range floating-point-to-integer conversion.
    x=std::clamp(x,0.,double(field.width-1));y=std::clamp(y,0.,double(field.height-1));
    const int ix=int(std::floor(x+0.5)),iy=int(std::floor(y+0.5));
    return field.vectors[std::size_t(iy)*field.width+ix];
}
Premultiplied warped(const RasterLayer &image,const OpticalFlowField &field,double x,double y,double factor) {
    double sx=x,sy=y;
    for(int i=0;i<4;++i){const auto v=vectorAt(field,sx,sy);sx=x-factor*v.x;sy=y-factor*v.y;}
    return sample(image,sx,sy);
}
std::uint32_t byte(double value){return std::uint32_t(std::lround(std::clamp(value,0.,1.)*255.));}
std::uint32_t mix(Premultiplied a,Premultiplied b,double phase) {
    const double alpha=a.a*(1-phase)+b.a*phase;
    if(alpha<=0)return 0;
    const double r=(a.r*(1-phase)+b.r*phase)/alpha,g=(a.g*(1-phase)+b.g*phase)/alpha,
                 blue=(a.b*(1-phase)+b.b*phase)/alpha;
    return (byte(alpha)<<24)|(byte(r)<<16)|(byte(g)<<8)|byte(blue);
}
} // namespace

OpticalFlowEstimateResult OpticalFlowEstimator::operator()(const RasterLayer &first,const RasterLayer &second,
                                                           const OpticalFlowOptions &options) const {
    if(!aligned(first,second,options.maximumPixels) || options.searchRadius>64 || options.patchRadius>16)
        return {{},"invalid or excessive optical-flow input"};
    const std::uint64_t side=std::uint64_t(options.searchRadius)*2+1,patch=std::uint64_t(options.patchRadius)*2+1;
    const std::uint64_t factor=side*side*patch*patch*2;
    if(first.pixels.size()>options.maximumComparisons/factor)return {{},"optical-flow comparison budget exceeded"};
    try {
        return {{estimate(first,second,int(options.searchRadius),int(options.patchRadius)),
                 estimate(second,first,int(options.searchRadius),int(options.patchRadius))},{}};
    } catch(const std::bad_alloc &) { return {{},"cannot allocate optical-flow fields"}; }
}

OpticalFlowRasterResult OpticalFlowInterpolator::operator()(const RasterLayer &first,const RasterLayer &second,
                                                             const OpticalFlowPair &flow,double phase,std::size_t maximumPixels) const {
    if(!aligned(first,second,maximumPixels) || !validField(flow.forward,first) || !validField(flow.backward,second) ||
       !std::isfinite(phase) || phase<0 || phase>1)return {{},"invalid optical-flow interpolation input"};
    try {
        if(phase==0)return {first,{}};
        if(phase==1)return {second,{}};
        RasterLayer result{first.width,first.height,{}};result.pixels.resize(first.pixels.size());
        for(int y=0;y<first.height;++y)for(int x=0;x<first.width;++x){
            const auto a=warped(first,flow.forward,x,y,phase);
            const auto b=warped(second,flow.backward,x,y,1-phase);
            result.pixels[std::size_t(y)*first.width+x]=mix(a,b,phase);
        }
        return {std::move(result),{}};
    } catch(const std::bad_alloc &) { return {{},"cannot allocate interpolated frame"}; }
}
} // namespace iiSharedCanvas
