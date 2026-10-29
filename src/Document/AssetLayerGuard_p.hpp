#pragma once

#include "Document/Document.h"

#include <optional>
#include <type_traits>
#include <utility>

namespace iiSharedCanvas::detail {

// Candidate owns the original assets by move, never by pixel copy. While it is
// active a DocumentFile is single-threaded and must not expose the candidate.
// On rejection the original vector and its asset objects are restored in place.
class AssetLayerGuard final {
    Document &document_;
    std::vector<Asset> originalAssets_;
    std::vector<Layer> originalLayers_;
    DocumentAuthorship originalAuthorship_;
    FormatVersion originalVersion_;
    std::optional<Asset> removed_;
    std::size_t index_=0;
    enum class Change { None, Inserted, Removed } change_=Change::None;
    bool published_=false;

public:
    explicit AssetLayerGuard(Document &document)
        : document_(document),originalLayers_(document.layers),
          originalAuthorship_(document.authorship),originalVersion_(document.formatVersion) {
        static_assert(std::is_nothrow_move_constructible_v<Asset>);
        static_assert(std::is_nothrow_move_assignable_v<Asset>);
        static_assert(std::is_nothrow_swappable_v<DocumentAuthorship>);
        std::vector<Asset> candidate;
        candidate.reserve(document.assets.size()+1);
        for (auto &asset:document.assets) candidate.push_back(std::move(asset));
        originalAssets_.swap(document.assets);
        document.assets.swap(candidate);
        using std::swap; swap(document.layers,originalLayers_);
        swap(document.authorship,originalAuthorship_);
    }
    AssetLayerGuard(const AssetLayerGuard &)=delete;
    AssetLayerGuard &operator=(const AssetLayerGuard &)=delete;
    void insert(std::size_t index,Asset asset) {
        document_.assets.insert(document_.assets.begin()+static_cast<std::ptrdiff_t>(index),std::move(asset));
        index_=index; change_=Change::Inserted;
    }
    void remove(std::size_t index) {
        removed_.emplace(std::move(document_.assets[index]));
        document_.assets.erase(document_.assets.begin()+static_cast<std::ptrdiff_t>(index));
        index_=index; change_=Change::Removed;
    }
    void publish() noexcept { published_=true; }
    ~AssetLayerGuard() noexcept {
        if (published_) return;
        if (change_==Change::Inserted)
            document_.assets.erase(document_.assets.begin()+static_cast<std::ptrdiff_t>(index_));
        else if (change_==Change::Removed)
            document_.assets.insert(document_.assets.begin()+static_cast<std::ptrdiff_t>(index_),std::move(*removed_));
        for (std::size_t i=0;i<originalAssets_.size();++i)
            originalAssets_[i]=std::move(document_.assets[i]);
        originalAssets_.swap(document_.assets);
        using std::swap; swap(document_.layers,originalLayers_);
        swap(document_.authorship,originalAuthorship_);
        document_.formatVersion=originalVersion_;
    }
};
} // namespace iiSharedCanvas::detail
