#include "Document/LayerLayout.h"
#include "Validation/Validation.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_set>

namespace iiSharedCanvas {
namespace {
// Keep bounds arithmetic finite before deriving offsets or allocating results.
bool validBounds(const LayerBounds &bounds) noexcept
{
    return std::isfinite(bounds.x) && std::isfinite(bounds.y) &&
        std::isfinite(bounds.width) && std::isfinite(bounds.height) &&
        bounds.width>=0 && bounds.height>=0 &&
        std::isfinite(bounds.x+bounds.width) && std::isfinite(bounds.y+bounds.height);
}

bool validGeometry(std::span<const LayerGeometry> layers)
{
    std::unordered_set<std::string_view> ids;
    for (const auto &layer:layers)
        if (layer.id.empty() || !ids.insert(layer.id).second || !validBounds(layer.bounds)) return false;
    return !layers.empty();
}

// Validate the structural rules that depend on order without copying any of
// the document's potentially large raster or media assets.
bool validOrder(const Document &document,const std::vector<std::size_t> &order)
{
    const auto &tree=document.relations.groups;
    for (const auto &group:tree.groups()) {
        std::size_t first=order.size(),last=0,count=0;
        for (std::size_t i=0;i<order.size();++i) {
            const auto member=tree.groupOf(layerProperties(document.layers[order[i]]).id);
            if (!member.empty() && tree.descendsFrom(member,group.id)) {
                first=std::min(first,i); last=i; ++count;
            }
        }
        if (count && last-first+1!=count) return false;
    }
    for (std::size_t i=0;i<order.size();++i) {
        const auto &layer=document.layers[order[i]]; const auto &p=layerProperties(layer);
        if (layerRole(layer)!=LayerRole::Artwork || !p.appearance.clippedToBelow) continue;
        bool base=false;
        for (std::size_t j=i;j>0;) {
            const auto &candidate=document.layers[order[--j]]; const auto &q=layerProperties(candidate);
            if (layerRole(candidate)!=LayerRole::Artwork || q.artboardId!=p.artboardId) continue;
            if (tree.groupOf(q.id)!=tree.groupOf(p.id)) break;
            if (q.appearance.clippedToBelow) continue;
            const auto adjustment=[&](const std::string &id) {
                const auto *asset=findAsset(document,id);
                return asset && std::holds_alternative<AdjustmentLayerContent>(*asset);
            };
            if (const auto *source=staticLayerSource(candidate); source && adjustment(source->assetId)) return false;
            if (keyframedLayerSource(candidate)) for (const auto &frame:document.frames)
                for (const auto &key:frame.keyframes)
                    if (key.layerId==q.id && adjustment(key.assetId)) return false;
            base=true; break;
        }
        if (!base) return false;
    }
    return true;
}

// Find the immediate child scope containing a descendant. The validated tree
// owns all returned views for the duration of planning. Empty groups have no
// position in the document's flat layer order and need no separate unit.
std::string_view directChild(const LayerGroupTree &tree,std::string_view member,std::string_view parent)
{
    for (std::size_t depth=0;depth<tree.groups().size();++depth) {
        const auto *group=tree.find(member);
        if (!group) return {};
        if (group->parentId==parent) return member;
        if (group->parentId.empty()) return {};
        member=group->parentId;
    }
    return {};
}

// Arrange one direct scope. Nested groups are indivisible sibling units, so
// crossing one moves its entire descendant range without changing membership.
bool arrangeScope(const Document &document,const std::unordered_set<std::string_view> &wanted,
                  std::string_view scope,LayerArrangement action,std::vector<std::size_t> &order)
{
    const auto &tree=document.relations.groups;
    const auto groupAt=[&](std::size_t position) {
        return tree.groupOf(layerProperties(document.layers[order[position]]).id);
    };
    std::size_t begin=0,end=order.size();
    if (!scope.empty()) {
        begin=order.size(); end=0;
        for (std::size_t i=0;i<order.size();++i)
            if (tree.descendsFrom(groupAt(i),scope)) { begin=std::min(begin,i); end=i+1; }
        if (begin==order.size()) return false;
    }
    struct Unit { std::size_t begin,end; bool selected; };
    std::vector<Unit> units;
    for (std::size_t i=begin;i<end;) {
        const auto group=groupAt(i);
        if (group==scope) {
            units.push_back({i,i+1,wanted.contains(layerProperties(document.layers[order[i]]).id)});
            ++i; continue;
        }
        const auto child=directChild(tree,group,scope);
        if (child.empty()) return false;
        auto next=i+1;
        while (next<end && tree.descendsFrom(groupAt(next),child)) ++next;
        units.push_back({i,next,false}); i=next;
    }
    if (action==LayerArrangement::Front || action==LayerArrangement::Back) {
        std::stable_partition(units.begin(),units.end(),[&](const Unit &unit) {
            return action==LayerArrangement::Front ? !unit.selected : unit.selected;
        });
    } else if (action==LayerArrangement::Forward) {
        for (std::size_t i=units.size();i>1;--i)
            if (units[i-2].selected && !units[i-1].selected) std::swap(units[i-2],units[i-1]);
    } else {
        for (std::size_t i=1;i<units.size();++i)
            if (units[i].selected && !units[i-1].selected) std::swap(units[i],units[i-1]);
    }
    std::vector<std::size_t> arranged; arranged.reserve(end-begin);
    for (const auto &unit:units)
        arranged.insert(arranged.end(),order.begin()+static_cast<std::ptrdiff_t>(unit.begin),
                        order.begin()+static_cast<std::ptrdiff_t>(unit.end));
    std::copy(arranged.begin(),arranged.end(),order.begin()+static_cast<std::ptrdiff_t>(begin));
    return true;
}
} // namespace

LayerArrangementResult LayerArrangementPlanner::operator()(const Document &document,
    std::span<const std::string> selected, LayerArrangement action) const
{
    if (selected.empty() || static_cast<unsigned>(action)>static_cast<unsigned>(LayerArrangement::Back) ||
        !validate(document).ok()) return {{},"invalid document, selection or arrangement"};
    std::unordered_set<std::string_view> wanted,seenScopes;
    std::vector<std::string_view> scopes;
    for (const auto &id:selected) {
        if (id.empty() || !wanted.insert(id).second || !findLayer(document,id))
            return {{},"selected layer IDs must be distinct and present"};
        const auto scope=document.relations.groups.groupOf(id);
        if (seenScopes.insert(scope).second) scopes.push_back(scope);
    }
    std::vector<std::size_t> indices(document.layers.size());
    std::iota(indices.begin(),indices.end(),std::size_t{0});
    for (const auto scope:scopes)
        if (!arrangeScope(document,wanted,scope,action,indices))
            return {{},"the selected group scope is invalid"};
    for (std::size_t i=0;i<indices.size();++i) if (indices[i]!=i) {
        const auto &id=layerProperties(document.layers[i]).id;
        if (hasLayerLock(document.relations.locks.effective(document,id),LayerLock::Structure))
            return {{},"a layer affected by the order is structurally locked"};
    }
    if (!validOrder(document,indices)) return {{},"the order violates group or clipping constraints"};
    LayerArrangementResult result; result.order.reserve(indices.size());
    for (const auto index:indices) result.order.push_back(layerProperties(document.layers[index]).id);
    return result;
}

LayerLayoutResult LayerAlignmentSolver::operator()(std::span<const LayerGeometry> layers,
    LayerBounds reference,LayerAlignment alignment) const
{
    if (!validGeometry(layers) || !validBounds(reference) ||
        static_cast<unsigned>(alignment)>static_cast<unsigned>(LayerAlignment::Bottom))
        return {{},"invalid alignment geometry"};
    LayerLayoutResult result; result.offsets.reserve(layers.size());
    for (const auto &layer:layers) {
        const auto &b=layer.bounds; LayerOffset offset{layer.id};
        switch (alignment) {
        case LayerAlignment::Left: offset.x=reference.x-b.x; break;
        case LayerAlignment::Center: offset.x=(reference.x+reference.width*.5)-(b.x+b.width*.5); break;
        case LayerAlignment::Right: offset.x=(reference.x+reference.width)-(b.x+b.width); break;
        case LayerAlignment::Top: offset.y=reference.y-b.y; break;
        case LayerAlignment::Middle: offset.y=(reference.y+reference.height*.5)-(b.y+b.height*.5); break;
        case LayerAlignment::Bottom: offset.y=(reference.y+reference.height)-(b.y+b.height); break;
        }
        if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) return {{},"alignment offset is not finite"};
        result.offsets.push_back(std::move(offset));
    }
    return result;
}

LayerLayoutResult LayerDistributionSolver::operator()(std::span<const LayerGeometry> layers,
    LayerAxis axis,LayerDistribution distribution) const
{
    if (layers.size()<2 || !validGeometry(layers) ||
        static_cast<unsigned>(axis)>static_cast<unsigned>(LayerAxis::Vertical) ||
        static_cast<unsigned>(distribution)>static_cast<unsigned>(LayerDistribution::Gaps))
        return {{},"invalid distribution geometry"};
    const auto start=[&](std::size_t i) { return axis==LayerAxis::Horizontal ? layers[i].bounds.x : layers[i].bounds.y; };
    const auto width=[&](std::size_t i) { return axis==LayerAxis::Horizontal ? layers[i].bounds.width : layers[i].bounds.height; };
    std::vector<std::size_t> order(layers.size()); std::iota(order.begin(),order.end(),std::size_t{0});
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b) {
        const auto x=start(a)+(distribution==LayerDistribution::Centers ? width(a)*.5 : 0);
        const auto y=start(b)+(distribution==LayerDistribution::Centers ? width(b)*.5 : 0);
        return x<y;
    });
    LayerLayoutResult result; result.offsets.reserve(layers.size());
    for (const auto &layer:layers) result.offsets.push_back({layer.id});
    if (layers.size()==2) return result;
    double step=0,cursor=0;
    if (distribution==LayerDistribution::Centers) {
        cursor=start(order.front())+width(order.front())*.5;
        const auto last=start(order.back())+width(order.back())*.5;
        step=(last-cursor)/double(order.size()-1);
    } else {
        double total=0;
        for (auto i:order) { total+=width(i); if (!std::isfinite(total)) return {{},"distribution extent overflow"}; }
        const auto end=start(order.back())+width(order.back());
        step=(end-start(order.front())-total)/double(order.size()-1);
        cursor=start(order.front());
    }
    if (!std::isfinite(step)) return {{},"distribution spacing is not finite"};
    for (std::size_t n=1;n+1<order.size();++n) {
        const auto i=order[n]; double delta;
        if (distribution==LayerDistribution::Centers)
            delta=(cursor+step*double(n))-(start(i)+width(i)*.5);
        else {
            cursor+=width(order[n-1])+step;
            delta=cursor-start(i);
        }
        if (!std::isfinite(delta)) return {{},"distribution offset is not finite"};
        if (axis==LayerAxis::Horizontal) result.offsets[i].x=delta; else result.offsets[i].y=delta;
    }
    return result;
}
} // namespace iiSharedCanvas
