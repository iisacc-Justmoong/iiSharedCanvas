#include "Document/LayerTransactions.h"
#include "Document/LayerTransactions_p.hpp"
#include "Validation/Validation.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>

namespace iiSharedCanvas {
namespace {
// Verify that a detached render corresponds to the complete native canvas.
bool validFrame(const FrameRenderResult &frame,CanvasRegion region)
{
    return frame.ok() && frame.origin==region.origin &&
        frame.pixels.width==region.extent.width && frame.pixels.height==region.extent.height &&
        region.extent.width>0 && region.extent.height>0 &&
        frame.pixels.pixels.size()==std::uint64_t(region.extent.width)*std::uint64_t(region.extent.height);
}
bool sameFrame(const FrameRenderResult &a,const FrameRenderResult &b)
{
    return a.ok() && b.ok() && a.origin==b.origin && a.pixels.width==b.pixels.width &&
        a.pixels.height==b.pixels.height && a.pixels.pixels==b.pixels.pixels;
}

// Build a value-only candidate. Source asset ownership remains with the
// document; only generated output frames are copied into the returned plan.
LayerTransactionResult prepare(const Document &document,const std::vector<std::string> &removed,
    std::span<const FrameRenderResult> frames,std::string layerId,std::string prefix,
    bool flatten,std::size_t insertAt,std::string group,std::optional<std::string> artboard)
{
    const auto region=canvasRegion(document);
    if (documentViewRegion(document)!=region)
        return {{},"the complete document view must fit the current canvas region"};
    if (layerId.empty() || prefix.empty() || findLayer(document,layerId) ||
        frames.size()!=document.timeline.frameCount || frames.empty())
        return {{},"invalid output identifier or incomplete frame sequence"};
    for (const auto &frame:frames) if (!validFrame(frame,region))
        return {{},"every frame must cover the native canvas at its original resolution"};
    LayerTransactionPlan plan; plan.sourceRevision=document.authorship.revision(); plan.flatten=flatten; plan.resultLayerId=layerId;
    plan.removedLayers=removed; plan.layers=document.layers; plan.frames=document.frames;
    plan.artboards=flatten ? std::vector<Artboard>{} : document.artboards;
    plan.relations=document.relations;
    const std::unordered_set<std::string_view> selected(removed.begin(),removed.end());
    for (const auto &id:removed) plan.relations.removeLayer(id);
    if (flatten) {
        SelectionLayerRelations relations; relations.selections=plan.relations.selections;
        plan.relations=std::move(relations);
    }
    plan.layers.erase(std::remove_if(plan.layers.begin(),plan.layers.end(),[&](const Layer &layer) {
        return selected.contains(layerProperties(layer).id);
    }),plan.layers.end());
    for (auto &frame:plan.frames)
        frame.keyframes.erase(std::remove_if(frame.keyframes.begin(),frame.keyframes.end(),[&](const Keyframe &key) {
            return selected.contains(key.layerId);
        }),frame.keyframes.end());
    plan.frames.erase(std::remove_if(plan.frames.begin(),plan.frames.end(),[](const Frame &f) { return f.keyframes.empty(); }),plan.frames.end());

    plan.removedAssets=detail::removableLayerAssets(document,removed);

    std::vector<FrameIndex> keyFrames; std::vector<std::string> assetIds;
    for (std::size_t i=0;i<frames.size();++i) {
        if (i && frames[i].pixels.pixels==frames[i-1].pixels.pixels) continue;
        const auto id=prefix+":"+std::to_string(i);
        if (findAsset(document,id) || findAudioAsset(document,id)) return {{},"generated asset ID already exists"};
        plan.addedAssets.emplace_back(RasterAsset{id,frames[i].pixels});
        keyFrames.push_back(static_cast<FrameIndex>(i)); assetIds.push_back(id);
    }
    LayerProperties properties; properties.id=layerId; properties.name=layerId; properties.artboardId=artboard;
    properties.transform.translationX=region.origin.x; properties.transform.translationY=region.origin.y;
    if (artboard) {
        const auto *board=findArtboard(document,*artboard);
        if (!board) return {{},"source artboard was not found"};
        properties.transform.translationX-=board->region.origin.x;
        properties.transform.translationY-=board->region.origin.y;
    }
    Layer replacement;
    if (keyFrames.size()==1) replacement=StaticBitmapLayer{properties,StaticSource{assetIds.front()}};
    else {
        DynamicBitmapContent content; content.frameIndices=keyFrames;
        replacement=DynamicBitmapLayer{properties,std::move(content)};
        for (std::size_t i=0;i<keyFrames.size();++i) {
            const auto at=std::lower_bound(plan.frames.begin(),plan.frames.end(),keyFrames[i],
                [](const Frame &frame,FrameIndex index) { return frame.index<index; });
            auto target=at;
            if (at==plan.frames.end() || at->index!=keyFrames[i]) target=plan.frames.insert(at,Frame{keyFrames[i],{}});
            const auto key=std::lower_bound(target->keyframes.begin(),target->keyframes.end(),layerId,
                [](const Keyframe &entry,const std::string &id) { return entry.layerId<id; });
            target->keyframes.insert(key,Keyframe{layerId,assetIds[i]});
        }
    }
    if (insertAt>plan.layers.size()) return {{},"replacement layer position is invalid"};
    plan.layers.insert(plan.layers.begin()+static_cast<std::ptrdiff_t>(insertAt),std::move(replacement));
    if (!group.empty() && !plan.relations.groups.assign(layerId,std::move(group)))
        return {{},"replacement group is invalid"};
    return {std::move(plan),{}};
}
} // namespace

std::vector<std::string> detail::removableLayerAssets(const Document &document,
    std::span<const std::string> removed)
{
    const std::unordered_set<std::string_view> selected(removed.begin(),removed.end());
    std::unordered_set<std::string> removable;
    for (const auto &asset:document.assets) {
        const auto &id=assetId(asset); const auto refs=assetReferences(document,id);
        if (!refs.empty() && std::all_of(refs.begin(),refs.end(),[&](const AssetReference &ref) {
                return ref.layerIndex<document.layers.size() && selected.contains(layerProperties(document.layers[ref.layerIndex]).id);
            })) removable.insert(id);
    }
    for (const auto &asset:document.assets) if (const auto *fill=std::get_if<FillLayerContent>(&asset))
        if (!removable.contains(fill->id) && !fill->patternAssetId.empty()) removable.erase(fill->patternAssetId);
    std::vector<std::string> result;
    for (const auto &asset:document.assets) if (removable.contains(assetId(asset))) result.push_back(assetId(asset));
    return result;
}

LayerTransactionResult LayerMergeTransaction::operator()(const Document &document,
    std::span<const std::string> selected,std::span<const LayerMergeFrame> frames,
    std::string layerId,std::string assetPrefix) const
{
    if (!validate(document).ok() || selected.empty() || frames.size()!=document.timeline.frameCount)
        return {{},"invalid document, selection or frame evidence"};
    std::unordered_set<std::string_view> ids;
    std::vector<std::size_t> indices; indices.reserve(selected.size());
    for (const auto &id:selected) {
        const auto index=layerIndex(document,id);
        if (!index || !ids.insert(id).second) return {{},"selected layer IDs must be present and distinct"};
        indices.push_back(*index);
    }
    std::sort(indices.begin(),indices.end());
    if (indices.back()-indices.front()+1!=indices.size()) return {{},"selected layers must be contiguous"};
    const auto &first=layerProperties(document.layers[indices.front()]);
    const std::string group(document.relations.groups.groupOf(first.id)); const auto artboard=first.artboardId;
    for (const auto index:indices) {
        const auto &layer=document.layers[index]; const auto &p=layerProperties(layer);
        if (layerRole(layer)!=LayerRole::Artwork || p.artboardId!=artboard ||
            document.relations.groups.groupOf(p.id)!=group || p.blendMode!=RasterBlendMode::SourceOver ||
            p.appearance.clippedToBelow || p.appearance.blendIf ||
            hasLayerLock(document.relations.locks.effective(document,p.id),LayerLock::Structure))
            return {{},"layers are locked, have different scopes, or require their backdrop"};
        for (FrameIndex frame=0;frame<document.timeline.frameCount;++frame) {
            const auto *asset=resolveAssetAt(document,layer,frame);
            if (asset && std::holds_alternative<AdjustmentLayerContent>(*asset))
                return {{},"adjustment layers require their backdrop"};
        }
    }
    const auto region=canvasRegion(document); std::vector<FrameRenderResult> merged; merged.reserve(frames.size());
    for (const auto &frame:frames) {
        if (!validFrame(frame.merged,region) || !validFrame(frame.before,region) ||
            !validFrame(frame.after,region) || !sameFrame(frame.before,frame.after))
            return {{},"before and after render evidence must match at every frame"};
        merged.push_back(frame.merged);
    }
    std::vector<std::string> removed; removed.reserve(indices.size());
    for (const auto index:indices) removed.push_back(layerProperties(document.layers[index]).id);
    return prepare(document,removed,merged,std::move(layerId),std::move(assetPrefix),false,
                   indices.front(),group,artboard);
}

LayerTransactionResult DocumentFlattenTransaction::operator()(const Document &document,
    std::span<const FrameRenderResult> frames,std::string layerId,std::string assetPrefix) const
{
    if (!validate(document).ok()) return {{},"invalid source document"};
    std::vector<std::string> removed; removed.reserve(document.layers.size());
    for (const auto &layer:document.layers) {
        const auto &id=layerProperties(layer).id;
        if (hasLayerLock(document.relations.locks.effective(document,id),LayerLock::Structure))
            return {{},"a layer to be flattened is structurally locked"};
        removed.push_back(id);
    }
    for (const auto &entry:document.relations.locks.groups())
        if (hasLayerLock(entry.flags,LayerLock::Structure)) return {{},"a group to be flattened is structurally locked"};
    return prepare(document,removed,frames,std::move(layerId),std::move(assetPrefix),true,0,{},{});
}

LayerTransactionResult prepareVerifiedLayerMerge(const Document &document,
    std::span<const std::string> selected,std::string layerId,std::string assetPrefix)
{
    if (!validate(document).ok()) return {{},"invalid source document"};
    const std::unordered_set<std::string_view> wanted(selected.begin(),selected.end());
    const auto region=canvasRegion(document);
    const FrameRenderTileRequest request{region,region.extent,RasterSampling::Nearest};
    std::vector<LayerMergeFrame> evidence;
    evidence.reserve(document.timeline.frameCount);
    for (FrameIndex frame=0;frame<document.timeline.frameCount;++frame) {
        auto before=renderFrame(document,frame);
        auto batch=renderFrameLayers(document,frame,{request});
        if (!before.ok()) return {{},before.message.empty() ? "source frame render failed" : before.message};
        if (!batch.ok()) return {{},batch.message.empty() ? "layer render failed" : batch.message};
        batch.layers.erase(std::remove_if(batch.layers.begin(),batch.layers.end(),[&](const FrameLayerTileRenderResult &item) {
            return !wanted.contains(item.layerId);
        }),batch.layers.end());
        // The detached batch is now a new contiguous sequence. Composition
        // validates its local indices, which need not match source indices.
        for (std::size_t index=0;index<batch.layers.size();++index)
            batch.layers[index].layerIndex=index;
        for (auto &item:batch.layers) {
            auto group=document.relations.groups.groupOf(item.layerId);
            std::size_t depth=0;
            while (!group.empty()) {
                if (++depth>document.relations.groups.groups().size()) return {{},"invalid group cycle"};
                const auto *parent=document.relations.groups.find(group);
                if (!parent) return {{},"group not found"};
                item.visible=item.visible && parent->visible;
                group=parent->parentId;
            }
            item.artboardId.reset();
        }
        batch.artboards.clear();
        batch.groups=LayerGroupTree{};
        auto composite=composeFrameLayers(batch);
        if (!composite.ok()) return {{},composite.message.empty() ? "selected layers could not be composed" : composite.message};
        if (composite.tiles.size()!=1) return {{},"missing merged tile"};
        FrameRenderResult merged{region.origin,std::move(composite.tiles[0].pixels),FrameRenderStatus::Success,{}};
        evidence.push_back({std::move(merged),before,before});
    }
    auto provisional=LayerMergeTransaction{}(document,selected,evidence,layerId,assetPrefix);
    if (!provisional.ok()) return provisional;
    // Candidate construction belongs to this external controller. It never
    // writes through the source and is discarded before the plan is applied.
    auto candidate=document;
    const auto &plan=provisional.plan;
    const std::unordered_set<std::string_view> removed(plan.removedAssets.begin(),plan.removedAssets.end());
    candidate.assets.erase(std::remove_if(candidate.assets.begin(),candidate.assets.end(),[&](const Asset &asset) {
        return removed.contains(assetId(asset));
    }),candidate.assets.end());
    candidate.assets.insert(candidate.assets.end(),plan.addedAssets.begin(),plan.addedAssets.end());
    candidate.layers=plan.layers;
    candidate.frames=plan.frames;
    candidate.artboards=plan.artboards;
    candidate.relations=plan.relations;
    for (FrameIndex frame=0;frame<candidate.timeline.frameCount;++frame)
        evidence[frame].after=renderFrame(candidate,frame);
    return LayerMergeTransaction{}(document,selected,evidence,std::move(layerId),std::move(assetPrefix));
}
} // namespace iiSharedCanvas
