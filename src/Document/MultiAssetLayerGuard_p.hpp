#pragma once

#include "Document/Document.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace iiSharedCanvas::detail {

// A private, single-threaded candidate for an atomic structural edit. Assets
// move into a separately allocated vector without cloning unchanged pixel data.
// Failed edits restore the original asset vector and every structural field;
// a successful controller explicitly publishes before this guard is destroyed.
class MultiAssetLayerGuard final {
    struct Removed { std::size_t index; Asset asset; };
    Document &document_;
    std::vector<Asset> originalAssets_;
    std::vector<Layer> layers_;
    std::vector<Frame> frames_;
    std::vector<Artboard> artboards_;
    SelectionLayerRelations relations_;
    DocumentAuthorship authorship_;
    FormatVersion version_;
    std::vector<Removed> removed_;
    std::vector<std::size_t> inserted_;
    bool published_=false;
public:
    // All allocation completes before the original asset values are moved.
    MultiAssetLayerGuard(Document &document,std::size_t removeCount,std::size_t addCount)
        : document_(document),layers_(document.layers),frames_(document.frames),artboards_(document.artboards),
          relations_(document.relations),authorship_(document.authorship),version_(document.formatVersion) {
        static_assert(std::is_nothrow_move_constructible_v<Asset> && std::is_nothrow_move_assignable_v<Asset>);
        static_assert(std::is_nothrow_swappable_v<SelectionLayerRelations> && std::is_nothrow_swappable_v<DocumentAuthorship>);
        if (addCount>std::numeric_limits<std::size_t>::max()-document.assets.size())
            throw std::length_error("asset candidate size overflow");
        std::vector<Asset> candidate; candidate.reserve(document.assets.size()+addCount);
        removed_.reserve(removeCount); inserted_.reserve(addCount);
        for (auto &asset:document.assets) candidate.push_back(std::move(asset));
        originalAssets_.swap(document.assets); document.assets.swap(candidate);
        using std::swap; swap(document.layers,layers_); swap(document.frames,frames_);
        swap(document.artboards,artboards_); swap(document.relations,relations_); swap(document.authorship,authorship_);
    }
    MultiAssetLayerGuard(const MultiAssetLayerGuard &)=delete;
    // Removal indices are supplied in descending order, preserving earlier indices.
    void remove(std::size_t index) {
        removed_.push_back({index,std::move(document_.assets[index])});
        document_.assets.erase(document_.assets.begin()+static_cast<std::ptrdiff_t>(index));
    }
    // Insertions are supplied in ascending order; reserved storage keeps rollback allocation-free.
    void insert(std::size_t index,Asset asset) {
        document_.assets.insert(document_.assets.begin()+static_cast<std::ptrdiff_t>(index),std::move(asset));
        inserted_.push_back(index);
    }
    void publish() noexcept { published_=true; }
    ~MultiAssetLayerGuard() noexcept {
        if (published_) return;
        for (auto i=inserted_.rbegin();i!=inserted_.rend();++i)
            document_.assets.erase(document_.assets.begin()+static_cast<std::ptrdiff_t>(*i));
        for (auto i=removed_.rbegin();i!=removed_.rend();++i)
            document_.assets.insert(document_.assets.begin()+static_cast<std::ptrdiff_t>(i->index),std::move(i->asset));
        for (std::size_t i=0;i<originalAssets_.size();++i) originalAssets_[i]=std::move(document_.assets[i]);
        originalAssets_.swap(document_.assets);
        using std::swap; swap(document_.layers,layers_); swap(document_.frames,frames_);
        swap(document_.artboards,artboards_); swap(document_.relations,relations_);
        swap(document_.authorship,authorship_); document_.formatVersion=version_;
    }
};
} // namespace iiSharedCanvas::detail
