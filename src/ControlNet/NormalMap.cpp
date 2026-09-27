#include "ControlNet/NormalMap.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace iiSharedCanvas {
namespace {
bool validOptions(NormalMapRenderOptions options) noexcept {
    return options.channelOrder == NormalMapChannelOrder::Xyz
        || options.channelOrder == NormalMapChannelOrder::Zyx;
}
}
NormalMapAsset *findNormalMapAsset(Document &doc, const std::string &id) noexcept {
    auto *asset=findAsset(doc,id); return asset ? std::get_if<NormalMapAsset>(asset) : nullptr;
}
const NormalMapAsset *findNormalMapAsset(const Document &doc, const std::string &id) noexcept {
    const auto *asset=findAsset(doc,id); return asset ? std::get_if<NormalMapAsset>(asset) : nullptr;
}
NormalMapLayer *findNormalMapLayer(Document &doc, const std::string &id) noexcept {
    auto *layer=findLayer(doc,id); return layer ? std::get_if<NormalMapLayer>(layer) : nullptr;
}
const NormalMapLayer *findNormalMapLayer(const Document &doc, const std::string &id) noexcept {
    const auto *layer=findLayer(doc,id); return layer ? std::get_if<NormalMapLayer>(layer) : nullptr;
}
std::string validateNormalMapAsset(const NormalMapAsset &asset) {
    if (asset.viewport.width<=0 || asset.viewport.height<=0
        || std::uint64_t(asset.viewport.width)*asset.viewport.height!=asset.samples.size())
        return "normal map dimensions must be positive and match the dense sample count";
    for (const auto &s:asset.samples) {
        if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z)
            || std::abs(s.x)>1 || std::abs(s.y)>1 || std::abs(s.z)>1)
            return "normal components must be finite within [-1,1]";
        if (!s.valid) {
            if (s.x!=0 || s.y!=0 || s.z!=0) return "missing normal samples must have zero components";
        } else if (std::abs(std::sqrt(s.x*s.x+s.y*s.y+s.z*s.z)-1.0)>1e-6) {
            return "valid normal samples must be unit vectors within length tolerance 1e-6";
        }
    }
    return {};
}
RasterLayer normalMapRasterPreview(const NormalMapAsset &asset, NormalMapRenderOptions options) {
    if (!validOptions(options)) throw std::invalid_argument("unknown normal map channel order");
    const auto error=validateNormalMapAsset(asset);
    if (!error.empty()) throw std::invalid_argument(error);
    if (asset.samples.size()>options.maximumPixels) throw std::length_error("normal map output exceeds maximumPixels");
    auto result=makeRasterLayer(asset.viewport.width,asset.viewport.height,0xff000000);
    const auto channel=[](double value) { return static_cast<std::uint32_t>(std::lround((value+1.0)*127.5)); };
    for (std::size_t i=0;i<asset.samples.size();++i) {
        const auto &s=asset.samples[i];
        if (!s.valid) continue;
        const auto x=channel(s.x), y=channel(options.flipY ? -s.y : s.y), z=channel(s.z);
        const auto r=options.channelOrder==NormalMapChannelOrder::Xyz ? x : z;
        const auto b=options.channelOrder==NormalMapChannelOrder::Xyz ? z : x;
        result.pixels[i]=0xff000000U | (r<<16) | (y<<8) | b;
    }
    return result;
}
NormalMapControlMapResult renderNormalMapControlMap(const Document &doc, const std::string &id,
    std::uint32_t frame, NormalMapRenderOptions options) {
    const auto fail=[](std::string message) { NormalMapControlMapResult r; r.message=std::move(message); return r; };
    if (!validOptions(options)) return fail("unknown normal map channel order");
    const auto validation=validate(doc);
    if (!validation.ok()) return fail(validation.issues.front().path+": "+validation.issues.front().message);
    const auto *base=findLayer(doc,id); const auto *layer=base ? std::get_if<NormalMapLayer>(base) : nullptr;
    if (!layer) return fail("normal map layer not found");
    if (!layer->control.enabled) return fail("ControlNet layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame);
    const auto *asset=source ? std::get_if<NormalMapAsset>(source) : nullptr;
    if (!asset) return fail("normal map source is outside its frame range or timeline");
    if (asset->samples.size()>options.maximumPixels) return fail("normal map output exceeds maximumPixels");
    NormalMapControlMapResult result;
    result.pixels=normalMapRasterPreview(*asset,options); result.samples=asset->samples;
    result.validPixels.reserve(asset->samples.size());
    for (const auto &sample:asset->samples) result.validPixels.push_back(sample.valid ? 1 : 0);
    return result;
}
} // namespace iiSharedCanvas
