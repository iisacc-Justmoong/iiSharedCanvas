#include "ControlNet/Reference.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace iiSharedCanvas {
ReferenceAsset *findReferenceAsset(Document &doc, const std::string &id) noexcept {
    auto *asset=findAsset(doc,id); return asset ? std::get_if<ReferenceAsset>(asset) : nullptr;
}
const ReferenceAsset *findReferenceAsset(const Document &doc, const std::string &id) noexcept {
    const auto *asset=findAsset(doc,id); return asset ? std::get_if<ReferenceAsset>(asset) : nullptr;
}
ReferenceLayer *findReferenceLayer(Document &doc, const std::string &id) noexcept {
    auto *layer=findLayer(doc,id); return layer ? std::get_if<ReferenceLayer>(layer) : nullptr;
}
const ReferenceLayer *findReferenceLayer(const Document &doc, const std::string &id) noexcept {
    const auto *layer=findLayer(doc,id); return layer ? std::get_if<ReferenceLayer>(layer) : nullptr;
}
std::string validateReferenceSettings(const ReferenceSettings &settings) {
    if (settings.mode!=ReferenceMode::Attention && settings.mode!=ReferenceMode::AdaIN
        && settings.mode!=ReferenceMode::AttentionAdaIN) return "unknown reference mode";
    if (!std::isfinite(settings.styleFidelity) || settings.styleFidelity<0 || settings.styleFidelity>1)
        return "reference style fidelity must be finite within [0,1]";
    return {};
}
std::string validateReferenceAsset(const ReferenceAsset &asset) {
    if(asset.viewport.width<=0 || asset.viewport.height<=0
        || std::uint64_t(asset.viewport.width)*asset.viewport.height!=asset.colors.size())
        return "reference dimensions must be positive and match the dense sample count";
    return {};
}
RasterLayer referenceRasterPreview(const ReferenceAsset &asset, std::uint64_t maxPixels) {
    const auto error=validateReferenceAsset(asset);
    if(!error.empty()) throw std::invalid_argument(error);
    if(asset.colors.size()>maxPixels) throw std::length_error("reference output exceeds maxPixels");
    auto result=makeRasterLayer(asset.viewport.width,asset.viewport.height,0xff000000);
    for(std::size_t i=0;i<asset.colors.size();++i) {
        const auto &color=asset.colors[i];
        result.pixels[i]=0xff000000U | (std::uint32_t(color.red)<<16) | (std::uint32_t(color.green)<<8) | color.blue;
    }
    return result;
}
ReferenceControlMapResult renderReferenceControlMap(const Document &doc, const std::string &id,
    std::uint32_t frame, std::uint64_t maxPixels) {
    const auto fail=[](std::string message){ReferenceControlMapResult r; r.message=std::move(message); return r;};
    const auto validation=validate(doc);
    if(!validation.ok()) return fail(validation.issues.front().path+": "+validation.issues.front().message);
    const auto *base=findLayer(doc,id); const auto *layer=base ? std::get_if<ReferenceLayer>(base) : nullptr;
    if(!layer) return fail("reference layer not found");
    if(!layer->control.enabled) return fail("ControlNet layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame);
    const auto *asset=source ? std::get_if<ReferenceAsset>(source) : nullptr;
    if(!asset) return fail("reference source is outside its frame range or timeline");
    if(asset->colors.size()>maxPixels) return fail("reference output exceeds maxPixels");
    ReferenceControlMapResult result;
    result.pixels=referenceRasterPreview(*asset,maxPixels); result.colors=asset->colors;
    result.reference=layer->reference; result.control=layer->control;
    return result;
}
ReferenceAsset makeReferenceAsset(std::string id, const RasterLayer &source, std::uint64_t maxPixels) {
    if (id.empty()) throw std::invalid_argument("reference asset id must not be empty");
    if (source.width<=0 || source.height<=0
        || std::uint64_t(source.width)*source.height!=source.pixels.size())
        throw std::invalid_argument("reference source dimensions must match positive pixel count");
    if (source.pixels.size()>maxPixels) throw std::length_error("reference output exceeds maxPixels");
    if (std::any_of(source.pixels.begin(),source.pixels.end(),[](std::uint32_t p){return (p>>24)!=255;}))
        throw std::invalid_argument("reference source must be opaque; composite transparency explicitly");
    ReferenceAsset result; result.id=std::move(id); result.viewport={source.width,source.height};
    result.colors.reserve(source.pixels.size());
    for (auto pixel:source.pixels) result.colors.push_back({static_cast<std::uint8_t>((pixel>>16)&255),
        static_cast<std::uint8_t>((pixel>>8)&255),static_cast<std::uint8_t>(pixel&255)});
    return result;
}
} // namespace iiSharedCanvas
