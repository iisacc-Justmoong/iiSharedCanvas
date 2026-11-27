#pragma once
#include "iiSharedCanvas/Export.h"
#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace iiSharedCanvas {
class TiledMask;
struct Document;

// A named, immutable coverage snapshot. Copies may share the immutable mask.
struct IISHAREDCANVAS_EXPORT NamedSelectionChannel {
    std::string id, name;
    std::shared_ptr<const TiledMask> mask;
    [[nodiscard]] bool operator==(const NamedSelectionChannel &) const;
};

// Value-owned channel collection. Mutate a copy and submit it to document history.
class IISHAREDCANVAS_EXPORT SelectionChannelStore final {
public:
    // Reject a missing/duplicate ID or an absent mask without changing the store.
    bool insert(NamedSelectionChannel channel);
    // Replace or rename only an existing channel; replacement preserves its ID.
    bool replace(std::string_view id, NamedSelectionChannel channel);
    // Preserve the stable ID and coverage while replacing the display name.
    bool rename(std::string_view id, std::string name);
    // Remove one existing channel; absent IDs leave the store unchanged.
    bool erase(std::string_view id);
    // Borrow a channel until the next store mutation, or return nullptr.
    [[nodiscard]] const NamedSelectionChannel *find(std::string_view id) const noexcept;
    // Inspect channels in their document insertion order.
    [[nodiscard]] const std::vector<NamedSelectionChannel> &channels() const noexcept { return channels_; }
    [[nodiscard]] bool operator==(const SelectionChannelStore &) const = default;
private:
    std::vector<NamedSelectionChannel> channels_;
};

enum class LayerGroupMode : std::uint8_t { PassThrough, Isolated };
// Groups are structural values, not artwork-layer alternatives.
struct LayerGroup {
    std::string id, name, parentId;
    LayerGroupMode mode = LayerGroupMode::PassThrough;
    bool visible = true;
    bool operator==(const LayerGroup &) const = default;
};
struct LayerGroupMembership {
    std::string layerId, groupId;
    bool operator==(const LayerGroupMembership &) const = default;
};

// Nested groups reference existing layers; flat layer order remains canonical.
class IISHAREDCANVAS_EXPORT LayerGroupTree final {
public:
    // Parents must already exist, and cycles or duplicate IDs are rejected.
    bool insert(LayerGroup group);
    // Delete an empty group only; move children and members before deleting.
    bool erase(std::string_view id);
    // Rename a group without changing its stable ID or descendants.
    bool rename(std::string_view id, std::string name);
    // Move a group below another group or to the root; cycles are rejected.
    bool setParent(std::string_view id, std::string parentId);
    // Select the backdrop semantics used while compositing this group.
    bool setMode(std::string_view id, LayerGroupMode mode);
    // Set visibility inherited by all descendants during composition.
    bool setVisible(std::string_view id, bool visible);
    // An empty group ID removes membership; document existence is checked on commit.
    bool assign(std::string layerId, std::string groupId);
    // Borrow group metadata until the tree is next mutated.
    [[nodiscard]] const LayerGroup *find(std::string_view id) const noexcept;
    // Return a layer's direct group ID, or an empty view at the root.
    [[nodiscard]] std::string_view groupOf(std::string_view layerId) const noexcept;
    // Test membership in an ancestor chain, including the group itself.
    [[nodiscard]] bool descendsFrom(std::string_view groupId, std::string_view ancestorId) const noexcept;
    // Group definitions retain insertion order independently from layer order.
    [[nodiscard]] const std::vector<LayerGroup> &groups() const noexcept { return groups_; }
    // Memberships are sorted by stable layer ID for deterministic encoding.
    [[nodiscard]] const std::vector<LayerGroupMembership> &memberships() const noexcept { return memberships_; }
    // Index storage is an implementation detail and does not affect equality.
    [[nodiscard]] bool operator==(const LayerGroupTree &other) const {
        return groups_==other.groups_ && memberships_==other.memberships_;
    }
private:
    std::vector<LayerGroup> groups_;
    std::vector<LayerGroupMembership> memberships_;
    // The copy-on-write index permits allocation-free moves and rollback even
    // on standard-library implementations whose map move may allocate.
    using GroupIndex=std::map<std::string,std::size_t,std::less<>>;
    std::shared_ptr<GroupIndex> index_;
};

struct LayerLink { std::string first, second; bool operator==(const LayerLink &) const = default; };
// An undirected graph of associations. It never applies edits to its vertices.
class IISHAREDCANVAS_EXPORT LayerLinkGraph final {
public:
    // Add a canonical undirected edge; duplicates and self-links fail.
    bool connect(std::string first, std::string second);
    // Remove an existing direct edge without changing either layer.
    bool disconnect(std::string_view first, std::string_view second);
    // Remove all edges incident to a layer in one linear pass.
    void removeLayer(std::string_view layerId);
    // Report a direct edge, rather than transitive component membership.
    [[nodiscard]] bool connected(std::string_view first, std::string_view second) const noexcept;
    // Return the transitively connected IDs in stable lexical order.
    [[nodiscard]] std::vector<std::string> component(std::string_view layerId) const;
    // Inspect canonical undirected edges in stable lexical order.
    [[nodiscard]] const std::vector<LayerLink> &edges() const noexcept { return edges_; }
    [[nodiscard]] bool operator==(const LayerLinkGraph &) const = default;
private:
    std::vector<LayerLink> edges_;
};

enum class LayerLock : std::uint8_t { None = 0, Pixels = 1, Transparency = 2, Transform = 4, Structure = 8, All = 15 };
[[nodiscard]] constexpr LayerLock operator|(LayerLock a, LayerLock b) noexcept {
    return static_cast<LayerLock>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr bool hasLayerLock(LayerLock value, LayerLock flag) noexcept {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
}
struct LayerLockEntry { std::string id; LayerLock flags = LayerLock::None; bool operator==(const LayerLockEntry &) const = default; };
// Direct and inherited locks are combined before an editor accepts a change.
class IISHAREDCANVAS_EXPORT LayerLockPolicy final {
public:
    // Replace direct flags; None removes the stored entry.
    bool setLayer(std::string id, LayerLock flags);
    // Replace a group's inherited flags; None removes its entry.
    bool setGroup(std::string id, LayerLock flags);
    // Combine a layer's direct flags with every ancestor group's flags.
    [[nodiscard]] LayerLock effective(const Document &, std::string_view layerId) const noexcept;
    // Union of locks on every content or mask reference to a shared asset.
    [[nodiscard]] LayerLock assetLocks(const Document &, std::string_view assetId) const;
    // Inspect direct layer and group entries, sorted by stable ID.
    [[nodiscard]] const std::vector<LayerLockEntry> &layers() const noexcept { return layers_; }
    [[nodiscard]] const std::vector<LayerLockEntry> &groups() const noexcept { return groups_; }
    [[nodiscard]] bool operator==(const LayerLockPolicy &) const = default;
private:
    std::vector<LayerLockEntry> layers_, groups_;
};

// A detached candidate which is validated against the document on publication.
struct IISHAREDCANVAS_EXPORT SelectionLayerRelations {
    SelectionChannelStore selections;
    LayerGroupTree groups;
    LayerLinkGraph links;
    LayerLockPolicy locks;
    // True only if no channel, group, membership, edge or lock is stored.
    [[nodiscard]] bool empty() const noexcept;
    // Explain the first invalid reference, grouping, scope or depth; empty means valid.
    [[nodiscard]] std::string validate(const Document &) const;
    // Remove every membership, edge and direct lock belonging to a deleted layer.
    void removeLayer(std::string_view layerId);
    // Retarget relationships when a layer's stable identifier changes.
    void renameLayer(std::string_view oldId, std::string newId);
    [[nodiscard]] bool operator==(const SelectionLayerRelations &) const = default;
};
} // namespace iiSharedCanvas
