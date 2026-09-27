#include "ControlNet/IpAdapter.h"
#include "Document/Document.h"
#include "Validation/Validation.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace iiSharedCanvas {
static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
bool IpAdapterTensor::operator==(const IpAdapterTensor &other) const {
    return tokenCount==other.tokenCount && channelCount==other.channelCount
        && std::equal(values.begin(),values.end(),other.values.begin(),other.values.end(),
            [](float a,float b){return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b);});
}
IpAdapterAsset *findIpAdapterAsset(Document &d,const std::string &id) noexcept {
    auto *a=findAsset(d,id); return a ? std::get_if<IpAdapterAsset>(a) : nullptr;
}
const IpAdapterAsset *findIpAdapterAsset(const Document &d,const std::string &id) noexcept {
    const auto *a=findAsset(d,id); return a ? std::get_if<IpAdapterAsset>(a) : nullptr;
}
IpAdapterLayer *findIpAdapterLayer(Document &d,const std::string &id) noexcept {
    auto *l=findLayer(d,id); return l ? std::get_if<IpAdapterLayer>(l) : nullptr;
}
const IpAdapterLayer *findIpAdapterLayer(const Document &d,const std::string &id) noexcept {
    const auto *l=findLayer(d,id); return l ? std::get_if<IpAdapterLayer>(l) : nullptr;
}
namespace {
bool sameShape(const IpAdapterTensor &a,const IpAdapterTensor &b) {
    return a.tokenCount==b.tokenCount && a.channelCount==b.channelCount;
}
std::string validateTensor(const IpAdapterTensor &t) {
    if(t.tokenCount==0 || t.channelCount==0
        || std::uint64_t(t.tokenCount)*t.channelCount!=t.values.size())
        return "IP-Adapter tensor dimensions must be positive and match its value count";
    if(std::any_of(t.values.begin(),t.values.end(),[](float value){return !std::isfinite(value);}))
        return "IP-Adapter tensor values must be finite";
    return {};
}
}
std::string validateIpAdapterAsset(const IpAdapterAsset &a) {
    if(a.id.empty()) return "IP-Adapter asset id must not be empty";
    const auto &d=a.descriptor;
    if(d.stage!=IpAdapterEmbeddingStage::EncoderPooled && d.stage!=IpAdapterEmbeddingStage::EncoderHiddenStates
        && d.stage!=IpAdapterEmbeddingStage::ProjectedTokens) return "unknown IP-Adapter embedding stage";
    for(const auto *text:{&d.encoderId,&d.encoderRevision,&d.adapterId,&d.adapterRevision,&d.baseModelId,&d.preprocessingId})
        if(text->empty()) return "IP-Adapter encoder, adapter, revisions, base model and preprocessing identity are required";
    if(auto error=validateTensor(a.conditional); !error.empty()) return error;
    if(d.stage==IpAdapterEmbeddingStage::EncoderPooled && a.conditional.tokenCount!=1)
        return "pooled encoder embeddings must contain exactly one token";
    if(a.unconditional) {
        if(auto error=validateTensor(*a.unconditional); !error.empty()) return error;
        if(!sameShape(a.conditional,*a.unconditional)) return "conditional and unconditional embedding shapes must match";
    }
    return {};
}
std::string validateIpAdapterLayerSources(const Document &doc,const IpAdapterLayer &layer) {
    if(layer.control.modelId.empty() || layer.control.modelRevision.empty())
        return "IP-Adapter layer requires an explicit adapter id and revision";
    const IpAdapterAsset *first=nullptr;
    const auto check=[&](const std::string &id)->std::string {
        const auto *a=findIpAdapterAsset(doc,id);
        if(!a) return "IP-Adapter layer source must reference an embedding asset";
        if(a->descriptor.adapterId!=layer.control.modelId || a->descriptor.adapterRevision!=layer.control.modelRevision)
            return "IP-Adapter layer and asset adapter identities must match";
        if(first && (first->descriptor!=a->descriptor || !sameShape(first->conditional,a->conditional)))
            return "IP-Adapter frame states must share their embedding descriptor and shape";
        first=a; return {};
    };
    if(const auto *s=std::get_if<StaticSource>(&layer.source)) return check(s->assetId);
    for(const auto &frame:doc.frames) {
        for(const auto &key:frame.keyframes) {
            if(key.layerId==layer.properties.id) if(auto error=check(key.assetId); !error.empty()) return error;
        }
    }
    return {};
}
IpAdapterEmbeddingResult exportIpAdapterEmbeddings(const Document &doc,const std::string &id,
    std::uint32_t frame,IpAdapterExportOptions options) {
    const auto fail=[](std::string message){IpAdapterEmbeddingResult r; r.message=std::move(message); return r;};
    const auto validity=validate(doc);
    if(!validity.ok()) return fail(validity.issues.front().path+": "+validity.issues.front().message);
    const auto *base=findLayer(doc,id); const auto *layer=base ? std::get_if<IpAdapterLayer>(base) : nullptr;
    if(!layer) return fail("IP-Adapter layer not found");
    if(!layer->control.enabled) return fail("IP-Adapter layer is disabled");
    const auto *source=resolveAssetAt(doc,*base,frame); const auto *a=source ? std::get_if<IpAdapterAsset>(source) : nullptr;
    if(!a) return fail("IP-Adapter source is outside its frame range or timeline");
    if(options.requireUnconditional && !a->unconditional) return fail("CFG requires an explicit unconditional embedding; it cannot be synthesized");
    const auto count=a->conditional.values.size();
    if(count>options.maximumValues || (a->unconditional && a->unconditional->values.size()>options.maximumValues-count))
        return fail("IP-Adapter output exceeds maximumValues");
    IpAdapterEmbeddingResult result; result.descriptor=a->descriptor; result.conditional=a->conditional;
    result.unconditional=a->unconditional; result.control=layer->control; return result;
}
} // namespace iiSharedCanvas
