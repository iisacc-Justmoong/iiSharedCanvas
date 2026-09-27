#include "ControlNet/Canny.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include "ControlNet/BinaryLineMask.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace iiSharedCanvas {
CannyAsset *findCannyAsset(Document &doc, const std::string &id) noexcept {
    auto *asset=findAsset(doc,id); return asset ? std::get_if<CannyAsset>(asset) : nullptr;
}
const CannyAsset *findCannyAsset(const Document &doc, const std::string &id) noexcept {
    const auto *asset=findAsset(doc,id); return asset ? std::get_if<CannyAsset>(asset) : nullptr;
}
CannyLayer *findCannyLayer(Document &doc, const std::string &id) noexcept {
    auto *layer=findLayer(doc,id); return layer ? std::get_if<CannyLayer>(layer) : nullptr;
}
const CannyLayer *findCannyLayer(const Document &doc, const std::string &id) noexcept {
    const auto *layer=findLayer(doc,id); return layer ? std::get_if<CannyLayer>(layer) : nullptr;
}
std::string validateCannyAsset(const CannyAsset &asset) {
    return detail::validateBinaryLineMask(asset.viewport.width,asset.viewport.height,asset.mask);
}
RasterLayer cannyRasterPreview(const CannyAsset &asset, std::uint64_t maxPixels) {
    return detail::binaryLinePreview(asset.viewport.width,asset.viewport.height,asset.mask,maxPixels);
}
CannyControlMapResult renderCannyControlMap(const Document &doc, const std::string &id,
    std::uint32_t frame, std::uint64_t maxPixels) {
    const auto fail=[](std::string message){CannyControlMapResult r; r.message=std::move(message); return r;};
    const auto validation=validate(doc);
    if(!validation.ok()) return fail(validation.issues.front().path+": "+validation.issues.front().message);
    const auto *base=findLayer(doc,id); const auto *layer=base ? std::get_if<CannyLayer>(base) : nullptr;
    if(!layer) return fail("canny layer not found");
    if(!layer->control.enabled) return fail("ControlNet layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame);
    const auto *asset=source ? std::get_if<CannyAsset>(source) : nullptr;
    if(!asset) return fail("canny source is outside its frame range or timeline");
    if(asset->mask.size()>maxPixels) return fail("canny output exceeds maxPixels");
    CannyControlMapResult result;
    result.pixels=cannyRasterPreview(*asset,maxPixels); result.mask=asset->mask;
    return result;
}
} // namespace iiSharedCanvas
