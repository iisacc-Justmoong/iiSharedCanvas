#include "ControlNet/Scribble.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include "ControlNet/BinaryLineMask.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace iiSharedCanvas {
ScribbleAsset *findScribbleAsset(Document &doc, const std::string &id) noexcept {
    auto *asset=findAsset(doc,id); return asset ? std::get_if<ScribbleAsset>(asset) : nullptr;
}
const ScribbleAsset *findScribbleAsset(const Document &doc, const std::string &id) noexcept {
    const auto *asset=findAsset(doc,id); return asset ? std::get_if<ScribbleAsset>(asset) : nullptr;
}
ScribbleLayer *findScribbleLayer(Document &doc, const std::string &id) noexcept {
    auto *layer=findLayer(doc,id); return layer ? std::get_if<ScribbleLayer>(layer) : nullptr;
}
const ScribbleLayer *findScribbleLayer(const Document &doc, const std::string &id) noexcept {
    const auto *layer=findLayer(doc,id); return layer ? std::get_if<ScribbleLayer>(layer) : nullptr;
}
std::string validateScribbleAsset(const ScribbleAsset &asset) {
    return detail::validateBinaryLineMask(asset.viewport.width,asset.viewport.height,asset.mask);
}
RasterLayer scribbleRasterPreview(const ScribbleAsset &asset, std::uint64_t maxPixels) {
    return detail::binaryLinePreview(asset.viewport.width,asset.viewport.height,asset.mask,maxPixels);
}
ScribbleControlMapResult renderScribbleControlMap(const Document &doc, const std::string &id,
    std::uint32_t frame, std::uint64_t maxPixels) {
    const auto fail=[](std::string message){ScribbleControlMapResult r; r.message=std::move(message); return r;};
    const auto validation=validate(doc);
    if(!validation.ok()) return fail(validation.issues.front().path+": "+validation.issues.front().message);
    const auto *base=findLayer(doc,id); const auto *layer=base ? std::get_if<ScribbleLayer>(base) : nullptr;
    if(!layer) return fail("scribble layer not found");
    if(!layer->control.enabled) return fail("ControlNet layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame);
    const auto *asset=source ? std::get_if<ScribbleAsset>(source) : nullptr;
    if(!asset) return fail("scribble source is outside its frame range or timeline");
    if(asset->mask.size()>maxPixels) return fail("scribble output exceeds maxPixels");
    ScribbleControlMapResult result;
    result.pixels=scribbleRasterPreview(*asset,maxPixels); result.mask=asset->mask;
    return result;
}
} // namespace iiSharedCanvas
