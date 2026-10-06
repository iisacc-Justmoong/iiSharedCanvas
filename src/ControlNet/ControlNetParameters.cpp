#include "ControlNet/ControlNetParameters.h"
#include "Validation/Validation.h"
#include <unordered_set>

namespace iiSharedCanvas {
ControlNetSettings *controlNetSettings(Layer &layer) noexcept {
    return std::visit([](auto &value)->ControlNetSettings * {
        if constexpr(requires { value.control; }) return &value.control;
        else return nullptr;
    },layer);
}
const ControlNetSettings *controlNetSettings(const Layer &layer) noexcept {
    return std::visit([](const auto &value)->const ControlNetSettings * {
        if constexpr(requires { value.control; }) return &value.control;
        else return nullptr;
    },layer);
}
ControlNetParametersResult getControlNetParameters(const Document &doc,const std::string &id) {
    const auto validity=validate(doc);
    if(!validity.ok()) return {{},validity.issues.front().path+": "+validity.issues.front().message};
    const auto *layer=findLayer(doc,id);
    if(!layer || !controlNetSettings(*layer)) return {{},"ControlNet layer was not found"};
    ControlNetParameters result; result.layer=*layer;
    std::unordered_set<std::string> seen;
    const auto append=[&](const std::string &assetId) {
        if(seen.insert(assetId).second) result.assets.push_back(*findAsset(doc,assetId));
    };
    if(const auto *source=staticLayerSource(*layer)) append(source->assetId);
    else for(auto frame:keyframedLayerSource(*layer)->frameIndices)
        append(findKeyframe(doc,id,frame)->assetId);
    return {std::move(result),{}};
}
} // namespace iiSharedCanvas
