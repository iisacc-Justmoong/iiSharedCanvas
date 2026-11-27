#include "Render/LayerStyleEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace iiSharedCanvas {
namespace {
// A normalized separable Gaussian with transparent samples outside the input.
std::vector<double> blur(const std::vector<double> &source,int width,int height,double sigmaX,double sigmaY)
{
    const auto pass=[&](const std::vector<double> &input,double sigma,bool horizontal) {
        if (sigma<=0.001) return input;
        const int radius=static_cast<int>(std::ceil(3*sigma));
        std::vector<double> kernel(2*radius+1); double sum=0;
        for (int d=-radius;d<=radius;++d) { auto &v=kernel[d+radius]; v=std::exp(-.5*d*d/(sigma*sigma)); sum+=v; }
        for (auto &v:kernel) v/=sum;
        std::vector<double> out(input.size());
        for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
            double value=0;
            for (int d=-radius;d<=radius;++d) {
                const int sx=x+(horizontal?d:0),sy=y+(horizontal?0:d);
                if (sx>=0 && sx<width && sy>=0 && sy<height) value+=input[std::size_t(sy)*width+sx]*kernel[d+radius];
            }
            out[std::size_t(y)*width+x]=value;
        }
        return out;
    };
    return pass(pass(source,sigmaX,true),sigmaY,false);
}

// Straight-ARGB source-over, with deterministic byte rounding. This is local
// pixel arithmetic; no executable dependency or external state is retained.
std::uint32_t sourceOver(std::uint32_t source,std::uint32_t backdrop,double opacity)
{
    const double sourceAlpha=double(source>>24)/255.*opacity;
    const double backdropAlpha=double(backdrop>>24)/255.;
    const double outputAlpha=sourceAlpha+backdropAlpha*(1-sourceAlpha);
    const auto alpha=static_cast<std::uint32_t>(std::lround(outputAlpha*255.));
    if (!alpha || outputAlpha<=0) return 0;
    const auto channel=[&](unsigned shift) {
        const double src=double((source>>shift)&255U)/255.;
        const double dst=double((backdrop>>shift)&255U)/255.;
        const double blended=(sourceAlpha*src+backdropAlpha*(1-sourceAlpha)*dst)/outputAlpha;
        return static_cast<std::uint32_t>(std::clamp(std::lround(blended*255.),0L,255L));
    };
    return (alpha<<24)|(channel(16)<<16)|(channel(8)<<8)|channel(0);
}

std::uint32_t colored(std::uint32_t color,double coverage)
{
    const auto alpha=static_cast<std::uint32_t>(std::lround(std::clamp(coverage,0.,1.)*(color>>24)));
    return alpha ? (alpha<<24)|(color&0xffffffU) : 0;
}
} // namespace

RasterProcessResult LayerStyleEvaluator::operator()(const RasterLayer &source,
    const LayerStyleStack &styles,double fillOpacity,double scaleX,double scaleY) const
{
    if (auto error=validateLayerStyleStack(styles); !error.empty()) return {{},std::move(error)};
    if (source.width<=0 || source.height<=0 || std::uint64_t(source.width)*source.height!=source.pixels.size() ||
        source.pixels.size()>16777216 || !std::isfinite(fillOpacity) || fillOpacity<0 || fillOpacity>1 ||
        !std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX<=0 || scaleY<=0)
        return {{},"invalid style source, opacity or scale"};
    for (const auto &effect:styles.effects)
        if (effect.enabled && (effect.radius*scaleX>128 || effect.radius*scaleY>128 ||
            std::abs(effect.offsetX*scaleX)>1024 || std::abs(effect.offsetY*scaleY)>1024))
            return {{},"scaled style exceeds the evaluation budget"};
    double work=0;
    for (const auto &effect:styles.effects) if (effect.enabled) {
        const double rx=std::ceil(effect.radius*scaleX),ry=std::ceil(effect.radius*scaleY);
        if (effect.kind==LayerStyleKind::Stroke) work+=(2*rx+1)*(2*ry+1);
        else if (effect.kind==LayerStyleKind::Shadow || effect.kind==LayerStyleKind::Glow ||
                 effect.kind==LayerStyleKind::InnerShadow || effect.kind==LayerStyleKind::InnerGlow)
            work+=6*(rx+ry)+2;
        else work+=1;
    }
    if (work*source.pixels.size()>500000000.) return {{},"style computation exceeds the evaluation budget"};
    const int width=source.width,height=source.height;
    std::vector<double> alpha(source.pixels.size());
    for (std::size_t i=0;i<alpha.size();++i) alpha[i]=double(source.pixels[i]>>24)/255;
    auto result=makeRasterLayer(width,height,0);
    const auto over=[&](std::size_t i,std::uint32_t color,double amount) {
        result.pixels[i]=sourceOver(colored(color,amount),result.pixels[i],1);
    };
    // Exterior effects are painted beneath the fill, retaining stack order.
    for (const auto &effect:styles.effects) {
        if (!effect.enabled || (effect.kind!=LayerStyleKind::Shadow && effect.kind!=LayerStyleKind::Glow &&
                                effect.kind!=LayerStyleKind::Stroke)) continue;
        std::vector<double> field;
        if (effect.kind==LayerStyleKind::Stroke) {
            field.resize(alpha.size());
            const int rx=static_cast<int>(std::ceil(effect.radius*scaleX)),ry=static_cast<int>(std::ceil(effect.radius*scaleY));
            for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
                double largest=0;
                for (int dy=-ry;dy<=ry;++dy) for (int dx=-rx;dx<=rx;++dx) {
                    if ((rx && double(dx*dx)/(rx*rx)>1) || (ry && double(dy*dy)/(ry*ry)>1) ||
                        (rx && ry && double(dx*dx)/(rx*rx)+double(dy*dy)/(ry*ry)>1)) continue;
                    const int sx=x+dx,sy=y+dy;
                    if (sx>=0 && sx<width && sy>=0 && sy<height) largest=std::max(largest,alpha[std::size_t(sy)*width+sx]);
                }
                const auto i=std::size_t(y)*width+x; field[i]=std::max(0.,largest-alpha[i]);
            }
        } else field=blur(alpha,width,height,effect.radius*scaleX,effect.radius*scaleY);
        const int ox=static_cast<int>(std::lround(effect.offsetX*scaleX));
        const int oy=static_cast<int>(std::lround(effect.offsetY*scaleY));
        for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
            const int sx=x-ox,sy=y-oy; const auto i=std::size_t(y)*width+x;
            if (sx<0 || sx>=width || sy<0 || sy>=height) continue;
            double amount=field[std::size_t(sy)*width+sx];
            if (effect.kind==LayerStyleKind::Glow) amount*=1-alpha[i];
            over(i,effect.color,amount*effect.opacity);
        }
    }
    for (std::size_t i=0;i<alpha.size();++i)
        result.pixels[i]=sourceOver(source.pixels[i],result.pixels[i],fillOpacity);
    // Interior effects and color overlay are constrained by original coverage.
    for (const auto &effect:styles.effects) {
        if (!effect.enabled || effect.kind==LayerStyleKind::Shadow || effect.kind==LayerStyleKind::Glow ||
            effect.kind==LayerStyleKind::Stroke) continue;
        auto field=effect.kind==LayerStyleKind::InnerShadow || effect.kind==LayerStyleKind::InnerGlow
            ? blur(alpha,width,height,effect.radius*scaleX,effect.radius*scaleY) : alpha;
        const int ox=static_cast<int>(std::lround(effect.offsetX*scaleX)),oy=static_cast<int>(std::lround(effect.offsetY*scaleY));
        for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
            const auto i=std::size_t(y)*width+x; if (alpha[i]<=0) continue;
            double amount=alpha[i];
            if (effect.kind==LayerStyleKind::InnerShadow || effect.kind==LayerStyleKind::InnerGlow) {
                const int sx=x-ox,sy=y-oy;
                amount*=1-((sx>=0 && sx<width && sy>=0 && sy<height)?field[std::size_t(sy)*width+sx]:0);
            } else if (effect.kind==LayerStyleKind::Bevel) {
                const int sx=std::max(0,x-std::max(1,static_cast<int>(std::lround(effect.radius*scaleX))));
                const int sy=std::max(0,y-std::max(1,static_cast<int>(std::lround(effect.radius*scaleY))));
                amount*=std::clamp(alpha[i]-(alpha[std::size_t(y)*width+sx]+alpha[std::size_t(sy)*width+x])*.5,0.,1.);
            }
            // Interior styles change color within the source shape; they must
            // not accumulate alpha as another independent translucent layer.
            const auto tint=colored(effect.color,(amount/alpha[i])*effect.opacity);
            if (!(tint>>24)) continue;
            const auto previous=result.pixels[i];
            const auto shaded=sourceOver(tint,previous|0xff000000U,1);
            const auto styledAlpha=static_cast<std::uint32_t>(std::lround(alpha[i]*(tint>>24)));
            const auto outputAlpha=std::max(previous>>24,styledAlpha);
            result.pixels[i]=outputAlpha ? (outputAlpha<<24)|(shaded&0x00ffffffU) : 0;
        }
    }
    return {std::move(result),{}};
}
} // namespace iiSharedCanvas
