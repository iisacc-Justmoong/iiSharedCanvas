#include "Document/SelectionLayerRelations.h"
#include "Document/Document.h"
#include "Bitmap/TiledMask.h"
#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace iiSharedCanvas {
bool NamedSelectionChannel::operator==(const NamedSelectionChannel &other) const
{
    return id == other.id && name == other.name && bool(mask) == bool(other.mask) &&
           (!mask || mask==other.mask || mask->equivalent(*other.mask));
}
bool SelectionChannelStore::insert(NamedSelectionChannel channel)
{
    if (channel.id.empty() || !channel.mask || !*channel.mask || find(channel.id)) return false;
    // Detach the wrapper from a caller-held mutable shared_ptr while keeping
    // its underlying immutable sparse tile storage shared.
    channel.mask=std::make_shared<const TiledMask>(*channel.mask);
    channels_.push_back(std::move(channel)); return true;
}
bool SelectionChannelStore::replace(std::string_view id, NamedSelectionChannel channel)
{
    if (channel.id != id || !channel.mask || !*channel.mask) return false;
    const auto at=std::find_if(channels_.begin(),channels_.end(),[&](const auto &v){return v.id==id;});
    if (at==channels_.end()) return false;
    channel.mask=std::make_shared<const TiledMask>(*channel.mask);
    *at=std::move(channel); return true;
}
bool SelectionChannelStore::rename(std::string_view id, std::string name)
{
    const auto at=std::find_if(channels_.begin(),channels_.end(),[&](const auto &v){return v.id==id;});
    if (at==channels_.end()) return false; at->name=std::move(name); return true;
}
bool SelectionChannelStore::erase(std::string_view id)
{
    const auto at=std::find_if(channels_.begin(),channels_.end(),[&](const auto &v){return v.id==id;});
    if (at==channels_.end()) return false; channels_.erase(at); return true;
}
const NamedSelectionChannel *SelectionChannelStore::find(std::string_view id) const noexcept
{
    const auto at=std::find_if(channels_.begin(),channels_.end(),[&](const auto &v){return v.id==id;});
    return at==channels_.end()?nullptr:&*at;
}

const LayerGroup *LayerGroupTree::find(std::string_view id) const noexcept
{
    if (!index_) return nullptr;
    const auto at=index_->find(id);
    return at==index_->end() ? nullptr : &groups_[at->second];
}
bool LayerGroupTree::insert(LayerGroup group)
{
    if (group.id.empty() || find(group.id) || (!group.parentId.empty() && !find(group.parentId)) ||
        (group.mode!=LayerGroupMode::PassThrough && group.mode!=LayerGroupMode::Isolated)) return false;
    if (!index_) index_=std::make_shared<GroupIndex>();
    else if (index_.use_count()!=1) index_=std::make_shared<GroupIndex>(*index_);
    auto [entry,inserted]=index_->emplace(group.id,groups_.size());
    if (!inserted) return false;
    try { groups_.push_back(std::move(group)); }
    catch (...) { index_->erase(entry); throw; }
    return true;
}
bool LayerGroupTree::erase(std::string_view id)
{
    if (!index_) return false;
    auto at=index_->find(id);
    if (at==index_->end() || std::any_of(groups_.begin(),groups_.end(),[&](const auto &g){return g.parentId==id;}) ||
        std::any_of(memberships_.begin(),memberships_.end(),[&](const auto &m){return m.groupId==id;})) return false;
    const auto position=at->second;
    if (index_.use_count()!=1) index_=std::make_shared<GroupIndex>(*index_);
    at=index_->find(id);
    groups_.erase(groups_.begin()+static_cast<std::ptrdiff_t>(position)); index_->erase(at);
    for (auto i=position;i<groups_.size();++i) index_->find(groups_[i].id)->second=i;
    return true;
}
bool LayerGroupTree::rename(std::string_view id,std::string name)
{
    if (!index_) return false;
    const auto at=index_->find(id);
    if (at==index_->end()) return false;
    groups_[at->second].name=std::move(name);
    return true;
}
bool LayerGroupTree::descendsFrom(std::string_view id, std::string_view ancestor) const noexcept
{
    if (id.empty() || ancestor.empty() || !find(id) || !find(ancestor)) return false;
    for (std::size_t n=0; !id.empty() && n<=groups_.size(); ++n) {
        if (id==ancestor) return true;
        const auto *group=find(id); if (!group) return false; id=group->parentId;
    }
    return false;
}
bool LayerGroupTree::setParent(std::string_view id, std::string parent)
{
    if (!index_) return false;
    const auto at=index_->find(id);
    if (at==index_->end() || (!parent.empty() && (!find(parent) || descendsFrom(parent,id)))) return false;
    groups_[at->second].parentId=std::move(parent); return true;
}
bool LayerGroupTree::setMode(std::string_view id, LayerGroupMode mode)
{
    if (!index_) return false;
    const auto at=index_->find(id);
    if (at==index_->end() || (mode!=LayerGroupMode::PassThrough && mode!=LayerGroupMode::Isolated)) return false;
    groups_[at->second].mode=mode; return true;
}
bool LayerGroupTree::setVisible(std::string_view id, bool visible)
{
    if (!index_) return false;
    const auto at=index_->find(id);
    if (at==index_->end()) return false; groups_[at->second].visible=visible; return true;
}
bool LayerGroupTree::assign(std::string layer, std::string group)
{
    if (layer.empty() || (!group.empty() && !find(group))) return false;
    const auto at=std::lower_bound(memberships_.begin(),memberships_.end(),layer,
        [](const auto &m,const auto &id){return m.layerId<id;});
    if (at!=memberships_.end() && at->layerId==layer) {
        if (group.empty()) memberships_.erase(at); else at->groupId=std::move(group);
    } else if (!group.empty()) memberships_.insert(at,{std::move(layer),std::move(group)});
    return true;
}
std::string_view LayerGroupTree::groupOf(std::string_view layer) const noexcept
{
    const auto at=std::lower_bound(memberships_.begin(),memberships_.end(),layer,
        [](const auto &m,auto id){return m.layerId<id;});
    return at!=memberships_.end() && at->layerId==layer ? std::string_view(at->groupId) : std::string_view{};
}

bool LayerLinkGraph::connect(std::string first, std::string second)
{
    if (first.empty() || second.empty() || first==second) return false;
    if (second<first) std::swap(first,second);
    const auto key=std::tie(first,second);
    const auto at=std::lower_bound(edges_.begin(),edges_.end(),key,
        [](const auto &edge,const auto &value){return std::tie(edge.first,edge.second)<value;});
    if (at!=edges_.end() && at->first==first && at->second==second) return false;
    edges_.insert(at,{std::move(first),std::move(second)});
    return true;
}
bool LayerLinkGraph::disconnect(std::string_view first, std::string_view second)
{
    if (second<first) std::swap(first,second);
    const auto key=std::tie(first,second);
    const auto at=std::lower_bound(edges_.begin(),edges_.end(),key,
        [](const auto &edge,const auto &value){return std::tie(edge.first,edge.second)<value;});
    if (at==edges_.end() || at->first!=first || at->second!=second) return false;
    edges_.erase(at); return true;
}
void LayerLinkGraph::removeLayer(std::string_view id)
{
    const std::string key(id);
    std::erase_if(edges_,[&](const LayerLink &edge){return edge.first==key || edge.second==key;});
}
bool LayerLinkGraph::connected(std::string_view first, std::string_view second) const noexcept
{
    if (second<first) std::swap(first,second);
    const auto key=std::tie(first,second);
    const auto at=std::lower_bound(edges_.begin(),edges_.end(),key,
        [](const auto &edge,const auto &value){return std::tie(edge.first,edge.second)<value;});
    return at!=edges_.end() && at->first==first && at->second==second;
}
std::vector<std::string> LayerLinkGraph::component(std::string_view id) const
{
    // Build adjacency once; rescanning every edge for every discovered
    // vertex makes a connected chain quadratic in its length.
    std::unordered_map<std::string_view,std::vector<std::string_view>> adjacency;
    for (const auto &edge:edges_) {
        adjacency[edge.first].push_back(edge.second);
        adjacency[edge.second].push_back(edge.first);
    }
    std::unordered_set<std::string_view> visited;
    std::vector<std::string_view> pending{id};
    while (!pending.empty()) {
        const auto current=pending.back(); pending.pop_back();
        if (!visited.insert(current).second) continue;
        const auto found=adjacency.find(current);
        if (found!=adjacency.end())
            for (const auto neighbour:found->second) if (!visited.contains(neighbour)) pending.push_back(neighbour);
    }
    std::vector<std::string> result; result.reserve(visited.size());
    for (const auto value:visited) result.emplace_back(value);
    std::sort(result.begin(),result.end());
    return result;
}

namespace {
bool setLock(std::vector<LayerLockEntry> &entries,std::string id,LayerLock flags)
{
    if (id.empty() || static_cast<unsigned>(flags)>static_cast<unsigned>(LayerLock::All)) return false;
    const auto at=std::lower_bound(entries.begin(),entries.end(),id,[](const auto &e,const auto &key){return e.id<key;});
    if (at!=entries.end() && at->id==id) {
        if (flags==LayerLock::None) entries.erase(at); else at->flags=flags;
    } else if (flags!=LayerLock::None) entries.insert(at,{std::move(id),flags});
    return true;
}
LayerLock getLock(const std::vector<LayerLockEntry> &entries,std::string_view id) noexcept
{
    const auto at=std::lower_bound(entries.begin(),entries.end(),id,[](const auto &e,auto key){return e.id<key;});
    return at!=entries.end() && at->id==id ? at->flags : LayerLock::None;
}
}
bool LayerLockPolicy::setLayer(std::string id,LayerLock flags) { return setLock(layers_,std::move(id),flags); }
bool LayerLockPolicy::setGroup(std::string id,LayerLock flags) { return setLock(groups_,std::move(id),flags); }
LayerLock LayerLockPolicy::effective(const Document &document,std::string_view layer) const noexcept
{
    auto flags=getLock(layers_,layer);
    auto id=document.relations.groups.groupOf(layer);
    for (std::size_t n=0; !id.empty() && n<=document.relations.groups.groups().size(); ++n) {
        flags=flags|getLock(groups_,id);
        const auto *group=document.relations.groups.find(id); if (!group) break; id=group->parentId;
    }
    return flags;
}
LayerLock LayerLockPolicy::assetLocks(const Document &document,std::string_view asset) const
{
    if (layers_.empty() && groups_.empty()) return LayerLock::None;
    LayerLock flags=LayerLock::None;
    for (const auto &reference:assetReferences(document,std::string(asset)))
        flags=flags|effective(document,layerProperties(document.layers[reference.layerIndex]).id);
    return flags;
}
bool SelectionLayerRelations::empty() const noexcept
{
    return selections.channels().empty() && groups.groups().empty() && groups.memberships().empty() &&
           links.edges().empty() && locks.layers().empty() && locks.groups().empty();
}
void SelectionLayerRelations::removeLayer(std::string_view id)
{
    groups.assign(std::string(id),{});
    locks.setLayer(std::string(id),LayerLock::None);
    links.removeLayer(id);
}
void SelectionLayerRelations::renameLayer(std::string_view from,std::string to)
{
    const std::string group(groups.groupOf(from));
    LayerLock lock=LayerLock::None;
    for (const auto &entry:locks.layers()) if (entry.id==from) { lock=entry.flags; break; }
    const auto existing=links.edges();
    removeLayer(from);
    if (!group.empty()) groups.assign(to,group);
    if (lock!=LayerLock::None) locks.setLayer(to,lock);
    for (const auto &edge:existing)
        if (edge.first==from || edge.second==from)
            links.connect(edge.first==from ? to : edge.first,edge.second==from ? to : edge.second);
}
std::string SelectionLayerRelations::validate(const Document &document) const
{
    if (empty()) return {};
    for (const auto &channel:selections.channels())
        if (channel.id.empty() || !channel.mask || !*channel.mask) return "invalid selection channel";
    // Index IDs once. A per-group scan of the entire layer list becomes
    // quadratic even for a shallow document with independent groups.
    std::unordered_set<std::string_view> layerIds;
    layerIds.reserve(document.layers.size());
    for (const auto &layer:document.layers) layerIds.insert(layerProperties(layer).id);
    std::unordered_map<std::string_view,std::size_t> groupIds;
    groupIds.reserve(groups.groups().size());
    for (std::size_t i=0;i<groups.groups().size();++i) {
        const auto &group=groups.groups()[i];
        if (group.id.empty() || layerIds.contains(group.id) || !groupIds.emplace(group.id,i).second)
            return "group and layer IDs must be unique and differ";
    }
    for (const auto &membership:groups.memberships())
        if (!layerIds.contains(membership.layerId) || !groupIds.contains(membership.groupId)) return "invalid group membership";
    for (const auto &edge:links.edges())
        if (!layerIds.contains(edge.first) || !layerIds.contains(edge.second)) return "link references a missing layer";
    for (const auto &entry:locks.layers()) if (!layerIds.contains(entry.id)) return "lock references a missing layer";
    for (const auto &entry:locks.groups()) if (!groupIds.contains(entry.id)) return "lock references a missing group";
    // Reject broken parent chains and bound recursion before visiting layers.
    for (const auto &group:groups.groups()) {
        auto parent=std::string_view(group.parentId);
        std::size_t depth=1;
        while (!parent.empty()) {
            const auto at=groupIds.find(parent);
            if (at==groupIds.end() || ++depth>256) return "invalid group parent or nesting exceeds the supported depth";
            parent=groups.groups()[at->second].parentId;
        }
    }
    struct Span {
        bool seen=false;
        std::size_t last=0;
        std::optional<std::string_view> artboard;
    };
    std::vector<Span> spans(groups.groups().size());
    // Walk each layer's ancestors once. Every ancestor's descendants must
    // occupy consecutive positions and share one artboard or world scope.
    for (std::size_t i=0;i<document.layers.size();++i) {
        const auto &properties=layerProperties(document.layers[i]);
        const std::optional<std::string_view> scope=properties.artboardId ?
            std::optional<std::string_view>(*properties.artboardId) : std::nullopt;
        auto owner=groups.groupOf(properties.id);
        while (!owner.empty()) {
            const auto found=groupIds.find(owner);
            if (found==groupIds.end()) return "invalid group membership";
            auto &span=spans[found->second];
            if (span.seen && span.last+1!=i) return "group descendants must be contiguous";
            if (span.seen && span.artboard!=scope) return "a group cannot span artboards";
            span.seen=true; span.last=i; span.artboard=scope;
            owner=groups.groups()[found->second].parentId;
        }
    }
    return {};
}
} // namespace iiSharedCanvas
