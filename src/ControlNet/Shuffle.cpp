#include "ControlNet/Shuffle.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace iiSharedCanvas {
ShuffleAsset *findShuffleAsset(Document &doc, const std::string &id) noexcept {
    auto *asset=findAsset(doc,id); return asset ? std::get_if<ShuffleAsset>(asset) : nullptr;
}
const ShuffleAsset *findShuffleAsset(const Document &doc, const std::string &id) noexcept {
    const auto *asset=findAsset(doc,id); return asset ? std::get_if<ShuffleAsset>(asset) : nullptr;
}
ShuffleLayer *findShuffleLayer(Document &doc, const std::string &id) noexcept {
    auto *layer=findLayer(doc,id); return layer ? std::get_if<ShuffleLayer>(layer) : nullptr;
}
const ShuffleLayer *findShuffleLayer(const Document &doc, const std::string &id) noexcept {
    const auto *layer=findLayer(doc,id); return layer ? std::get_if<ShuffleLayer>(layer) : nullptr;
}
std::string validateShuffleAsset(const ShuffleAsset &asset) {
    if(asset.viewport.width<=0 || asset.viewport.height<=0
        || std::uint64_t(asset.viewport.width)*asset.viewport.height!=asset.colors.size())
        return "shuffle dimensions must be positive and match the dense sample count";
    return {};
}
RasterLayer shuffleRasterPreview(const ShuffleAsset &asset, std::uint64_t maxPixels) {
    const auto error=validateShuffleAsset(asset);
    if(!error.empty()) throw std::invalid_argument(error);
    if(asset.colors.size()>maxPixels) throw std::length_error("shuffle output exceeds maxPixels");
    auto result=makeRasterLayer(asset.viewport.width,asset.viewport.height,0xff000000);
    for(std::size_t i=0;i<asset.colors.size();++i) {
        const auto &color=asset.colors[i];
        result.pixels[i]=0xff000000U | (std::uint32_t(color.red)<<16) | (std::uint32_t(color.green)<<8) | color.blue;
    }
    return result;
}
ShuffleControlMapResult renderShuffleControlMap(const Document &doc, const std::string &id,
    std::uint32_t frame, std::uint64_t maxPixels) {
    const auto fail=[](std::string message){ShuffleControlMapResult r; r.message=std::move(message); return r;};
    const auto validation=validate(doc);
    if(!validation.ok()) return fail(validation.issues.front().path+": "+validation.issues.front().message);
    const auto *base=findLayer(doc,id); const auto *layer=base ? std::get_if<ShuffleLayer>(base) : nullptr;
    if(!layer) return fail("shuffle layer not found");
    if(!layer->control.enabled) return fail("ControlNet layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame);
    const auto *asset=source ? std::get_if<ShuffleAsset>(source) : nullptr;
    if(!asset) return fail("shuffle source is outside its frame range or timeline");
    if(asset->colors.size()>maxPixels) return fail("shuffle output exceeds maxPixels");
    ShuffleControlMapResult result;
    result.pixels=shuffleRasterPreview(*asset,maxPixels); result.colors=asset->colors;
    return result;
}
ShuffleAsset makeShuffleAsset(std::string id, const RasterLayer &source,
    CanvasExtent outputExtent, const std::vector<ShuffleCoordinate> &coordinates, std::uint64_t maxPixels) {
    if (id.empty()) throw std::invalid_argument("shuffle asset id must not be empty");
    if (outputExtent.width<=0 || outputExtent.height<=0
        || std::uint64_t(outputExtent.width)*outputExtent.height!=coordinates.size())
        throw std::invalid_argument("shuffle coordinates must match positive output dimensions");
    if (coordinates.size()>maxPixels) throw std::length_error("shuffle output exceeds maxPixels");
    if (source.width<=0 || source.height<=0
        || std::uint64_t(source.width)*source.height!=source.pixels.size())
        throw std::invalid_argument("shuffle source dimensions must match pixels");
    if (std::any_of(source.pixels.begin(),source.pixels.end(),[](std::uint32_t p){return (p>>24)!=255;}))
        throw std::invalid_argument("shuffle source must be opaque; composite transparency explicitly");
    for (const auto &uv:coordinates) {
        if (!std::isfinite(uv.u) || !std::isfinite(uv.v) || uv.u<0 || uv.u>1 || uv.v<0 || uv.v>1)
            throw std::invalid_argument("shuffle coordinates must be finite within [0,1]");
    }
    ShuffleAsset result; result.id=std::move(id); result.viewport=outputExtent;
    result.colors.reserve(coordinates.size());
    for (const auto &uv:coordinates) {
        const double x=uv.u*(source.width-1), y=uv.v*(source.height-1);
        const auto x0=static_cast<std::int32_t>(std::floor(x)), y0=static_cast<std::int32_t>(std::floor(y));
        const auto x1=std::min(x0+1,source.width-1), y1=std::min(y0+1,source.height-1);
        const double tx=x-x0, ty=y-y0;
        const auto channel=[&](unsigned shift) {
            const auto get=[&](std::int32_t px,std::int32_t py) {
                return double((source.pixels[std::size_t(py)*source.width+px]>>shift)&255U);
            };
            const double top=get(x0,y0)*(1-tx)+get(x1,y0)*tx;
            const double bottom=get(x0,y1)*(1-tx)+get(x1,y1)*tx;
            return static_cast<std::uint8_t>(std::clamp(std::lround(top*(1-ty)+bottom*ty),0L,255L));
        };
        result.colors.push_back({channel(16),channel(8),channel(0)});
    }
    return result;
}
} // namespace iiSharedCanvas
