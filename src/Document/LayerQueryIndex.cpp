#include "Document/LayerQueryIndex.h"
#include "Document/CanvasSampling.h"
#include "Validation/Validation.h"

namespace iiSharedCanvas {
LayerQueryIndex::LayerQueryIndex(const Document &document) { (void)refresh(document); }
LayerQueryResult LayerQueryIndex::refresh(const Document &document)
{
    const auto validation=validate(document);
    if (!validation.ok()) return {{},validation.issues.front().message};
    std::vector<std::string> next; next.reserve(document.layers.size());
    for (const auto &layer:document.layers) next.push_back(layerProperties(layer).id);
    owner_=&document; revision_=document.authorship.revision(); order_=std::move(next); return {};
}
LayerQueryResult LayerQueryIndex::query(const Document &document,const LayerQuery &query) const
{
    if (owner_!=&document || revision_!=document.authorship.revision()) return {{},"layer index is stale"};
    if (query.frame && *query.frame>=document.timeline.frameCount) return {{},"frame is out of range"};
    LayerQueryResult result;
    if (document.layers.size()!=order_.size()) return {{},"layer index is stale"};
    // Captured document order gives direct access. Looking up every ID again
    // would turn one query into quadratic work on large layer stacks.
    for (std::size_t position=0;position<order_.size();++position) {
        const auto &id=order_[position];
        const auto *layer=&document.layers[position];
        if (layerProperties(*layer).id!=id) return {{},"layer index is stale"};
        const auto &p=layerProperties(*layer);
        if (!query.nameContains.empty() && p.name.find(query.nameContains)==std::string::npos) continue;
        if (query.role && layerRole(*layer)!=*query.role) continue;
        if (query.groupId && document.relations.groups.groupOf(id)!=*query.groupId) continue;
        if (query.frame && !layerExistsAt(document,*layer,*query.frame)) continue;
        bool visible=sampleLayerAt(document,*layer,query.frame.value_or(0)).visible;
        auto group=document.relations.groups.groupOf(id);
        while (!group.empty()) {
            const auto *parent=document.relations.groups.find(group); if (!parent) return {{},"layer group is invalid"};
            visible=visible && parent->visible; group=parent->parentId;
        }
        if (query.visible && visible!=*query.visible) continue;
        result.layerIds.push_back(id);
    }
    return result;
}
} // namespace iiSharedCanvas
