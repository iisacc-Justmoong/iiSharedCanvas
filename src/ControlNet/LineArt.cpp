#include "ControlNet/LineArt.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace iiSharedCanvas {
LineArtAsset *findLineArtAsset(Document &doc, const std::string &id) noexcept {
    auto *asset=findAsset(doc,id); return asset ? std::get_if<LineArtAsset>(asset) : nullptr;
}
const LineArtAsset *findLineArtAsset(const Document &doc, const std::string &id) noexcept {
    const auto *asset=findAsset(doc,id); return asset ? std::get_if<LineArtAsset>(asset) : nullptr;
}
LineArtLayer *findLineArtLayer(Document &doc, const std::string &id) noexcept {
    auto *layer=findLayer(doc,id); return layer ? std::get_if<LineArtLayer>(layer) : nullptr;
}
const LineArtLayer *findLineArtLayer(const Document &doc, const std::string &id) noexcept {
    const auto *layer=findLayer(doc,id); return layer ? std::get_if<LineArtLayer>(layer) : nullptr;
}
std::string validateLineArtAsset(const LineArtAsset &asset) {
    if(asset.viewport.width<=0 || asset.viewport.height<=0
        || std::uint64_t(asset.viewport.width)*asset.viewport.height!=asset.coverage.size())
        return "lineArt dimensions must be positive and match the dense sample count";
    if(std::any_of(asset.coverage.begin(),asset.coverage.end(),[](double v){return !std::isfinite(v) || v<0 || v>1;}))
        return "lineArt samples must be finite within [0,1] (zero is white background, one is full black ink)";
    return {};
}
RasterLayer lineArtRasterPreview(const LineArtAsset &asset, std::uint64_t maxPixels) {
    const auto error=validateLineArtAsset(asset);
    if(!error.empty()) throw std::invalid_argument(error);
    if(asset.coverage.size()>maxPixels) throw std::length_error("lineArt output exceeds maxPixels");
    auto result=makeRasterLayer(asset.viewport.width,asset.viewport.height,0xff000000);
    for(std::size_t i=0;i<asset.coverage.size();++i) {
        const auto gray=static_cast<std::uint32_t>(std::lround((1.0-asset.coverage[i])*255));
        result.pixels[i]=0xff000000U | (gray<<16) | (gray<<8) | gray;
    }
    return result;
}
LineArtControlMapResult renderLineArtControlMap(const Document &doc, const std::string &id,
    std::uint32_t frame, std::uint64_t maxPixels) {
    const auto fail=[](std::string message){LineArtControlMapResult r; r.message=std::move(message); return r;};
    const auto validation=validate(doc);
    if(!validation.ok()) return fail(validation.issues.front().path+": "+validation.issues.front().message);
    const auto *base=findLayer(doc,id); const auto *layer=base ? std::get_if<LineArtLayer>(base) : nullptr;
    if(!layer) return fail("lineArt layer not found");
    if(!layer->control.enabled) return fail("ControlNet layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame);
    const auto *asset=source ? std::get_if<LineArtAsset>(source) : nullptr;
    if(!asset) return fail("lineArt source is outside its frame range or timeline");
    if(asset->coverage.size()>maxPixels) return fail("lineArt output exceeds maxPixels");
    LineArtControlMapResult result;
    result.pixels=lineArtRasterPreview(*asset,maxPixels); result.coverage=asset->coverage;
    result.inkPixels.reserve(asset->coverage.size());
    for(double value:asset->coverage) result.inkPixels.push_back(value>0 ? 1 : 0);
    return result;
}
} // namespace iiSharedCanvas
