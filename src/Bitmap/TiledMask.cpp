#include "Bitmap/TiledMask.h"
#include "Bitmap/SelectionPolygon_p.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <utility>
#include <vector>

namespace iiSharedCanvas {
namespace {
constexpr int side = 64;
using Key = std::pair<std::int32_t,std::int32_t>;
using Pixels = std::array<std::uint8_t,side*side>;
struct Tile { std::uint8_t uniform = 0; std::shared_ptr<const Pixels> dense; };
bool valid(CanvasRegion r) {
    if (r.extent.width <= 0 || r.extent.height <= 0) return false;
    constexpr auto max = std::numeric_limits<std::int32_t>::max();
    return std::int64_t(r.origin.x)+r.extent.width-1 <= max &&
           std::int64_t(r.origin.y)+r.extent.height-1 <= max;
}
std::uint8_t apply(std::uint8_t a,std::uint8_t b,MaskOperation op) {
    switch(op) {
        case MaskOperation::Replace:return b;
        case MaskOperation::Add:return std::max(a,b);
        case MaskOperation::Subtract:return std::uint8_t(std::max(0,int(a)-int(b)));
        case MaskOperation::Intersect:return std::min(a,b);
    }
    return 0;
}
bool operationValid(MaskOperation op) {
    return op==MaskOperation::Replace || op==MaskOperation::Add ||
           op==MaskOperation::Subtract || op==MaskOperation::Intersect;
}
}
struct TiledMask::State {
    CanvasRegion domain{};
    std::uint8_t background=0;
    std::map<Key,Tile> tiles;
    std::size_t denseBytes=0;
};
namespace {
std::uint8_t at(const Tile* tile,std::uint8_t background,std::size_t i) {
    return !tile ? background : tile->dense ? (*tile->dense)[i] : tile->uniform;
}
bool putUniform(TiledMask::State& state,Key key,std::uint8_t value,TiledMaskLimits limits) {
    if(value==state.background) return true;
    if(state.tiles.size()>=limits.maximumTiles) return false;
    state.tiles.emplace(key,Tile{value,{}});return true;
}
bool put(TiledMask::State& state,Key key,const Pixels& pixels,TiledMaskLimits limits) {
    const auto first=pixels.front();
    const int ox=key.first*side, oy=key.second*side;
    const int width=std::min(side,state.domain.extent.width-ox),height=std::min(side,state.domain.extent.height-oy);
    bool uniform=true;
    for(int y=0;y<height && uniform;++y) for(int x=0;x<width;++x)
        if(pixels[std::size_t(y)*side+x]!=first) {uniform=false;break;}
    if (uniform) return putUniform(state,key,first,limits);
    if (state.tiles.size()>=limits.maximumTiles || state.denseBytes>limits.maximumDenseBytes ||
        sizeof(Pixels)>limits.maximumDenseBytes-state.denseBytes) return false;
    Tile tile;tile.dense=std::make_shared<Pixels>(pixels);state.denseBytes+=sizeof(Pixels);
    state.tiles.emplace(key,std::move(tile));return true;
}
}
TiledMaskResult TiledMask::rectangle(CanvasRegion domain,Point first,Point second,bool antialias,TiledMaskLimits limits) {
    if (!valid(domain) || !std::isfinite(first.x) || !std::isfinite(first.y) ||
        !std::isfinite(second.x) || !std::isfinite(second.y)) return {{},"invalid mask domain or rectangle"};
    try {
        auto state=std::make_shared<State>();state->domain=domain;
        const double left=std::clamp(std::min(first.x,second.x)-double(domain.origin.x),0.,double(domain.extent.width));
        const double right=std::clamp(std::max(first.x,second.x)-double(domain.origin.x),0.,double(domain.extent.width));
        const double top=std::clamp(std::min(first.y,second.y)-double(domain.origin.y),0.,double(domain.extent.height));
        const double bottom=std::clamp(std::max(first.y,second.y)-double(domain.origin.y),0.,double(domain.extent.height));
        if (left>=right || top>=bottom) return {TiledMask(std::move(state)),{}};
        if(left==0 && top==0 && right==domain.extent.width && bottom==domain.extent.height) {
            state->background=255;return {TiledMask(std::move(state)),{}};
        }
        const auto x0=std::int64_t(std::floor(left))/side, x1=(std::int64_t(std::ceil(right))-1)/side;
        const auto y0=std::int64_t(std::floor(top))/side, y1=(std::int64_t(std::ceil(bottom))-1)/side;
        const auto count=std::uint64_t(x1-x0+1)*std::uint64_t(y1-y0+1);
        if(count>limits.maximumTiles) return {{},"mask exceeds tile budget"};
        for(auto ty=y0;ty<=y1;++ty) for(auto tx=x0;tx<=x1;++tx) {
            const int ox=int(tx*side),oy=int(ty*side);
            const int width=std::min(side,domain.extent.width-ox),height=std::min(side,domain.extent.height-oy);
            const Key key{std::int32_t(tx),std::int32_t(ty)};
            if(left<=ox && top<=oy && right>=ox+width && bottom>=oy+height) {
                if(!putUniform(*state,key,255,limits)) return {{},"mask exceeds storage budget"};
                continue;
            }
            Pixels pixels{};
            auto tile=selectionGeometry({width,height},SelectionGeometry::Rectangle,{left-ox,top-oy},{right-ox,bottom-oy},antialias);
            if(!tile.ok()) return {{},tile.error};
            for(int y=0;y<height;++y) std::copy_n(tile.mask.alpha.begin()+std::size_t(y)*width,width,pixels.begin()+y*side);
            if(!put(*state,key,pixels,limits)) return {{},"mask exceeds storage budget"};
        }
        return {TiledMask(std::move(state)),{}};
    } catch(const std::bad_alloc&) { return {{},"insufficient memory for tiled mask"}; }
}
TiledMaskResult TiledMask::ellipse(CanvasRegion domain,Point first,Point second,bool antialias,TiledMaskLimits limits) {
    if (!valid(domain) || !std::isfinite(first.x) || !std::isfinite(first.y) ||
        !std::isfinite(second.x) || !std::isfinite(second.y)) return {{},"invalid mask domain or ellipse"};
    try {
        auto state=std::make_shared<State>();state->domain=domain;
        const double left=std::min(first.x,second.x)-domain.origin.x, right=std::max(first.x,second.x)-domain.origin.x;
        const double top=std::min(first.y,second.y)-domain.origin.y, bottom=std::max(first.y,second.y)-domain.origin.y;
        if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) || !std::isfinite(bottom))
            return {{},"invalid ellipse coordinates"};
        if (left>=right || top>=bottom) return {TiledMask(std::move(state)),{}};
        const double xlo=std::clamp(left,0.,double(domain.extent.width)), xhi=std::clamp(right,0.,double(domain.extent.width));
        const double ylo=std::clamp(top,0.,double(domain.extent.height)), yhi=std::clamp(bottom,0.,double(domain.extent.height));
        if (xlo>=xhi || ylo>=yhi) return {TiledMask(std::move(state)),{}};
        const auto x0=std::int64_t(std::floor(xlo))/side, x1=(std::int64_t(std::ceil(xhi))-1)/side;
        const auto y0=std::int64_t(std::floor(ylo))/side, y1=(std::int64_t(std::ceil(yhi))-1)/side;
        const auto count=std::uint64_t(x1-x0+1)*std::uint64_t(y1-y0+1);
        if (count>limits.maximumTiles) return {{},"mask exceeds tile budget"};
        const double cx=left/2+right/2, cy=top/2+bottom/2, rx=right/2-left/2, ry=bottom/2-top/2;
        for (auto ty=y0;ty<=y1;++ty) for (auto tx=x0;tx<=x1;++tx) {
            const int ox=int(tx*side),oy=int(ty*side);
            const int width=std::min(side,domain.extent.width-ox),height=std::min(side,domain.extent.height-oy);
            const Key key{std::int32_t(tx),std::int32_t(ty)};
            const auto minimumDistance=[](double center,double lo,double hi) {return center<lo ? lo-center : center>hi ? center-hi : 0.;};
            const double nearX=minimumDistance(cx,ox,ox+width)/rx, nearY=minimumDistance(cy,oy,oy+height)/ry;
            if (nearX*nearX+nearY*nearY>1.) continue;
            const double farX=std::max(std::abs(ox-cx),std::abs(ox+width-cx))/rx;
            const double farY=std::max(std::abs(oy-cy),std::abs(oy+height-cy))/ry;
            if (farX*farX+farY*farY<=1.) {
                if(!putUniform(*state,key,255,limits)) return {{},"mask exceeds storage budget"};
                continue;
            }
            Pixels pixels{};
            auto tile=selectionGeometry({width,height},SelectionGeometry::Ellipse,{left-ox,top-oy},{right-ox,bottom-oy},antialias);
            if (!tile.ok()) return {{},tile.error};
            for(int y=0;y<height;++y) std::copy_n(tile.mask.alpha.begin()+std::size_t(y)*width,width,pixels.begin()+y*side);
            if(!put(*state,key,pixels,limits)) return {{},"mask exceeds storage budget"};
        }
        return {TiledMask(std::move(state)),{}};
    } catch(const std::bad_alloc&) { return {{},"insufficient memory for tiled mask"}; }
}
TiledMaskResult TiledMask::polygon(CanvasRegion domain,std::span<const Point> points,bool antialias,TiledMaskLimits limits) {
    constexpr std::uint64_t maxScans=50'000'000;
    constexpr double maxCoordinate=1'000'000'000.;
    if (!valid(domain) || points.size()<3 || points.size()>65536) return {{},"invalid mask domain or polygon point count"};
    try {
        auto state=std::make_shared<State>();state->domain=domain;
        std::vector<Point> local;local.reserve(points.size());
        double left=std::numeric_limits<double>::infinity(),top=left,right=-left,bottom=-left;
        for (const auto& p:points) {
            const Point q{p.x-domain.origin.x,p.y-domain.origin.y};
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || std::abs(q.x)>maxCoordinate || std::abs(q.y)>maxCoordinate)
                return {{},"invalid polygon point"};
            local.push_back(q);left=std::min(left,q.x);right=std::max(right,q.x);top=std::min(top,q.y);bottom=std::max(bottom,q.y);
        }
        const auto xlo=std::clamp(left,0.,double(domain.extent.width)),xhi=std::clamp(right,0.,double(domain.extent.width));
        const auto ylo=std::clamp(top,0.,double(domain.extent.height)),yhi=std::clamp(bottom,0.,double(domain.extent.height));
        if(xlo>=xhi || ylo>=yhi) return {TiledMask(std::move(state)),{}};
        const auto x0=std::int64_t(std::floor(xlo))/side,x1=(std::int64_t(std::ceil(xhi))-1)/side;
        const auto y0=std::int64_t(std::floor(ylo))/side,y1=(std::int64_t(std::ceil(yhi))-1)/side;
        const auto columns=std::uint64_t(x1-x0+1),rows=std::uint64_t(y1-y0+1);
        if(columns*rows>limits.maximumTiles) return {{},"mask exceeds tile budget"};
        std::uint64_t scans=columns;
        for (const auto factor:{rows,std::uint64_t(side),std::uint64_t(antialias?4:1),std::uint64_t(points.size())}) {
            if(factor && scans>maxScans/factor) return {{},"polygon exceeds tiled edge scan budget"};
            scans*=factor;
        }
        for(auto ty=y0;ty<=y1;++ty) for(auto tx=x0;tx<=x1;++tx) {
            const int ox=int(tx*side),oy=int(ty*side),width=std::min(side,domain.extent.width-ox),height=std::min(side,domain.extent.height-oy);
            auto tile=detail::selectionPolygonRegion(domain.extent,{{ox,oy},{width,height}},local,antialias);
            if(!tile.ok())return {{},tile.error};
            Pixels pixels{};for(int y=0;y<height;++y)std::copy_n(tile.mask.alpha.begin()+std::size_t(y)*width,width,pixels.begin()+y*side);
            if(!put(*state,{std::int32_t(tx),std::int32_t(ty)},pixels,limits))return {{},"mask exceeds storage budget"};
        }
        return {TiledMask(std::move(state)),{}};
    } catch(const std::bad_alloc&) { return {{},"insufficient memory for tiled mask"}; }
}
TiledMaskResult TiledMask::fromRaster(CanvasRegion domain,CanvasOrigin origin,const RasterMask& raster,
                                            std::uint8_t outsideCoverage,TiledMaskLimits limits) {
    const auto width=raster.extent.width,height=raster.extent.height;
    if (!valid(domain) || width<=0 || height<=0 || std::uint64_t(width)*height>64ULL*1024*1024 ||
        raster.alpha.size()!=std::size_t(width)*height ||
        std::int64_t(origin.x)+width-1>std::numeric_limits<std::int32_t>::max() ||
        std::int64_t(origin.y)+height-1>std::numeric_limits<std::int32_t>::max())
        return {{},"invalid mask domain or raster region"};
    try {
        auto state=std::make_shared<State>();state->domain=domain;state->background=outsideCoverage;
        const auto left=std::max<std::int64_t>(domain.origin.x,origin.x);
        const auto top=std::max<std::int64_t>(domain.origin.y,origin.y);
        const auto right=std::min<std::int64_t>(std::int64_t(domain.origin.x)+domain.extent.width,std::int64_t(origin.x)+width);
        const auto bottom=std::min<std::int64_t>(std::int64_t(domain.origin.y)+domain.extent.height,std::int64_t(origin.y)+height);
        if(left>=right || top>=bottom) return {TiledMask(std::move(state)),{}};
        const auto x0=(left-domain.origin.x)/side,x1=(right-1-domain.origin.x)/side;
        const auto y0=(top-domain.origin.y)/side,y1=(bottom-1-domain.origin.y)/side;
        if(std::uint64_t(x1-x0+1)*std::uint64_t(y1-y0+1)>limits.maximumTiles) return {{},"mask exceeds tile budget"};
        for(auto ty=y0;ty<=y1;++ty)for(auto tx=x0;tx<=x1;++tx) {
            const int ox=int(tx*side),oy=int(ty*side);
            const int tileWidth=std::min(side,domain.extent.width-ox),tileHeight=std::min(side,domain.extent.height-oy);
            Pixels pixels;pixels.fill(outsideCoverage);
            for(int y=0;y<tileHeight;++y)for(int x=0;x<tileWidth;++x) {
                const auto wx=std::int64_t(domain.origin.x)+ox+x,wy=std::int64_t(domain.origin.y)+oy+y;
                if(wx>=left && wx<right && wy>=top && wy<bottom)
                    pixels[std::size_t(y)*side+x]=raster.alpha[std::size_t(wy-origin.y)*width+std::size_t(wx-origin.x)];
            }
            if(!put(*state,{std::int32_t(tx),std::int32_t(ty)},pixels,limits)) return {{},"mask exceeds storage budget"};
        }
        return {TiledMask(std::move(state)),{}};
    } catch(const std::bad_alloc&) { return {{},"insufficient memory for tiled mask"}; }
}
TiledMaskResult TiledMask::combine(const TiledMask& other,MaskOperation op,TiledMaskLimits limits) const {
    if(!state_ || !other.state_ || state_->domain!=other.state_->domain || !operationValid(op)) return {{},"invalid masks or operation"};
    try {
        auto result=std::make_shared<State>();result->domain=state_->domain;result->background=apply(state_->background,other.state_->background,op);
        auto a=state_->tiles.begin(),b=other.state_->tiles.begin();
        while(a!=state_->tiles.end() || b!=other.state_->tiles.end()) {
            Key key;
            if(b==other.state_->tiles.end() || (a!=state_->tiles.end() && a->first<b->first)) key=a->first;
            else key=b->first;
            const Tile* ta=nullptr;const Tile* tb=nullptr;
            if(a!=state_->tiles.end() && a->first==key){ta=&a->second;++a;}
            if(b!=other.state_->tiles.end() && b->first==key){tb=&b->second;++b;}
            if ((!ta || !ta->dense) && (!tb || !tb->dense)) {
                const auto value=apply(ta ? ta->uniform : state_->background,tb ? tb->uniform : other.state_->background,op);
                if(!putUniform(*result,key,value,limits)) return {{},"mask exceeds storage budget"};
                continue;
            }
            Pixels pixels;pixels.fill(result->background);
            const int ox=key.first*side,oy=key.second*side;
            const int width=std::min(side,result->domain.extent.width-ox),height=std::min(side,result->domain.extent.height-oy);
            for(int y=0;y<height;++y)for(int x=0;x<width;++x){const auto i=std::size_t(y)*side+x;pixels[i]=apply(at(ta,state_->background,i),at(tb,other.state_->background,i),op);}
            if(!put(*result,key,pixels,limits)) return {{},"mask exceeds storage budget"};
        }
        return {TiledMask(std::move(result)),{}};
    } catch(const std::bad_alloc&) { return {{},"insufficient memory for tiled mask"}; }
}
TiledMaskResult TiledMask::reframed(CanvasRegion source,CanvasOrigin destination,TiledMaskLimits limits) const {
    if(!state_ || !valid(source) || !valid({destination,source.extent}))return {{},"invalid mask crop region"};
    const auto domain=state_->domain;
    if(source.origin.x<domain.origin.x || source.origin.y<domain.origin.y ||
       std::int64_t(source.origin.x)+source.extent.width>std::int64_t(domain.origin.x)+domain.extent.width ||
       std::int64_t(source.origin.y)+source.extent.height>std::int64_t(domain.origin.y)+domain.extent.height)
        return {{},"mask crop region lies outside the domain"};
    try {
        if(source==domain) {auto next=std::make_shared<State>(*state_);next->domain={destination,source.extent};
            if(next->tiles.size()>limits.maximumTiles || next->denseBytes>limits.maximumDenseBytes)return {{},"mask exceeds storage budget"};
            return {TiledMask(std::move(next)),{}};}
        auto next=std::make_shared<State>();next->domain={destination,source.extent};next->background=state_->background;
        constexpr std::uint64_t maxPixels=16ULL*1024*1024;std::uint64_t examined=0;std::map<Key,Pixels> changed;
        for(const auto& [key,tile]:state_->tiles) {
            const auto tx=std::int64_t(domain.origin.x)+std::int64_t(key.first)*side;
            const auto ty=std::int64_t(domain.origin.y)+std::int64_t(key.second)*side;
            const auto left=std::max<std::int64_t>(tx,source.origin.x),top=std::max<std::int64_t>(ty,source.origin.y);
            const auto right=std::min({tx+side,std::int64_t(source.origin.x)+source.extent.width,std::int64_t(domain.origin.x)+domain.extent.width});
            const auto bottom=std::min({ty+side,std::int64_t(source.origin.y)+source.extent.height,std::int64_t(domain.origin.y)+domain.extent.height});
            if(right<=left||bottom<=top)continue;
            const auto count=std::uint64_t(right-left)*std::uint64_t(bottom-top);
            if(count>maxPixels-examined)return {{},"mask crop exceeds the working budget"};examined+=count;
            for(auto y=top;y<bottom;++y)for(auto x=left;x<right;++x) {
                const auto lx=x-source.origin.x,ly=y-source.origin.y;Key out{std::int32_t(lx/side),std::int32_t(ly/side)};
                auto [it,inserted]=changed.try_emplace(out);if(inserted)it->second.fill(state_->background);
                it->second[std::size_t(ly%side)*side+std::size_t(lx%side)]=at(&tile,state_->background,std::size_t(y-ty)*side+std::size_t(x-tx));
            }
        }
        for(const auto& [key,pixels]:changed)if(!put(*next,key,pixels,limits))return {{},"mask exceeds storage budget"};
        return {TiledMask(std::move(next)),{}};
    } catch(const std::bad_alloc&) {return {{},"insufficient memory for mask crop"};}
}
TiledMaskResult TiledMask::patched(std::span<const TiledMaskPatch> patches,TiledMaskLimits limits) const {
    if (!state_) return {{},"invalid tiled mask"};
    constexpr std::uint64_t maxPixels=16ULL*1024*1024;
    std::uint64_t total=0;
    for(const auto& patch:patches) {
        const auto& extent=patch.pixels.extent;
        if(extent.width<=0 || extent.height<=0 || std::uint64_t(extent.width)*extent.height>maxPixels-total ||
           patch.pixels.alpha.size()!=std::size_t(extent.width)*extent.height)
            return {{},"invalid or oversized mask patch"};
        total+=std::uint64_t(extent.width)*extent.height;
    }
    try {
        auto next=std::make_shared<State>(*state_);
        if(next->tiles.size()>limits.maximumTiles || next->denseBytes>limits.maximumDenseBytes)
            return {{},"mask exceeds storage budget"};
        std::map<Key,Pixels> changed;
        for(const auto& patch:patches) {
            const auto& e=patch.pixels.extent;
            const auto left=std::max<std::int64_t>(patch.origin.x,state_->domain.origin.x);
            const auto top=std::max<std::int64_t>(patch.origin.y,state_->domain.origin.y);
            const auto right=std::min<std::int64_t>(std::int64_t(patch.origin.x)+e.width,std::int64_t(state_->domain.origin.x)+state_->domain.extent.width);
            const auto bottom=std::min<std::int64_t>(std::int64_t(patch.origin.y)+e.height,std::int64_t(state_->domain.origin.y)+state_->domain.extent.height);
            for(auto y=top;y<bottom;++y) for(auto x=left;x<right;++x) {
                const auto lx=x-state_->domain.origin.x,ly=y-state_->domain.origin.y;
                Key key{std::int32_t(lx/side),std::int32_t(ly/side)};
                auto [it,inserted]=changed.try_emplace(key);
                if(inserted) {
                    const auto old=state_->tiles.find(key);const Tile* tile=old==state_->tiles.end()?nullptr:&old->second;
                    for(std::size_t i=0;i<it->second.size();++i)it->second[i]=at(tile,state_->background,i);
                }
                it->second[std::size_t(ly%side)*side+std::size_t(lx%side)]=patch.pixels.alpha[std::size_t(y-patch.origin.y)*e.width+std::size_t(x-patch.origin.x)];
            }
        }
        for(const auto& [key,pixels]:changed) {
            const auto old=next->tiles.find(key);
            if(old!=next->tiles.end()) {if(old->second.dense)next->denseBytes-=sizeof(Pixels);next->tiles.erase(old);}
            if(!put(*next,key,pixels,limits)) return {{},"mask exceeds storage budget"};
        }
        return {TiledMask(std::move(next)),{}};
    } catch(const std::bad_alloc&) {return {{},"insufficient memory for mask patches"};}
}
TiledMaskResult TiledMask::inverted(TiledMaskLimits limits) const {
    if(!state_) return {{},"invalid mask"};
    try {
        auto result=std::make_shared<State>();result->domain=state_->domain;result->background=std::uint8_t(255-state_->background);
        for(const auto& [key,tile]:state_->tiles) {
            if(!tile.dense) {
                if(!putUniform(*result,key,std::uint8_t(255-tile.uniform),limits)) return {{},"mask exceeds storage budget"};
                continue;
            }
            Pixels pixels;pixels.fill(result->background);
            const int ox=key.first*side,oy=key.second*side;
            const int width=std::min(side,result->domain.extent.width-ox),height=std::min(side,result->domain.extent.height-oy);
            for(int y=0;y<height;++y)for(int x=0;x<width;++x){const auto i=std::size_t(y)*side+x;pixels[i]=std::uint8_t(255-at(&tile,state_->background,i));}
            if(!put(*result,key,pixels,limits)) return {{},"mask exceeds storage budget"};
        }
        return {TiledMask(std::move(result)),{}};
    } catch(const std::bad_alloc&) { return {{},"insufficient memory for tiled mask"}; }
}
TiledMaskResult TiledMask::feathered(double radius,TiledMaskLimits limits) const {
    if(!state_ || !std::isfinite(radius) || radius<0 || radius>256) return {{},"invalid mask or feather radius"};
    if(radius==0 || state_->tiles.empty()) return {*this,{}};
    const auto bounds=nonzeroBounds();if(!bounds) return {*this,{}};
    const auto margin=std::int64_t(std::ceil(radius))*2;
    const auto domainLeft=std::int64_t(state_->domain.origin.x),domainTop=std::int64_t(state_->domain.origin.y);
    const auto domainRight=domainLeft+state_->domain.extent.width,domainBottom=domainTop+state_->domain.extent.height;
    const auto left=std::max(domainLeft,std::int64_t(bounds->origin.x)-margin);
    const auto top=std::max(domainTop,std::int64_t(bounds->origin.y)-margin);
    const auto right=std::min(domainRight,std::int64_t(bounds->origin.x)+bounds->extent.width+margin);
    const auto bottom=std::min(domainBottom,std::int64_t(bounds->origin.y)+bounds->extent.height+margin);
    if(right<=left || bottom<=top || std::uint64_t(right-left)*std::uint64_t(bottom-top)>16ULL*1024*1024)
        return {{},"feather exceeds the working budget"};
    const CanvasRegion region{{std::int32_t(left),std::int32_t(top)},{int(right-left),int(bottom-top)}};
    try {
        auto input=materialize(region);if(!input)return {{},input.error()};
        auto result=featherMask(*input,radius);if(!result.ok())return {{},result.error};
        return fromRaster(state_->domain,region.origin,result.mask,state_->background,limits);
    } catch(const std::bad_alloc&) {return {{},"insufficient memory for tiled feather"};}
}
std::uint8_t TiledMask::coverage(std::int32_t x,std::int32_t y) const noexcept {
    if(!state_) return 0;
    const auto lx=std::int64_t(x)-state_->domain.origin.x,ly=std::int64_t(y)-state_->domain.origin.y;
    if(lx<0 || ly<0 || lx>=state_->domain.extent.width || ly>=state_->domain.extent.height) return 0;
    const Key key{std::int32_t(lx/side),std::int32_t(ly/side)};const auto it=state_->tiles.find(key);
    return at(it==state_->tiles.end()?nullptr:&it->second,state_->background,std::size_t(ly%side)*side+std::size_t(lx%side));
}
std::expected<RasterMask,std::string> TiledMask::materialize(CanvasRegion region) const {
    if(!state_ || !valid(region) || std::uint64_t(region.extent.width)*region.extent.height>64ULL*1024*1024)
        return std::unexpected("invalid or oversized mask region");
    try {
        RasterMask result{region.extent,std::vector<std::uint8_t>(std::size_t(region.extent.width)*region.extent.height)};
        for(int y=0;y<region.extent.height;++y)for(int x=0;x<region.extent.width;++x)
            result.alpha[std::size_t(y)*region.extent.width+x]=coverage(std::int32_t(std::int64_t(region.origin.x)+x),std::int32_t(std::int64_t(region.origin.y)+y));
        return result;
    } catch(const std::bad_alloc&) { return std::unexpected("insufficient memory for mask region"); }
}
CanvasRegion TiledMask::domain() const noexcept {return state_ ? state_->domain : CanvasRegion{};}
std::optional<CanvasRegion> TiledMask::nonzeroBounds() const noexcept {
    if (!state_) return std::nullopt;
    if (state_->background) return state_->domain;
    int left=state_->domain.extent.width,top=state_->domain.extent.height,right=0,bottom=0;
    for (const auto& [key,tile]:state_->tiles) {
        const int ox=key.first*side,oy=key.second*side;
        const int width=std::min(side,state_->domain.extent.width-ox),height=std::min(side,state_->domain.extent.height-oy);
        if (!tile.dense) {
            if (tile.uniform) {left=std::min(left,ox);top=std::min(top,oy);right=std::max(right,ox+width);bottom=std::max(bottom,oy+height);}
        } else for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            if ((*tile.dense)[std::size_t(y)*side+x]) {
                left=std::min(left,ox+x);top=std::min(top,oy+y);right=std::max(right,ox+x+1);bottom=std::max(bottom,oy+y+1);
            }
        }
    }
    if (left>=right || top>=bottom) return std::nullopt;
    return CanvasRegion{{std::int32_t(std::int64_t(state_->domain.origin.x)+left),
                         std::int32_t(std::int64_t(state_->domain.origin.y)+top)}, {right-left,bottom-top}};
}
std::expected<std::vector<CanvasRegion>, std::string> TiledMask::nonzeroRegions(std::size_t maxRegions) const {
    if (!state_) return std::unexpected("invalid tiled mask");
    const auto width=std::int64_t(state_->domain.extent.width),height=std::int64_t(state_->domain.extent.height);
    const auto columns=(width+side-1)/side,rows=(height+side-1)/side;
    const auto count=state_->background ? std::uint64_t(columns)*std::uint64_t(rows) : std::uint64_t(state_->tiles.size());
    if(count>maxRegions || count>std::vector<CanvasRegion>{}.max_size()) return std::unexpected("mask regions exceed limit");
    try {
        std::vector<CanvasRegion> result;result.reserve(std::size_t(count));
        const auto append=[&](std::int64_t x,std::int64_t y) {
            const auto ox=x*side,oy=y*side;
            result.push_back({{std::int32_t(std::int64_t(state_->domain.origin.x)+ox),
                               std::int32_t(std::int64_t(state_->domain.origin.y)+oy)},
                              {int(std::min<std::int64_t>(side,width-ox)),int(std::min<std::int64_t>(side,height-oy))}});
        };
        if(state_->background) for(std::int64_t y=0;y<rows;++y) for(std::int64_t x=0;x<columns;++x) append(x,y);
        else for(const auto& [key,tile]:state_->tiles) { (void)tile; append(key.first,key.second); }
        return result;
    } catch(const std::bad_alloc&) {return std::unexpected("insufficient memory for mask regions");}
}
std::size_t TiledMask::tile_count() const noexcept {return state_ ? state_->tiles.size() : 0;}
std::size_t TiledMask::dense_bytes() const noexcept {return state_ ? state_->denseBytes : 0;}
std::size_t TiledMask::storage_bytes() const noexcept {
    return state_ ? sizeof(State)+state_->tiles.size()*(sizeof(Key)+sizeof(Tile)+4*sizeof(void*))+state_->denseBytes : 0;
}
bool TiledMask::equivalent(const TiledMask& other) const noexcept {
    if(state_==other.state_) return bool(state_);
    if(!state_ || !other.state_ || state_->domain!=other.state_->domain) return false;
    const auto columns=(std::uint64_t(state_->domain.extent.width)+side-1)/side;
    const auto rows=(std::uint64_t(state_->domain.extent.height)+side-1)/side;
    std::uint64_t unionCount=0;
    auto a=state_->tiles.begin(),b=other.state_->tiles.begin();
    while(a!=state_->tiles.end() || b!=other.state_->tiles.end()) {
        Key key;
        if(b==other.state_->tiles.end() || (a!=state_->tiles.end() && a->first<b->first)) key=a->first;
        else key=b->first;
        const Tile* ta=nullptr;const Tile* tb=nullptr;
        if(a!=state_->tiles.end() && a->first==key){ta=&a->second;++a;}
        if(b!=other.state_->tiles.end() && b->first==key){tb=&b->second;++b;}
        ++unionCount;
        if ((!ta || !ta->dense) && (!tb || !tb->dense)) {
            if ((ta ? ta->uniform : state_->background)!=(tb ? tb->uniform : other.state_->background)) return false;
            continue;
        }
        const int ox=key.first*side,oy=key.second*side;
        const int width=std::min(side,state_->domain.extent.width-ox),height=std::min(side,state_->domain.extent.height-oy);
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){const auto i=std::size_t(y)*side+x;if(at(ta,state_->background,i)!=at(tb,other.state_->background,i))return false;}
    }
    return state_->background==other.state_->background || unionCount==columns*rows;
}
} // namespace iiSharedCanvas
