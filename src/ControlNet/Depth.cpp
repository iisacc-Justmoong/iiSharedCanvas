#include "ControlNet/Depth.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace iiSharedCanvas {
DepthAsset *findDepthAsset(Document &doc, const std::string &id) noexcept {
    auto *asset=findAsset(doc,id); return asset ? std::get_if<DepthAsset>(asset) : nullptr;
}
const DepthAsset *findDepthAsset(const Document &doc, const std::string &id) noexcept {
    const auto *asset=findAsset(doc,id); return asset ? std::get_if<DepthAsset>(asset) : nullptr;
}
DepthLayer *findDepthLayer(Document &doc, const std::string &id) noexcept {
    auto *layer=findLayer(doc,id); return layer ? std::get_if<DepthLayer>(layer) : nullptr;
}
const DepthLayer *findDepthLayer(const Document &doc, const std::string &id) noexcept {
    const auto *layer=findLayer(doc,id); return layer ? std::get_if<DepthLayer>(layer) : nullptr;
}
std::string validateDepthAsset(const DepthAsset &asset) {
    if(asset.viewport.width<=0 || asset.viewport.height<=0
        || std::uint64_t(asset.viewport.width)*asset.viewport.height!=asset.values.size())
        return "depth dimensions must be positive and match the dense sample count";
    if(std::any_of(asset.values.begin(),asset.values.end(),[](double v){return !std::isfinite(v) || v<0 || v>1;}))
        return "depth samples must be finite within [0,1] (zero is empty, one is camera contact)";
    return {};
}
RasterLayer depthRasterPreview(const DepthAsset &asset, std::uint64_t maxPixels) {
    const auto error=validateDepthAsset(asset);
    if(!error.empty()) throw std::invalid_argument(error);
    if(asset.values.size()>maxPixels) throw std::length_error("depth output exceeds maxPixels");
    auto result=makeRasterLayer(asset.viewport.width,asset.viewport.height,0xff000000);
    for(std::size_t i=0;i<asset.values.size();++i) {
        const auto gray=static_cast<std::uint32_t>(std::lround(asset.values[i]*255));
        result.pixels[i]=0xff000000U | (gray<<16) | (gray<<8) | gray;
    }
    return result;
}
DepthControlMapResult renderDepthControlMap(const Document &doc, const std::string &id,
    std::uint32_t frame, std::uint64_t maxPixels) {
    const auto fail=[](std::string message){DepthControlMapResult r; r.message=std::move(message); return r;};
    const auto validation=validate(doc);
    if(!validation.ok()) return fail(validation.issues.front().path+": "+validation.issues.front().message);
    const auto *base=findLayer(doc,id); const auto *layer=base ? std::get_if<DepthLayer>(base) : nullptr;
    if(!layer) return fail("depth layer not found");
    if(!layer->control.enabled) return fail("ControlNet layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame);
    const auto *asset=source ? std::get_if<DepthAsset>(source) : nullptr;
    if(!asset) return fail("depth source is outside its frame range or timeline");
    if(asset->values.size()>maxPixels) return fail("depth output exceeds maxPixels");
    DepthControlMapResult result;
    result.pixels=depthRasterPreview(*asset,maxPixels); result.values=asset->values;
    result.occupiedPixels.reserve(asset->values.size());
    for(double value:asset->values) result.occupiedPixels.push_back(value>0 ? 1 : 0);
    return result;
}
} // namespace iiSharedCanvas
