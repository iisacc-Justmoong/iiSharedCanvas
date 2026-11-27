#include "Document/DocumentHistory.h"
#include "Document/AssetHistorySnapshot_p.hpp"
#include "Document/AssetLayerGuard_p.hpp"
#include "Document/MultiAssetLayerGuard_p.hpp"
#include "Document/LayerTransactions_p.hpp"
#include "Document/CanvasAffine_p.hpp"
#include "Timeline/TimelineProjectCodec_p.hpp"
#include "Document/DenseControlAsset_p.hpp"
#include "Serialization/DocumentRecords_p.hpp"
#include "Document/TimelineGuard_p.hpp"
#include "Bitmap/BitmapEditor.h"
#include "Bitmap/TiledMask.h"
#include "Bitmap/BitmapEditJournal_p.h"
#include "Bitmap/ChunkedBitmapEditor.h"
#include "Bitmap/StrokeSample_p.hpp"
#include "Document/DocumentEditor.h"
#include "File/DocumentFile.h"
#include "ControlNet/ControlNetParameters.h"
#include "Validation/Validation.h"
#include <iiSDKCore/Core/Text/Utf8.h>
#include <Render/DirtyRegion.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace iiSharedCanvas {
namespace detail {
// Each history entry owns a fresh editor; carry the coordinator's seed across
// entries so consecutive committed strokes do not repeat random patterns.
struct BitmapHistorySeed {
    static void set(BitmapEditor &editor,std::uint32_t seed) noexcept { editor.m_nextStrokeSeed=seed; }
    static void set(ChunkedBitmapEditor &editor,std::uint32_t seed) noexcept { editor.m_nextStrokeSeed=seed; }
    static bool canvasMode(BitmapEditor &,StrokeCanvasMode mode) { return mode==StrokeCanvasMode::Clip; }
    static bool canvasMode(ChunkedBitmapEditor &editor,StrokeCanvasMode mode) {
        if (mode!=StrokeCanvasMode::Clip && mode!=StrokeCanvasMode::Grow) return false;
        if (mode==StrokeCanvasMode::Grow &&
            (!editor.m_document || editor.m_document->canvasMode!=CanvasMode::Infinite)) return false;
        editor.journal().setStrokeCanvasGrowth(mode==StrokeCanvasMode::Grow);
        return true;
    }
    static bool growing(const BitmapEditor &) noexcept { return false; }
    static bool growing(const ChunkedBitmapEditor &editor) noexcept {
        return editor.m_journal && editor.m_journal->strokeCanvasGrowth();
    }
    static bool stagePixels(BitmapEditor &editor,PixelPreviewCanvasMode mode) {
        if (mode!=PixelPreviewCanvasMode::Clip) return false;
        editor.journal().setStagedPixels(false); return true;
    }
    static bool stagePixels(ChunkedBitmapEditor &editor,PixelPreviewCanvasMode mode) {
        if (mode!=PixelPreviewCanvasMode::Clip && mode!=PixelPreviewCanvasMode::Grow) return false;
        if (mode==PixelPreviewCanvasMode::Grow &&
            (!editor.m_document || editor.m_document->canvasMode!=CanvasMode::Infinite)) return false;
        editor.journal().setStagedPixels(mode==PixelPreviewCanvasMode::Grow); return true;
    }
    template<class Editor> static bool stagedPixels(const Editor &editor) noexcept {
        return editor.m_journal && editor.m_journal->stagedPixels();
    }
    template<class Editor> static std::vector<BitmapPreviewTile> stagedTiles(const Editor &editor) {
        return editor.m_journal ? editor.m_journal->stagedPixelTiles() : std::vector<BitmapPreviewTile>{};
    }
    template<class Editor> static std::optional<CanvasRegion> stagedRegion(const Editor &editor) noexcept {
        return editor.m_journal ? editor.m_journal->stagedPixelRegion() : std::nullopt;
    }
    template<class Editor> static bool discardStagedPixels(Editor &editor) noexcept {
        if (!stagedPixels(editor)) return false;
        editor.m_dirtyBounds=uniteDevicePixelRects(editor.m_dirtyBounds,
                                                   editor.m_journal->discardStagedPixels());
        editor.m_historyResult={}; return true;
    }
};
}
namespace {
bool sameTransform(const AffineTransform &a, const AffineTransform &b) noexcept {
    return a.m11==b.m11 && a.m12==b.m12 && a.m21==b.m21 && a.m22==b.m22 &&
           a.translationX==b.translationX && a.translationY==b.translationY;
}
// Charge the dynamically owned portions of an appearance on both sides of a
// history command. Saturation makes an unrepresentable size fail admission.
std::size_t appearanceExtra(const LayerAppearance &a) noexcept {
    std::size_t total=0;
    const auto add=[&](std::size_t amount) {
        if (amount>std::numeric_limits<std::size_t>::max()-total) total=std::numeric_limits<std::size_t>::max();
        else total+=amount;
    };
    const auto elements=[&](std::size_t count,std::size_t size) {
        if (count>std::numeric_limits<std::size_t>::max()/size) total=std::numeric_limits<std::size_t>::max();
        else add(count*size);
    };
    for (const auto *mask:{&a.rasterMask,&a.vectorMask,&a.filterMask}) if (*mask) add((*mask)->assetId.capacity());
    elements(a.filters.capacity(),sizeof(LayerFilter));
    for (const auto &filter:a.filters) add(filter.name.capacity());
    elements(a.maskStack.capacity(),sizeof(LayerMaskEntry));
    for (const auto &entry:a.maskStack) { add(entry.id.capacity()); add(entry.name.capacity()); add(entry.mask.assetId.capacity()); }
    elements(a.styles.effects.capacity(),sizeof(LayerStyle));
    return total;
}
}

LayerChangeValue LayerChange::read(const Document &document, const Layer &layer, LayerChangeField field)
{
    const auto &p=layerProperties(layer);
    switch (field) {
    case LayerChangeField::Name: return p.name;
    case LayerChangeField::Visible: return p.visible;
    case LayerChangeField::Opacity: return p.opacity;
    case LayerChangeField::Transform: return p.transform;
    case LayerChangeField::Blend: return p.blendMode;
    case LayerChangeField::Order: return layerIndex(document,p.id).value_or(document.layers.size());
    case LayerChangeField::Appearance: return std::make_shared<const LayerAppearance>(p.appearance);
    }
    return false;
}
bool LayerChange::same(LayerChangeField field, const LayerChangeValue &a, const LayerChangeValue &b) noexcept
{
    if (a.index()!=b.index()) return false;
    switch (field) {
    case LayerChangeField::Name: { auto x=std::get_if<std::string>(&a); return x && *x==std::get<std::string>(b); }
    case LayerChangeField::Visible: { auto x=std::get_if<bool>(&a); return x && *x==std::get<bool>(b); }
    case LayerChangeField::Opacity: { auto x=std::get_if<double>(&a); return x && *x==std::get<double>(b); }
    case LayerChangeField::Transform: { auto x=std::get_if<AffineTransform>(&a); return x && sameTransform(*x,std::get<AffineTransform>(b)); }
    case LayerChangeField::Blend: { auto x=std::get_if<RasterBlendMode>(&a); return x && *x==std::get<RasterBlendMode>(b); }
    case LayerChangeField::Order: { auto x=std::get_if<std::size_t>(&a); return x && *x==std::get<std::size_t>(b); }
    case LayerChangeField::Appearance: {
        const auto *x=std::get_if<LayerAppearanceValue>(&a); const auto &y=std::get<LayerAppearanceValue>(b);
        return x && *x && y && **x==*y;
    }
    }
    return false;
}
DocumentEditResult LayerChange::apply(DocumentEditor &editor, bool forward) const
{
    return applyValue(editor,forward ? after : before);
}
DocumentEditResult LayerChange::applyValue(DocumentEditor &editor, const LayerChangeValue &value) const
{
    switch (field) {
    case LayerChangeField::Name: if (const auto *v=std::get_if<std::string>(&value)) return editor.setLayerName(id,*v); break;
    case LayerChangeField::Visible: if (const auto *v=std::get_if<bool>(&value)) return editor.setLayerVisible(id,*v); break;
    case LayerChangeField::Opacity: if (const auto *v=std::get_if<double>(&value)) return editor.setLayerOpacity(id,*v); break;
    case LayerChangeField::Transform: if (const auto *v=std::get_if<AffineTransform>(&value)) return editor.setLayerTransform(id,*v); break;
    case LayerChangeField::Blend: if (const auto *v=std::get_if<RasterBlendMode>(&value)) return editor.setLayerBlendMode(id,*v); break;
    case LayerChangeField::Order: if (const auto *v=std::get_if<std::size_t>(&value)) return editor.moveLayer(id,*v); break;
    case LayerChangeField::Appearance: if (const auto *v=std::get_if<LayerAppearanceValue>(&value); v && *v) return editor.setLayerAppearance(id,**v); break;
    }
    return {DocumentEditCode::InvalidArgument,false,{},"layer change value does not match its field"};
}
bool LayerChange::swapProperty(LayerProperties &p, LayerChangeField field, LayerChangeValue &value) noexcept
{
    using std::swap;
    switch (field) {
    case LayerChangeField::Name: if (auto *v=std::get_if<std::string>(&value)) {swap(p.name,*v);return true;} break;
    case LayerChangeField::Visible: if (auto *v=std::get_if<bool>(&value)) {swap(p.visible,*v);return true;} break;
    case LayerChangeField::Opacity: if (auto *v=std::get_if<double>(&value)) {swap(p.opacity,*v);return true;} break;
    case LayerChangeField::Transform: if (auto *v=std::get_if<AffineTransform>(&value)) {swap(p.transform,*v);return true;} break;
    case LayerChangeField::Blend: if (auto *v=std::get_if<RasterBlendMode>(&value)) {swap(p.blendMode,*v);return true;} break;
    case LayerChangeField::Order: break;
    case LayerChangeField::Appearance: break; // Immutable snapshots use DocumentEditor.
    }
    return false;
}
std::size_t LayerChange::retainedBytes() const noexcept
{
    const auto extra=[](const LayerChangeValue &value) {
        if (const auto *appearance=std::get_if<LayerAppearanceValue>(&value); appearance && *appearance) {
            const auto dynamic=appearanceExtra(**appearance);
            return dynamic>std::numeric_limits<std::size_t>::max()-sizeof(LayerAppearance)
                ? std::numeric_limits<std::size_t>::max() : dynamic+sizeof(LayerAppearance);
        }
        const auto *text=std::get_if<std::string>(&value);
        return text ? text->capacity() : std::size_t{};
    };
    std::size_t total=sizeof(*this);
    for (const auto size:{id.capacity(),extra(before),extra(after)}) {
        if (size>std::numeric_limits<std::size_t>::max()-total) return std::numeric_limits<std::size_t>::max();
        total+=size;
    }
    return total;
}
LayerPropertyGuard::LayerPropertyGuard(Document &document, Layer &layer, LayerChangeField field)
    : document_(document), properties_(layerProperties(layer)), field_(field),
      value_(LayerChange::read(document,layer,field)), authorship_(document.authorship),
      version_(document.formatVersion)
{
    if (!LayerChange::swapProperty(properties_,field_,value_))
        throw std::invalid_argument("a layer property guard requires a property field");
    static_assert(std::is_nothrow_swappable_v<DocumentAuthorship>);
    using std::swap;
    swap(document_.authorship,authorship_);
}
LayerPropertyGuard::~LayerPropertyGuard() noexcept
{
    if (!active_) return;
    if (!LayerChange::swapProperty(properties_,field_,value_)) std::terminate();
    using std::swap;
    swap(document_.authorship,authorship_);
    document_.formatVersion=version_;
}

namespace {
using Field = LayerChangeField;
using Value = LayerChangeValue;
struct LayerStructureChange {
    Layer layer;
    std::vector<KeyframePlacement> keys;
    std::optional<std::string> previous, next;
    std::size_t index = 0;
    bool inserted = false;
    std::optional<SelectionLayerRelations> relationsBefore, relationsAfter;
};
struct AssetLayerChange {
    LayerStructureChange layer;
    detail::AssetHistorySnapshot asset;
    std::optional<std::string> previousAsset, nextAsset;
    std::size_t assetIndex=0;
};
// Detached before/after layer structure: raster and other assets remain owned
// by the document and are never copied into an order/translation history entry.
struct LayerBatchChange { std::vector<Layer> before, after; };
// Only changed assets are serialized into history. Other document assets are
// retained in place, including shared inputs and potentially large media.
// Both immutable snapshots own exact path and expression values for replay.
struct VectorAssetChange { detail::AssetHistorySnapshot before,after; };
// Canvas geometry snapshots exclude assets and frame payloads. Immutable
// selection masks may be shared until a translated replacement is prepared.
struct CanvasGeometryChange {
    CanvasMode mode=CanvasMode::Finite;
    CanvasRegion before{},after{};
    std::vector<Layer> beforeLayers,afterLayers;
    std::vector<Artboard> beforeArtboards,afterArtboards;
    SelectionLayerRelations beforeRelations,afterRelations;
    struct AssetPair { detail::AssetHistorySnapshot before,after; };
    std::vector<AssetPair> assets;
};
struct LayerTransactionAsset {
    std::string id;
    std::size_t index=0;
    std::vector<std::uint8_t> bytes;
};
struct LayerTransactionChange {
    std::vector<Layer> beforeLayers,afterLayers;
    std::vector<Frame> beforeFrames,afterFrames;
    std::vector<Artboard> beforeArtboards,afterArtboards;
    SelectionLayerRelations beforeRelations,afterRelations;
    std::vector<std::string> beforeAssets,afterAssets;
    std::vector<LayerTransactionAsset> removed,added;
    // Unchanged assets on which this transaction depends. Exact immutable
    // snapshots keep undo and redo from adopting untracked source edits.
    std::vector<detail::AssetHistorySnapshot> guards;
};
struct TimelineChange { Timeline before, after; };
struct PixelAspectChange { DocumentPixelAspectRatio before,after; };
struct LinkedCanvasChange { LinkedCanvasAsset before, after; };
struct ProceduralContentChange { Asset before, after; };
struct RelationsChange { SelectionLayerRelations before, after; };
struct SequenceProjectChange {
    std::optional<TimelineProject> before, after;
    std::vector<TimelineOwnedMedia> mediaBefore, mediaAfter;
};
struct Entry {
    std::string label;
    std::size_t bytes = 0;
    std::uint64_t id = 0;
    std::variant<std::monostate, std::unique_ptr<BitmapEditor>,
                 std::unique_ptr<ChunkedBitmapEditor>, LayerChange,
                 std::unique_ptr<LayerStructureChange>,std::unique_ptr<AssetLayerChange>,
                 TimelineChange, std::unique_ptr<LinkedCanvasChange>, std::unique_ptr<RelationsChange>,
                 std::unique_ptr<ProceduralContentChange>, std::unique_ptr<LayerBatchChange>,
                 std::unique_ptr<LayerTransactionChange>, std::unique_ptr<VectorAssetChange>,
                 std::unique_ptr<CanvasGeometryChange>,PixelAspectChange,
                 std::unique_ptr<SequenceProjectChange>> change;
};
bool addSize(std::size_t &total, std::size_t value) noexcept {
    if (value > std::numeric_limits<std::size_t>::max() - total) return false;
    total += value; return true;
}
bool addElements(std::size_t &total,std::size_t count,std::size_t width) noexcept {
    return !width || (count<=std::numeric_limits<std::size_t>::max()/width && addSize(total,count*width));
}
// Count retained identifiers, structures and mask payloads conservatively;
// shared immutable masks may be charged twice rather than under-budgeted.
bool relationsSize(std::size_t &total,const SelectionLayerRelations &value) noexcept {
    for (const auto &c:value.selections.channels())
        if (!addSize(total,sizeof(c)) || !addSize(total,c.id.size()) || !addSize(total,c.name.size()) ||
            (c.mask && !addSize(total,c.mask->storage_bytes()))) return false;
    for (const auto &g:value.groups.groups())
        if (!addSize(total,sizeof(g)) || !addSize(total,g.id.size()) || !addSize(total,g.name.size()) ||
            !addSize(total,g.parentId.size()) || !addSize(total,sizeof(std::pair<const std::string,std::size_t>)) ||
            !addSize(total,g.id.size())) return false;
    for (const auto &m:value.groups.memberships())
        if (!addSize(total,sizeof(m)) || !addSize(total,m.layerId.size()) || !addSize(total,m.groupId.size())) return false;
    for (const auto &e:value.links.edges())
        if (!addSize(total,sizeof(e)) || !addSize(total,e.first.size()) || !addSize(total,e.second.size())) return false;
    for (const auto *entries:{&value.locks.layers(),&value.locks.groups()})
        for (const auto &e:*entries) if (!addSize(total,sizeof(e)) || !addSize(total,e.id.size())) return false;
    return true;
}
bool bitmapInputSize(const Asset &asset,std::size_t &bytes) noexcept {
    const auto addPixels=[&](const RasterLayer &pixels) {
        return pixels.pixels.size()<=std::numeric_limits<std::size_t>::max()/sizeof(std::uint32_t) &&
               addSize(bytes,pixels.pixels.size()*sizeof(std::uint32_t));
    };
    if (const auto *raster=std::get_if<RasterAsset>(&asset)) return addPixels(raster->pixels);
    if (const auto *chunks=std::get_if<ChunkedRasterAsset>(&asset))
        for (const auto &chunk:chunks->chunks) if (!addPixels(chunk.pixels)) return false;
    if (const auto *video=std::get_if<VideoAsset>(&asset))
        for (const auto &frame:video->frames) if (!addPixels(frame)) return false;
    return true;
}
bool denseControlAsset(const Asset &asset) noexcept {
    if (asset.valueless_by_exception()) return false;
    return std::visit([](const auto &value) {
        return detail::isDenseControlAsset<std::decay_t<decltype(value)>>;
    },asset);
}
bool sameTimeline(const Timeline &a,const Timeline &b) noexcept {
    const auto &[rateA,countA]=a; const auto &[rateB,countB]=b;
    const auto &[numeratorA,denominatorA]=rateA; const auto &[numeratorB,denominatorB]=rateB;
    return numeratorA==numeratorB && denominatorA==denominatorB && countA==countB;
}
bool validTransform(const AffineTransform &v) noexcept {
    return std::isfinite(v.m11) && std::isfinite(v.m12) && std::isfinite(v.m21) &&
           std::isfinite(v.m22) && std::isfinite(v.translationX) && std::isfinite(v.translationY);
}
bool sameLayer(const Layer &a,const Layer &b) {
    if (a.valueless_by_exception() || b.valueless_by_exception() || a.index()!=b.index()) return false;
    const auto &x=layerProperties(a), &y=layerProperties(b);
    if (x.id!=y.id || x.name!=y.name || x.visible!=y.visible || x.opacity!=y.opacity ||
        !sameTransform(x.transform,y.transform) || x.blendMode!=y.blendMode ||
        x.frameRange!=y.frameRange || x.motion!=y.motion || x.artboardId!=y.artboardId ||
        x.appearance!=y.appearance) return false;
    const auto *as=staticLayerSource(a), *bs=staticLayerSource(b);
    if (bool(as)!=bool(bs)) return false;
    if (as) { if (as->assetId!=bs->assetId) return false; }
    else {
        const auto *ak=keyframedLayerSource(a), *bk=keyframedLayerSource(b);
        if (!ak || !bk || ak->frameIndices!=bk->frameIndices) return false;
    }
    const auto *ac=controlNetSettings(a), *bc=controlNetSettings(b);
    if (bool(ac)!=bool(bc) || (ac && !(*ac==*bc))) return false;
    if (const auto *video=std::get_if<VideoLayer>(&a))
        if (!(video->playback==std::get<VideoLayer>(b).playback)) return false;
    if (const auto *semantic=std::get_if<SemanticSegmentLayer>(&a))
        if (!(semantic->segmentation==std::get<SemanticSegmentLayer>(b).segmentation)) return false;
    if (const auto *reference=std::get_if<ReferenceLayer>(&a))
        if (!(reference->reference==std::get<ReferenceLayer>(b).reference)) return false;
    return true;
}

// Logical retained storage for the copied structural value. It counts owned
// strings and populated vector elements, not capacities or allocator overhead.
bool layerStructureSize(std::size_t &total,const Layer &layer,
                        const std::vector<KeyframePlacement> &keys) noexcept {
    const auto text=[&](const std::string &s) { return addSize(total,s.size()); };
    const auto &p=layerProperties(layer); const auto &a=p.appearance;
    if (!text(p.id) || !text(p.name) || (p.artboardId && !text(*p.artboardId)) ||
        !addElements(total,p.motion.size(),sizeof(MotionKeyframe)) ||
        !addElements(total,a.filters.size(),sizeof(LayerFilter)) ||
        !addElements(total,a.styles.effects.size(),sizeof(LayerStyle)) ||
        !addElements(total,a.maskStack.size(),sizeof(LayerMaskEntry))) return false;
    for (const auto *mask:{&a.rasterMask,&a.vectorMask,&a.filterMask})
        if (*mask && !text((*mask)->assetId)) return false;
    for (const auto &filter:a.filters) if (!text(filter.name)) return false;
    for (const auto &entry:a.maskStack)
        if (!text(entry.id) || !text(entry.name) || !text(entry.mask.assetId)) return false;
    if (const auto *source=staticLayerSource(layer)) { if (!text(source->assetId)) return false; }
    else if (const auto *source=keyframedLayerSource(layer)) {
        if (!addElements(total,source->frameIndices.size(),sizeof(FrameIndex))) return false;
    }
    if (const auto *control=controlNetSettings(layer))
        if (!text(control->modelId) || !text(control->modelRevision)) return false;
    if (const auto *semantic=std::get_if<SemanticSegmentLayer>(&layer)) {
        const auto &s=semantic->segmentation; const auto &t=s.taxonomy;
        if (!text(t.id) || !text(t.version) || !text(t.sourceUri) ||
            !addElements(total,t.classes.size(),sizeof(SemanticClass)) ||
            !addElements(total,s.regions.size(),sizeof(SemanticRegion))) return false;
        for (const auto &c:t.classes) {
            if (!text(c.key) || !text(c.name) || !text(c.description) || !text(c.category) ||
                !text(c.externalId) || !addElements(total,c.aliases.size(),sizeof(std::string))) return false;
            for (const auto &alias:c.aliases) if (!text(alias)) return false;
        }
        for (const auto &region:s.regions) {
            if (!text(region.name) || !text(region.description) || !text(region.generator) ||
                !text(region.sourceReference) || !addElements(total,region.attributes.size(),sizeof(SemanticAttribute))) return false;
            for (const auto &attribute:region.attributes)
                if (!text(attribute.key) || !text(attribute.value)) return false;
        }
    }
    if (!addElements(total,keys.size(),sizeof(KeyframePlacement))) return false;
    for (const auto &key:keys) if (!text(key.assetId)) return false;
    return true;
}
bool sameValue(Field field, const Value &a, const Value &b) {
    return LayerChange::same(field,a,b);
}
Value currentValue(Field field, const Document &document, const Layer &layer) {
    return LayerChange::read(document,layer,field);
}
DocumentEditResult editValue(DocumentEditor &editor, const LayerChange &change, const Value &value) {
    return change.applyValue(editor,value);
}

// In-memory DocumentEditor operations can allocate while updating authorship.
// Work on copies of only the affected structural values and restore the exact
// original buffers on rejection or exception, without copying raster assets.
struct LayersGuard {
    Document &document; std::vector<Layer> saved; DocumentAuthorship author;
    FormatVersion version; bool published = false;
    explicit LayersGuard(Document &d) : document(d),saved(d.layers),author(d.authorship),version(d.formatVersion) {
        using std::swap; swap(document.layers,saved); swap(document.authorship,author);
    }
    ~LayersGuard() noexcept {
        if (!published) { using std::swap; swap(document.layers,saved); swap(document.authorship,author); document.formatVersion=version; }
    }
};
// Restore every touched geometry value and authorship on validation, allocation
// or durable-commit failure. Assets, timeline and frame records remain untouched.
struct CanvasGeometryGuard {
    Document &document;
    CanvasExtent extent;
    InfiniteCanvas infinite;
    std::vector<Layer> layers;
    std::vector<Artboard> artboards;
    SelectionLayerRelations relations;
    DocumentAuthorship author;
    FormatVersion version;
    bool published=false;
    explicit CanvasGeometryGuard(Document &d)
        :document(d),extent(d.extent),infinite(d.infiniteCanvas),layers(d.layers),
         artboards(d.artboards),relations(d.relations),author(d.authorship),version(d.formatVersion) {
        using std::swap; swap(document.layers,layers); swap(document.artboards,artboards);
        swap(document.relations,relations); swap(document.authorship,author);
    }
    ~CanvasGeometryGuard() noexcept {
        if (!published) {
            using std::swap; document.extent=extent; document.infiniteCanvas=infinite;
            swap(document.layers,layers); swap(document.artboards,artboards);
            swap(document.relations,relations); swap(document.authorship,author); document.formatVersion=version;
        }
    }
};
struct StructureGuard {
    Document &document; std::vector<Layer> layers; std::optional<std::vector<Frame>> frames;
    std::optional<SelectionLayerRelations> relations;
    DocumentAuthorship author; FormatVersion version; bool published = false;
    StructureGuard(Document &d,bool changesFrames,bool changesRelations=false)
        : document(d),layers(d.layers),author(d.authorship),version(d.formatVersion) {
        if (changesFrames) frames.emplace(d.frames);
        if (changesRelations) relations.emplace(d.relations);
        using std::swap; swap(document.layers,layers);
        if (frames) swap(document.frames,*frames);
        swap(document.authorship,author);
    }
    ~StructureGuard() noexcept {
        if (!published) {
            using std::swap; swap(document.layers,layers);
            if (frames) swap(document.frames,*frames);
            if (relations) swap(document.relations,*relations);
            swap(document.authorship,author); document.formatVersion=version;
        }
    }
};
static_assert(std::is_nothrow_swappable_v<LayerProperties>);
static_assert(std::is_nothrow_swappable_v<DocumentAuthorship>);
static_assert(std::is_nothrow_swappable_v<std::vector<Layer>>);
static_assert(std::is_nothrow_swappable_v<std::vector<Frame>>);

bool sameKeys(const Document &document,std::string_view id,const std::vector<KeyframePlacement> &keys) noexcept {
    std::size_t index=0;
    for (const auto &frame:document.frames) for (const auto &key:frame.keyframes) {
        if (key.layerId!=id) continue;
        if (index>=keys.size() || keys[index].frame!=frame.index || keys[index].assetId!=key.assetId) return false;
        ++index;
    }
    return index==keys.size();
}
bool sameNeighbours(const Document &document,const LayerStructureChange &change,bool containsLayer) noexcept {
    if (change.index>document.layers.size() || (containsLayer && change.index==document.layers.size())) return false;
    if (change.previous) {
        if (!change.index || layerProperties(document.layers[change.index-1]).id!=*change.previous) return false;
    } else if (change.index!=0) return false;
    const auto following=change.index+(containsLayer?1:0);
    if (change.next) {
        if (following>=document.layers.size() || layerProperties(document.layers[following]).id!=*change.next) return false;
    } else if (following!=document.layers.size()) return false;
    return true;
}
bool sameAssetNeighbours(const Document &document,const AssetLayerChange &change,bool containsAsset) noexcept {
    const auto index=change.assetIndex;
    if (index>document.assets.size() || (containsAsset && index==document.assets.size())) return false;
    if (change.previousAsset) {
        if (!index || assetId(document.assets[index-1])!=*change.previousAsset) return false;
    } else if (index!=0) return false;
    const auto following=index+(containsAsset?1:0);
    if (change.nextAsset) {
        if (following>=document.assets.size() || assetId(document.assets[following])!=*change.nextAsset) return false;
    } else if (following!=document.assets.size()) return false;
    return true;
}

DocumentHistoryCode fileFailure(const DocumentFile *file) noexcept {
    if (!file) return DocumentHistoryCode::EditRejected;
    switch (file->lastResult().code) {
    case DocumentFileCode::Conflict: return DocumentHistoryCode::Conflict;
    case DocumentFileCode::NotOpen: return DocumentHistoryCode::NotBound;
    case DocumentFileCode::LimitExceeded: return DocumentHistoryCode::LimitExceeded;
    case DocumentFileCode::None: case DocumentFileCode::EditRejected: return DocumentHistoryCode::EditRejected;
    default: return DocumentHistoryCode::PersistenceFailure;
    }
}
DocumentHistoryCode bitmapFailure(RasterHistoryCode code, const DocumentFile *file) noexcept {
    if (file && !file->lastResult().ok() && file->lastResult().code != DocumentFileCode::EditRejected)
        return fileFailure(file);
    switch (code) {
    case RasterHistoryCode::BudgetExceeded: return DocumentHistoryCode::BudgetExceeded;
    case RasterHistoryCode::InvalidArgument: return DocumentHistoryCode::InvalidArgument;
    case RasterHistoryCode::Conflict: return DocumentHistoryCode::Conflict;
    case RasterHistoryCode::NoUndo: return DocumentHistoryCode::NoUndo;
    case RasterHistoryCode::NoRedo: return DocumentHistoryCode::NoRedo;
    case RasterHistoryCode::Reentrant: return DocumentHistoryCode::Reentrant;
    default: return DocumentHistoryCode::EditRejected;
    }
}
DocumentHistoryCode layerFailure(const DocumentEditResult &result, const DocumentFile *file) noexcept {
    if (result.code == DocumentEditCode::PersistenceFailed) return fileFailure(file);
    if (result.code == DocumentEditCode::NotBound) return DocumentHistoryCode::NotBound;
    return DocumentHistoryCode::InvalidArgument;
}
} // namespace

class DocumentHistory::Impl final {
public:
    Document *memory = nullptr;
    DocumentFile *file = nullptr;
    std::uint64_t generation = 0, expectedRevision = 0;
    std::uint64_t nextEntryId = 1;
    DocumentHistoryLimits limits;
    bool valid = false, busy = false;
    std::vector<std::unique_ptr<Entry>> undo, redo;
    std::size_t retained = 0;
    std::unique_ptr<Entry> preview;
    enum class PreviewKind { None, Pixels, Stroke };
    PreviewKind previewKind = PreviewKind::None;
    std::size_t previewOverhead = 0;
    DevicePixelRect previewDirty{};
    DevicePixelRect strokeDirty{};
    double strokeTime = 0;
    std::uint32_t nextStrokeSeed = 1;

    explicit Impl(Document &d, DocumentHistoryLimits l) : memory(&d),limits(l) {
        DocumentEditor probe(d); valid=probe.isBound(); expectedRevision=d.authorship.revision();
    }
    explicit Impl(DocumentFile &f, DocumentHistoryLimits l) : file(&f),generation(f.bindingGeneration()),limits(l) {
        valid=f.isOpen() && f.document(); expectedRevision=f.revision();
    }
    const Document *document() const noexcept { return file ? file->document() : memory; }
    bool bound() const noexcept {
        return valid && (file ? file->isOpen() && file->bindingGeneration()==generation : memory!=nullptr);
    }
    bool policyValid() const noexcept {
        return limits.overflow==DocumentHistoryOverflow::Reject || limits.overflow==DocumentHistoryOverflow::EvictOldest;
    }
    std::uint64_t revision() const noexcept {
        return file ? file->revision() : memory->authorship.revision();
    }
    DocumentHistoryCode current() const noexcept {
        if (!bound()) return DocumentHistoryCode::NotBound;
        if (!policyValid()) return DocumentHistoryCode::InvalidArgument;
        return revision()==expectedRevision ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
    }
    std::size_t undoBytes() const noexcept {
        std::size_t total=0; for (const auto &entry:undo) total+=entry->bytes; return total;
    }
    template <class Function> static decltype(auto) withBitmap(Entry &entry, Function &&function) {
        if (const auto *editor=std::get_if<std::unique_ptr<BitmapEditor>>(&entry.change))
            return function(**editor);
        return function(*std::get<std::unique_ptr<ChunkedBitmapEditor>>(entry.change));
    }
    template <class Function> decltype(auto) withPreview(Function &&function) const {
        return withBitmap(*preview,std::forward<Function>(function));
    }
    void collectPreviewDirty() noexcept {
        withPreview([this](auto &editor) {
            auto &dirty=previewKind==PreviewKind::Stroke ? strokeDirty : previewDirty;
            dirty=uniteDevicePixelRects(dirty,editor.dirtyBounds());
            editor.clearDirtyBounds();
        });
    }
    std::size_t rasterAllowance(std::size_t overhead) const noexcept {
        if (!limits.maximumEntries || (limits.overflow==DocumentHistoryOverflow::Reject && undo.size()>=limits.maximumEntries)) return 0;
        std::size_t available=limits.maximumBytes;
        if (limits.overflow==DocumentHistoryOverflow::Reject) {
            const auto used=undoBytes(); if (used>available) return 0; available-=used;
        }
        return overhead<=available ? available-overhead : 0;
    }
    bool canAdmit(std::size_t bytes) const noexcept {
        if (!nextEntryId || !limits.maximumEntries || bytes>limits.maximumBytes) return false;
        if (limits.overflow==DocumentHistoryOverflow::EvictOldest) return true;
        const auto used=undoBytes();
        return undo.size()<limits.maximumEntries && used<=limits.maximumBytes && bytes<=limits.maximumBytes-used;
    }
    DocumentHistoryResult publish(std::unique_ptr<Entry> entry) noexcept {
        entry->id = nextEntryId++;
        const auto discarded=redo.size();
        for (const auto &item:redo) retained-=item->bytes;
        redo.clear();
        std::size_t evicted=0;
        while (undo.size()>=limits.maximumEntries || entry->bytes>limits.maximumBytes-retained) {
            retained-=undo.front()->bytes; undo.erase(undo.begin()); ++evicted;
        }
        const auto bytes=entry->bytes; retained+=bytes;
        undo.push_back(std::move(entry)); // Capacity is prepared before mutation.
        expectedRevision=revision();
        return {DocumentHistoryCode::None,true,bytes,evicted,discarded};
    }
    DocumentHistoryCode applyLayer(const LayerChange &change, bool forward) {
        const auto *d=document(); const auto *layer=d ? findLayer(*d,change.id) : nullptr;
        if (!layer) return DocumentHistoryCode::Conflict;
        const Value &expected=forward?change.before:change.after;
        const Value &next=forward?change.after:change.before;
        if (!sameValue(change.field,currentValue(change.field,*d,*layer),expected)) return DocumentHistoryCode::Conflict;
        if (file) {
            DocumentEditor editor(*file);
            if (!editor.isBound()) return DocumentHistoryCode::NotBound;
            const auto result=editValue(editor,change,next);
            if (!result.ok()) return layerFailure(result,file);
            return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        if (change.field==Field::Order) {
            LayersGuard guard(*memory);
            DocumentEditor editor(*memory);
            const auto result=editValue(editor,change,next);
            if (!result.ok()) return layerFailure(result,nullptr);
            if (!result.changed) return DocumentHistoryCode::Conflict;
            guard.published=true; return DocumentHistoryCode::None;
        }
        if (change.field==Field::Appearance) {
            // The editor transaction restores the old value, authorship and
            // revision even when validation or allocation fails.
            DocumentEditor editor(*memory);
            const auto result=editValue(editor,change,next);
            if (!result.ok()) return layerFailure(result,nullptr);
            return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        auto *editable=findLayer(*memory,change.id);
        LayerPropertyGuard guard(*memory,*editable,change.field);
        DocumentEditor editor(*memory);
        const auto result=editValue(editor,change,next);
        if (!result.ok()) return layerFailure(result,nullptr);
        if (!result.changed) return DocumentHistoryCode::Conflict;
        guard.commit(); return DocumentHistoryCode::None;
    }
    DocumentHistoryResult layer(Field field, std::string_view id, Value next, std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const std::string key(id);
            const auto *d=document(); const auto *item=findLayer(*d,key);
            if (!item) return {DocumentHistoryCode::InvalidArgument};
            if ((field==Field::Name && !ii::core::valid_utf8(std::get<std::string>(next))) ||
                (field==Field::Opacity && (!std::isfinite(std::get<double>(next)) || std::get<double>(next)<0 || std::get<double>(next)>1)) ||
                (field==Field::Transform && !validTransform(std::get<AffineTransform>(next))) ||
                (field==Field::Blend && (std::get<RasterBlendMode>(next)==RasterBlendMode::DestinationOut ||
                    static_cast<unsigned>(std::get<RasterBlendMode>(next))>static_cast<unsigned>(RasterBlendMode::Dissolve))) ||
                (field==Field::Order && std::get<std::size_t>(next)>=d->layers.size()))
                return {DocumentHistoryCode::InvalidArgument};
            auto previous=currentValue(field,*d,*item);
            if (sameValue(field,previous,next)) {
                // A file-bound no-op still checks the backing store's external
                // writer token before reporting success. It must not discard redo.
                if (file) {
                    DocumentEditor editor(*file);
                    if (!editor.isBound()) return {DocumentHistoryCode::NotBound};
                    const auto result=editValue(editor,LayerChange{key,field,previous,next},next);
                    if (!result.ok()) return {layerFailure(result,file)};
                }
                return {};
            }
            std::size_t bytes=sizeof(Entry);
            if (!addSize(bytes,id.size()) || !addSize(bytes,label.size())) return {DocumentHistoryCode::BudgetExceeded};
            if (field==Field::Name && (!addSize(bytes,std::get<std::string>(previous).size()) ||
                                       !addSize(bytes,std::get<std::string>(next).size())))
                return {DocumentHistoryCode::BudgetExceeded};
            if (field==Field::Appearance) {
                const auto &before=std::get<LayerAppearanceValue>(previous), &after=std::get<LayerAppearanceValue>(next);
                if (!before || !after || !addSize(bytes,2*sizeof(LayerAppearance)) ||
                    !addSize(bytes,appearanceExtra(*before)) || !addSize(bytes,appearanceExtra(*after)))
                    return {DocumentHistoryCode::BudgetExceeded};
            }
            if (!canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<LayerChange>(LayerChange{key,field,std::move(previous),std::move(next)});
            const auto result=applyLayer(std::get<LayerChange>(entry->change),true);
            if (result!=DocumentHistoryCode::None) return {result};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    // Apply a detached layer-only candidate after checking both its original
    // state and inherited edit permissions. File publication goes through one
    // validated working-file transaction, so no partial batch becomes visible.
    DocumentHistoryCode applyLayerBatch(const LayerBatchChange &change,bool forward) {
        const auto *currentDocument=document();
        if (!currentDocument) return DocumentHistoryCode::NotBound;
        const auto &before=forward ? change.before : change.after;
        const auto &after=forward ? change.after : change.before;
        if (currentDocument->layers.size()!=before.size() || before.size()!=after.size())
            return DocumentHistoryCode::Conflict;
        for (std::size_t i=0;i<before.size();++i)
            if (!sameLayer(currentDocument->layers[i],before[i])) return DocumentHistoryCode::Conflict;
        bool orderOnly=true;
        for (std::size_t i=0;i<before.size();++i) {
            const auto &oldLayer=before[i]; const auto &old=layerProperties(oldLayer);
            const auto position=std::find_if(after.begin(),after.end(),[&](const Layer &layer) {
                return layerProperties(layer).id==old.id;
            });
            if (position==after.end()) return DocumentHistoryCode::InvalidArgument;
            const auto &replacement=layerProperties(*position);
            if (layerProperties(after[i]).id!=old.id &&
                hasLayerLock(currentDocument->relations.locks.effective(*currentDocument,old.id),LayerLock::Structure))
                return DocumentHistoryCode::EditRejected;
            if (!sameTransform(old.transform,replacement.transform) &&
                hasLayerLock(currentDocument->relations.locks.effective(*currentDocument,old.id),LayerLock::Transform))
                return DocumentHistoryCode::EditRejected;
            if (!sameLayer(oldLayer,*position)) orderOnly=false;
        }
        DocumentHistoryCode failure=DocumentHistoryCode::None;
        const auto mutate=[&](Document &target) {
            target.layers=after;
            target.formatVersion.minor=CurrentFormatMinor;
            if (!validate(target).ok()) { failure=DocumentHistoryCode::InvalidArgument; return false; }
            recordDocumentChange(target); return true;
        };
        if (file) {
            DocumentFileResult result;
#ifdef IISHAREDCANVAS_NATIVE_API
            bool fallback=false;
            if (orderOnly) result=file->editLayerStructureImpl(mutate,false,fallback);
            else fallback=true;
            if (fallback) result=file->edit(mutate);
#else
            result=file->edit(mutate);
#endif
            if (!result.ok()) return failure!=DocumentHistoryCode::None ? failure : fileFailure(file);
            return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        LayersGuard guard(*memory);
        if (!mutate(*memory)) return failure;
        guard.published=true; return DocumentHistoryCode::None;
    }

    // Charge both layer snapshots, prepare the history slot, and publish only
    // after the complete batch has passed validation and durable commit.
    DocumentHistoryResult commitLayerBatch(std::vector<Layer> after,std::string_view label) {
        const auto *d=document();
        if (!d || d->layers.size()!=after.size()) return {DocumentHistoryCode::InvalidArgument};
        bool same=true;
        for (std::size_t i=0;i<after.size();++i) if (!sameLayer(d->layers[i],after[i])) { same=false; break; }
        if (same) {
            if (file) { const auto result=file->edit([](Document &) { return true; });
                if (!result.ok()) return {fileFailure(file)}; }
            return {};
        }
        std::size_t bytes=sizeof(Entry)+sizeof(LayerBatchChange);
        if (!addSize(bytes,label.size()) || !addElements(bytes,d->layers.size(),2*sizeof(Layer)))
            return {DocumentHistoryCode::BudgetExceeded};
        const std::vector<KeyframePlacement> keys;
        for (const auto *layers:std::array<const std::vector<Layer> *,2>{&d->layers,&after}) for (const auto &layer:*layers)
            if (!layerStructureSize(bytes,layer,keys)) return {DocumentHistoryCode::BudgetExceeded};
        if (!canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
        undo.reserve(undo.size()+1);
        auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
        entry->change.emplace<std::unique_ptr<LayerBatchChange>>(
            std::make_unique<LayerBatchChange>(LayerBatchChange{d->layers,std::move(after)}));
        const auto code=applyLayerBatch(*std::get<std::unique_ptr<LayerBatchChange>>(entry->change),true);
        if (code!=DocumentHistoryCode::None) return {code};
        return publish(std::move(entry));
    }
    DocumentHistoryResult layerOrder(std::span<const std::string> order,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const auto *d=document();
            if (order.size()!=d->layers.size()) return {DocumentHistoryCode::InvalidArgument};
            std::unordered_set<std::string_view> seen;
            std::vector<Layer> layers; layers.reserve(order.size());
            for (const auto &id:order) {
                const auto *layer=findLayer(*d,id);
                if (!layer || !seen.insert(id).second) return {DocumentHistoryCode::InvalidArgument};
                layers.push_back(*layer);
            }
            return commitLayerBatch(std::move(layers),label);
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryCode applyPixelAspect(const PixelAspectChange &change,bool forward) {
        const auto *d=document(); if (!d) return DocumentHistoryCode::NotBound;
        const auto expected=forward ? change.before : change.after;
        const auto next=forward ? change.after : change.before;
        if (d->pixelAspectRatio!=expected) return DocumentHistoryCode::Conflict;
        DocumentHistoryCode failure=DocumentHistoryCode::None;
        const auto mutate=[&](Document &target) {
            const auto before=target.pixelAspectRatio; const auto version=target.formatVersion;
            auto author=target.authorship;
            struct Guard {
                Document &document; DocumentPixelAspectRatio value; FormatVersion version;
                DocumentAuthorship &author; bool published=false;
                ~Guard() noexcept { if (!published) { using std::swap; document.pixelAspectRatio=value;
                    document.formatVersion=version; swap(document.authorship,author); } }
            } guard{target,before,version,author};
            target.pixelAspectRatio=next; target.formatVersion.minor=CurrentFormatMinor;
            if (!validate(target).ok()) { failure=DocumentHistoryCode::InvalidArgument; return false; }
            recordDocumentChange(target); guard.published=true; return true;
        };
        if (file) {
            const auto result=file->edit(mutate);
            if (!result.ok()) return failure!=DocumentHistoryCode::None ? failure : fileFailure(file);
            return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        return mutate(*memory) ? DocumentHistoryCode::None : failure;
    }
    DocumentHistoryResult pixelAspect(DocumentPixelAspectRatio next,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        if (!next.numerator || !next.denominator) return {DocumentHistoryCode::InvalidArgument};
        try {
            const auto *d=document(); if (!d) return {DocumentHistoryCode::NotBound};
            if (next==d->pixelAspectRatio) {
                if (file) { const auto result=file->edit([](Document &) { return true; });
                    if (!result.ok()) return {fileFailure(file)}; }
                return {};
            }
            std::size_t bytes=sizeof(Entry);
            if (!addSize(bytes,label.size()) || !canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<PixelAspectChange>(PixelAspectChange{d->pixelAspectRatio,next});
            const auto result=applyPixelAspect(std::get<PixelAspectChange>(entry->change),true);
            if (result!=DocumentHistoryCode::None) return {result};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryCode applyCanvasGeometryChange(const CanvasGeometryChange &change,bool forward) {
        const auto *d=document(); if (!d) return DocumentHistoryCode::NotBound;
        const auto &expected=forward ? change.before : change.after;
        const auto &next=forward ? change.after : change.before;
        const auto &expectedLayers=forward ? change.beforeLayers : change.afterLayers;
        const auto &nextLayers=forward ? change.afterLayers : change.beforeLayers;
        const auto &expectedBoards=forward ? change.beforeArtboards : change.afterArtboards;
        const auto &nextBoards=forward ? change.afterArtboards : change.beforeArtboards;
        const auto &expectedRelations=forward ? change.beforeRelations : change.afterRelations;
        const auto &nextRelations=forward ? change.afterRelations : change.beforeRelations;
        if (d->canvasMode!=change.mode || canvasRegion(*d)!=expected || d->layers.size()!=expectedLayers.size() ||
            d->artboards!=expectedBoards || d->relations!=expectedRelations) return DocumentHistoryCode::Conflict;
        for (std::size_t i=0;i<d->layers.size();++i)
            if (!sameLayer(d->layers[i],expectedLayers[i])) return DocumentHistoryCode::Conflict;
        for (const auto &asset:change.assets) {
            const auto &expectedAsset=forward ? asset.before : asset.after;
            const auto *current=findAsset(*d,std::string(expectedAsset.id()));
            if (!current || !expectedAsset.matches(*current)) return DocumentHistoryCode::Conflict;
            const auto protection=d->relations.locks.assetLocks(*d,std::string(expectedAsset.id()));
            if (hasLayerLock(protection,LayerLock::Pixels) || hasLayerLock(protection,LayerLock::Transparency))
                return DocumentHistoryCode::EditRejected;
        }
        DocumentHistoryCode failure=DocumentHistoryCode::None;
        const auto mutate=[&](Document &target) {
            struct AssetGuard {
                Document &document;
                std::vector<std::pair<std::size_t,Asset>> replacements;
                bool applied=false,published=false;
                ~AssetGuard() noexcept {
                    if (applied && !published) for (auto &[index,asset]:replacements) std::swap(document.assets[index],asset);
                }
            } assets{target};
            assets.replacements.reserve(change.assets.size());
            for (const auto &asset:change.assets) {
                const auto &nextAsset=forward ? asset.after : asset.before;
                auto *location=findAsset(target,std::string(nextAsset.id()));
                if (!location) { failure=DocumentHistoryCode::Conflict; assets.published=true; return false; }
                assets.replacements.emplace_back(std::size_t(location-target.assets.data()),nextAsset.restore());
            }
            CanvasGeometryGuard guard(target);
            assets.applied=true;
            for (auto &[index,asset]:assets.replacements) std::swap(target.assets[index],asset);
            target.extent=next.extent;
            if (change.mode==CanvasMode::Infinite) target.infiniteCanvas.origin=next.origin;
            target.layers=nextLayers; target.artboards=nextBoards; target.relations=nextRelations;
            target.formatVersion.minor=CurrentFormatMinor;
            if (!validate(target).ok()) { failure=DocumentHistoryCode::InvalidArgument; return false; }
            recordDocumentChange(target); guard.published=true; assets.published=true; return true;
        };
        if (file) {
            const auto result=file->edit(mutate);
            if (!result.ok()) return failure!=DocumentHistoryCode::None ? failure : fileFailure(file);
            return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        return mutate(*memory) ? DocumentHistoryCode::None : failure;
    }
    DocumentHistoryResult canvasGeometry(const CanvasGeometryPlan &plan,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const auto *d=document();
            if (!d || plan.sourceRevision!=d->authorship.revision() || plan.mode!=d->canvasMode ||
                plan.source!=canvasRegion(*d)) return {DocumentHistoryCode::Conflict};
            const auto &target=plan.target; const auto &offset=plan.contentOffset;
            constexpr auto maximum=std::numeric_limits<std::int32_t>::max();
            if (target.extent.width<=0 || target.extent.height<=0 ||
                std::int64_t(target.origin.x)+target.extent.width>maximum ||
                std::int64_t(target.origin.y)+target.extent.height>maximum ||
                (plan.mode==CanvasMode::Finite && target.origin!=CanvasOrigin{}) ||
                (plan.mode==CanvasMode::Infinite && offset!=CanvasOrigin{})) return {DocumentHistoryCode::InvalidArgument};
            if (target==plan.source && offset==CanvasOrigin{}) {
                if (file) { const auto result=file->edit([](Document &) { return true; });
                    if (!result.ok()) return {fileFailure(file)}; }
                return {};
            }
            auto change=std::make_unique<CanvasGeometryChange>();
            change->mode=plan.mode; change->before=plan.source; change->after=target;
            change->beforeLayers=d->layers; change->afterLayers=d->layers;
            change->beforeArtboards=d->artboards; change->afterArtboards=d->artboards;
            change->beforeRelations=d->relations; change->afterRelations=d->relations;
            if (offset!=CanvasOrigin{}) {
                for (const auto &layer:d->layers) {
                    const auto &p=layerProperties(layer);
                    if (p.blendMode==RasterBlendMode::Dissolve ||
                        hasLayerLock(d->relations.locks.effective(*d,p.id),LayerLock::Transform))
                        return {DocumentHistoryCode::EditRejected};
                }
                for (auto &board:change->afterArtboards) {
                    const auto x=std::int64_t(board.region.origin.x)+offset.x;
                    const auto y=std::int64_t(board.region.origin.y)+offset.y;
                    if (x<std::numeric_limits<std::int32_t>::min() || x>maximum ||
                        y<std::numeric_limits<std::int32_t>::min() || y>maximum) return {DocumentHistoryCode::InvalidArgument};
                    board.region.origin={static_cast<std::int32_t>(x),static_cast<std::int32_t>(y)};
                    if (board.geometry) {
                        board.geometry->transform.translationX+=offset.x;
                        board.geometry->transform.translationY+=offset.y;
                    }
                }
                for (auto &layer:change->afterLayers) {
                    auto &p=layerProperties(layer);
                    if (p.artboardId) continue;
                    p.transform.translationX+=offset.x; p.transform.translationY+=offset.y;
                    if (!validTransform(p.transform)) return {DocumentHistoryCode::InvalidArgument};
                    const auto shift=[&](LayerMask &mask) {
                        if (!mask.linkedToLayer) {
                            mask.transform.translationX+=offset.x; mask.transform.translationY+=offset.y;
                        }
                    };
                    if (p.appearance.rasterMask) shift(*p.appearance.rasterMask);
                    if (p.appearance.vectorMask) shift(*p.appearance.vectorMask);
                    if (p.appearance.filterMask) shift(*p.appearance.filterMask);
                    for (auto &entry:p.appearance.maskStack) shift(entry.mask);
                }
                for (const auto &channel:d->relations.selections.channels()) {
                    auto data=channel.mask->snapshot();
                    const auto x=std::int64_t(data.domain.origin.x)+offset.x;
                    const auto y=std::int64_t(data.domain.origin.y)+offset.y;
                    if (x<std::numeric_limits<std::int32_t>::min() || x>maximum ||
                        y<std::numeric_limits<std::int32_t>::min() || y>maximum) return {DocumentHistoryCode::InvalidArgument};
                    data.domain.origin={static_cast<std::int32_t>(x),static_cast<std::int32_t>(y)};
                    auto moved=TiledMask::fromSnapshot(data);
                    if (!moved.ok()) return {DocumentHistoryCode::InvalidArgument};
                    NamedSelectionChannel next{channel.id,channel.name,std::make_shared<const TiledMask>(std::move(moved.mask))};
                    if (!change->afterRelations.selections.replace(channel.id,std::move(next))) return {DocumentHistoryCode::InvalidArgument};
                }
            }
            std::size_t bytes=sizeof(Entry)+sizeof(CanvasGeometryChange);
            if (!addSize(bytes,label.size()) || !addElements(bytes,d->layers.size(),2*sizeof(Layer)) ||
                !artboardBytes(bytes,change->beforeArtboards) || !artboardBytes(bytes,change->afterArtboards) ||
                !relationsSize(bytes,change->beforeRelations) || !relationsSize(bytes,change->afterRelations))
                return {DocumentHistoryCode::BudgetExceeded};
            const std::vector<KeyframePlacement> keys;
            for (const auto *layers:std::array<const std::vector<Layer> *,2>{&change->beforeLayers,&change->afterLayers})
                for (const auto &layer:*layers) if (!layerStructureSize(bytes,layer,keys)) return {DocumentHistoryCode::BudgetExceeded};
            if (!canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<std::unique_ptr<CanvasGeometryChange>>(std::move(change));
            const auto code=applyCanvasGeometryChange(*std::get<std::unique_ptr<CanvasGeometryChange>>(entry->change),true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult canvasAffine(const CanvasAffinePlan &plan,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const auto *d=document();
            if (!d || plan.sourceRevision!=d->authorship.revision() || plan.mode!=d->canvasMode ||
                plan.source!=canvasRegion(*d)) return {DocumentHistoryCode::Conflict};
            if ((plan.rasterMode!=RasterResamplingMode::PreserveSource &&
                 plan.rasterMode!=RasterResamplingMode::ReplacePixels) ||
                !canvas_affine_detail::invertible(plan.transform) || plan.target.extent.width<=0 ||
                plan.target.extent.height<=0 ||
                std::int64_t(plan.target.origin.x)+plan.target.extent.width>std::numeric_limits<std::int32_t>::max() ||
                std::int64_t(plan.target.origin.y)+plan.target.extent.height>std::numeric_limits<std::int32_t>::max() ||
                (plan.mode==CanvasMode::Finite && plan.target.origin!=CanvasOrigin{}))
                return {DocumentHistoryCode::InvalidArgument};
            const auto &t=plan.transform;
            if (plan.target==plan.source && sameTransform(t,AffineTransform{})) return {};
            const bool replacePixels=plan.rasterMode==RasterResamplingMode::ReplacePixels;
            const double sx=double(plan.target.extent.width)/plan.source.extent.width;
            const double sy=double(plan.target.extent.height)/plan.source.extent.height;
            if (replacePixels && (plan.source.origin!=plan.target.origin || t.m11!=sx || t.m22!=sy ||
                t.m12!=0 || t.m21!=0 || t.translationX!=plan.source.origin.x*(1-sx) ||
                t.translationY!=plan.source.origin.y*(1-sy))) return {DocumentHistoryCode::InvalidArgument};
            for (const auto &layer:d->layers) {
                const auto &p=layerProperties(layer);
                // Disabled or transparent effects contribute no pixels. An
                // overlay depends only on the source coverage, so it has no
                // geometric distance or direction to remap.
                const bool activeStyle=std::any_of(p.appearance.styles.effects.begin(),p.appearance.styles.effects.end(),
                    [](const LayerStyle &style) {
                        return style.enabled && style.opacity>0 && style.kind!=LayerStyleKind::Overlay;
                    });
                if (p.blendMode==RasterBlendMode::Dissolve || activeStyle ||
                    hasLayerLock(d->relations.locks.effective(*d,p.id),LayerLock::Transform))
                    return {DocumentHistoryCode::EditRejected};
                // Pointwise adjustments and ordinary blend modes commute with
                // nearest-neighbor replacement. Spatial, image-dependent and
                // position-seeded filters do not.
                const bool incompatibleFilter=std::any_of(p.appearance.filters.begin(),p.appearance.filters.end(),
                    [](const LayerFilter &filter) {
                        if (!filter.enabled || filter.opacity==0) return false;
                        if (filter.blendMode==RasterBlendMode::Dissolve) return true;
                        const auto *effect=std::get_if<RasterEffect>(&filter.operation);
                        if (!effect) return false;
                        return effect->kind!=RasterEffectKind::Invert && effect->kind!=RasterEffectKind::Grayscale &&
                            effect->kind!=RasterEffectKind::Solarize && effect->kind!=RasterEffectKind::Dehaze;
                    });
                if (replacePixels && incompatibleFilter)
                    return {DocumentHistoryCode::EditRejected};
            }
            auto change=std::make_unique<CanvasGeometryChange>();
            change->mode=plan.mode; change->before=plan.source; change->after=plan.target;
            change->beforeLayers=d->layers; change->afterLayers=d->layers;
            change->beforeArtboards=d->artboards; change->afterArtboards=d->artboards;
            change->beforeRelations=d->relations; change->afterRelations=d->relations;
            std::size_t assetBytes=0;
            struct PixelScale {
                std::uint64_t xNumerator=1,xDenominator=1,yNumerator=1,yDenominator=1;
                double x() const { return double(xNumerator)/xDenominator; }
                double y() const { return double(yNumerator)/yDenominator; }
                bool same(const PixelScale &other) const {
                    return xNumerator*other.xDenominator==other.xNumerator*xDenominator &&
                           yNumerator*other.yDenominator==other.yNumerator*yDenominator;
                }
            };
            std::unordered_map<std::string,PixelScale> physicalScales;
            if (replacePixels) {
                std::uint64_t inputBytes=0;
                const auto resizePixels=[&](const RasterLayer &source,RasterLayer &output) -> DocumentHistoryCode {
                    const auto nx=std::uint64_t(source.width)*plan.target.extent.width;
                    const auto ny=std::uint64_t(source.height)*plan.target.extent.height;
                    const auto w=std::max<std::uint64_t>(1,(nx+plan.source.extent.width/2)/plan.source.extent.width);
                    const auto h=std::max<std::uint64_t>(1,(ny+plan.source.extent.height/2)/plan.source.extent.height);
                    if (!w || !h || w>std::numeric_limits<int>::max() || h>std::numeric_limits<int>::max() ||
                        w*h>64ULL*1024*1024 || source.pixels.size()>limits.maximumBitmapInputBytes/4 ||
                        w*h>limits.maximumBitmapInputBytes/4) return DocumentHistoryCode::LimitExceeded;
                    const auto need=(std::uint64_t(source.pixels.size())+w*h)*4;
                    if (need>limits.maximumBitmapInputBytes-inputBytes) return DocumentHistoryCode::LimitExceeded;
                    inputBytes+=need;
                    output=makeRasterLayer(int(w),int(h),0);
                    for (std::uint64_t y=0;y<h;++y) for (std::uint64_t x=0;x<w;++x) {
                        const auto xx=(2*x+1)*std::uint64_t(source.width)/(2*w);
                        const auto yy=(2*y+1)*std::uint64_t(source.height)/(2*h);
                        output.pixels[std::size_t(y*w+x)]=source.pixels[std::size_t(yy)*source.width+xx];
                    }
                    return DocumentHistoryCode::None;
                };
                const auto resizeChunks=[&](const ChunkedRasterAsset &source,ChunkedRasterAsset &output) -> DocumentHistoryCode {
                    // The chunk grid remains fixed; sample in signed asset coordinates
                    // so sparse negative and off-canvas content is retained. Bounds are
                    // conservatively expanded and each sample is checked with integer
                    // arithmetic, avoiding floating-point boundary decisions.
                    const std::int64_t size=d->infiniteCanvas.chunkSize;
                    const auto bytesPerChunk=std::uint64_t(size)*size*sizeof(std::uint32_t);
                    if (source.chunks.size()>(limits.maximumBitmapInputBytes-inputBytes)/bytesPerChunk)
                        return DocumentHistoryCode::LimitExceeded;
                    inputBytes+=source.chunks.size()*bytesPerChunk;
                    std::map<std::pair<std::int32_t,std::int32_t>,RasterLayer> chunks;
                    std::uint64_t work=0;
                    const auto floorDivide=[](std::int64_t numerator,std::int64_t denominator) {
                        auto quotient=numerator/denominator;
                        if (numerator%denominator<0) --quotient;
                        return quotient;
                    };
                    const auto sourceAt=[&](std::int64_t coordinate,std::int32_t before,std::int32_t after) {
                        return floorDivide((2*coordinate+1)*std::int64_t(before),2*std::int64_t(after));
                    };
                    const auto range=[](std::int64_t start,std::int64_t length,std::int32_t before,
                                        std::int32_t after,std::int64_t &first,std::int64_t &last) {
                        const long double factor=static_cast<long double>(after)/before;
                        const long double lo=std::floor(start*factor),hi=std::ceil((start+length)*factor);
                        constexpr auto min=std::numeric_limits<std::int32_t>::min();
                        constexpr auto end=std::int64_t(std::numeric_limits<std::int32_t>::max())+1;
                        if (lo<min || hi>end) return false;
                        first=std::max<std::int64_t>(min,static_cast<std::int64_t>(lo)-1);
                        last=std::min<std::int64_t>(end,static_cast<std::int64_t>(hi)+1);
                        return true;
                    };
                    for (const auto &chunk:source.chunks) {
                        const auto x0=std::int64_t(chunk.column)*size,y0=std::int64_t(chunk.row)*size;
                        std::int64_t left,right,top,bottom;
                        if (!range(x0,size,plan.source.extent.width,plan.target.extent.width,left,right) ||
                            !range(y0,size,plan.source.extent.height,plan.target.extent.height,top,bottom))
                            return DocumentHistoryCode::LimitExceeded;
                        const auto width=std::uint64_t(right-left),height=std::uint64_t(bottom-top);
                        constexpr std::uint64_t maximumWork=64ULL*1024*1024;
                        if (width && (height>maximumWork/width || width*height>maximumWork-work))
                            return DocumentHistoryCode::LimitExceeded;
                        work+=width*height;
                        for (auto y=top;y<bottom;++y) {
                            const auto sampleY=sourceAt(y,plan.source.extent.height,plan.target.extent.height);
                            if (sampleY<y0 || sampleY>=y0+size) continue;
                            for (auto x=left;x<right;++x) {
                                const auto sampleX=sourceAt(x,plan.source.extent.width,plan.target.extent.width);
                                if (sampleX<x0 || sampleX>=x0+size) continue;
                                const auto pixel=chunk.pixels.pixels[std::size_t(sampleY-y0)*size+std::size_t(sampleX-x0)];
                                if (!pixel) continue;
                                const auto column=floorDivide(x,size),row=floorDivide(y,size);
                                const auto key=std::pair{static_cast<std::int32_t>(row),static_cast<std::int32_t>(column)};
                                auto found=chunks.find(key);
                                if (found==chunks.end()) {
                                    if (bytesPerChunk>limits.maximumBitmapInputBytes-inputBytes)
                                        return DocumentHistoryCode::LimitExceeded;
                                    inputBytes+=bytesPerChunk;
                                    found=chunks.emplace(key,makeRasterLayer(int(size),int(size),0)).first;
                                }
                                found->second.pixels[std::size_t(y-row*size)*size+std::size_t(x-column*size)]=pixel;
                            }
                        }
                    }
                    output.id=source.id; output.chunks.reserve(chunks.size());
                    for (auto &[position,pixels]:chunks)
                        output.chunks.push_back({position.second,position.first,std::move(pixels)});
                    return DocumentHistoryCode::None;
                };
                const auto resizeDense=[&](const Asset &source,Asset &output,CanvasExtent &before,CanvasExtent &after) {
                    return std::visit([&](const auto &value) -> DocumentHistoryCode {
                        using T=std::decay_t<decltype(value)>;
                        if constexpr (detail::isDenseControlAsset<T>) {
                            const auto &samples=detail::denseControlSamples(value);
                            using Sample=typename std::decay_t<decltype(samples)>::value_type;
                            before=value.viewport;
                            const auto nx=std::uint64_t(before.width)*plan.target.extent.width;
                            const auto ny=std::uint64_t(before.height)*plan.target.extent.height;
                            const auto width=std::max<std::uint64_t>(1,(nx+plan.source.extent.width/2)/plan.source.extent.width);
                            const auto height=std::max<std::uint64_t>(1,(ny+plan.source.extent.height/2)/plan.source.extent.height);
                            if (width>std::numeric_limits<std::int32_t>::max() || height>std::numeric_limits<std::int32_t>::max() ||
                                width*height>64ULL*1024*1024 || samples.size()>limits.maximumBitmapInputBytes/sizeof(Sample) ||
                                width*height>limits.maximumBitmapInputBytes/sizeof(Sample)) return DocumentHistoryCode::LimitExceeded;
                            const auto needed=(std::uint64_t(samples.size())+width*height)*sizeof(Sample);
                            if (needed>limits.maximumBitmapInputBytes-inputBytes) return DocumentHistoryCode::LimitExceeded;
                            inputBytes+=needed;
                            T next; next.id=value.id; next.viewport={std::int32_t(width),std::int32_t(height)};
                            auto &newSamples=detail::denseControlSamples(next);
                            newSamples.reserve(std::size_t(width*height));
                            for (std::uint64_t y=0;y<height;++y) for (std::uint64_t x=0;x<width;++x) {
                                const auto xx=(2*x+1)*std::uint64_t(before.width)/(2*width);
                                const auto yy=(2*y+1)*std::uint64_t(before.height)/(2*height);
                                newSamples.push_back(samples[std::size_t(yy)*before.width+xx]);
                            }
                            after=next.viewport; output=std::move(next); return DocumentHistoryCode::None;
                        } else return DocumentHistoryCode::InvalidArgument;
                    },source);
                };
                for (const auto &asset:d->assets) {
                    const auto *raster=std::get_if<RasterAsset>(&asset);
                    const auto *video=std::get_if<VideoAsset>(&asset);
                    const auto *chunked=std::get_if<ChunkedRasterAsset>(&asset);
                    const bool dense=denseControlAsset(asset);
                    if (!raster && !video && !chunked && !dense) {
                        const auto *fill=std::get_if<FillLayerContent>(&asset);
                        // These sources are editable geometry or nonspatial data;
                        // their layer transform, rather than their data, changes.
                        if (!std::holds_alternative<VectorAsset>(asset) && !std::holds_alternative<PoseAsset>(asset) &&
                            !std::holds_alternative<MlsdAsset>(asset) && !std::holds_alternative<IpAdapterAsset>(asset) &&
                            !std::holds_alternative<AdjustmentLayerContent>(asset) &&
                            !(fill && fill->kind!=FillLayerKind::Pattern)) return {DocumentHistoryCode::EditRejected};
                        continue;
                    }
                    const auto protection=d->relations.locks.assetLocks(*d,assetId(asset));
                    if (hasLayerLock(protection,LayerLock::Pixels) || hasLayerLock(protection,LayerLock::Transparency))
                        return {DocumentHistoryCode::EditRejected};
                    Asset resized; CanvasExtent oldDense{},newDense{};
                    if (raster) {
                        RasterLayer output;
                        if (const auto code=resizePixels(raster->pixels,output); code!=DocumentHistoryCode::None) return {code};
                        resized=RasterAsset{raster->id,std::move(output)};
                    } else if (video) {
                        if (video->frames.empty()) return {DocumentHistoryCode::InvalidArgument};
                        VideoAsset output; output.id=video->id; output.frameRate=video->frameRate;
                        output.frames.reserve(video->frames.size());
                        for (const auto &frame:video->frames) {
                            RasterLayer next;
                            if (const auto code=resizePixels(frame,next); code!=DocumentHistoryCode::None) return {code};
                            output.frames.push_back(std::move(next));
                        }
                        resized=std::move(output);
                    } else if (chunked) {
                        ChunkedRasterAsset output;
                        if (const auto code=resizeChunks(*chunked,output); code!=DocumentHistoryCode::None) return {code};
                        resized=std::move(output);
                    } else {
                        if (const auto code=resizeDense(asset,resized,oldDense,newDense); code!=DocumentHistoryCode::None) return {code};
                    }
                    if (chunked) {
                        physicalScales.emplace(assetId(asset),PixelScale{std::uint64_t(plan.target.extent.width),
                            std::uint64_t(plan.source.extent.width),std::uint64_t(plan.target.extent.height),
                            std::uint64_t(plan.source.extent.height)});
                    } else if (dense) {
                        physicalScales.emplace(assetId(asset),PixelScale{std::uint64_t(newDense.width),std::uint64_t(oldDense.width),
                            std::uint64_t(newDense.height),std::uint64_t(oldDense.height)});
                    } else {
                        const auto &oldPixels=raster ? raster->pixels : video->frames.front();
                        const auto &newPixels=raster ? std::get<RasterAsset>(resized).pixels : std::get<VideoAsset>(resized).frames.front();
                        physicalScales.emplace(assetId(asset),PixelScale{std::uint64_t(newPixels.width),std::uint64_t(oldPixels.width),
                            std::uint64_t(newPixels.height),std::uint64_t(oldPixels.height)});
                    }
                    CanvasGeometryChange::AssetPair pair;
                    if (detail::AssetHistorySnapshot::capture(asset,limits.maximumBytes,pair.before)!=detail::AssetHistorySnapshot::Code::None ||
                        detail::AssetHistorySnapshot::capture(resized,limits.maximumBytes,pair.after)!=detail::AssetHistorySnapshot::Code::None ||
                        !addSize(assetBytes,pair.before.bytes()) || !addSize(assetBytes,pair.after.bytes()))
                        return {DocumentHistoryCode::BudgetExceeded};
                    change->assets.push_back(std::move(pair));
                }
            }
            for (auto &board:change->afterArtboards) {
                ArtboardGeometry g;
                if (board.geometry) g=*board.geometry;
                else { g.extent=board.region.extent; g.transform.translationX=board.region.origin.x;
                       g.transform.translationY=board.region.origin.y; }
                g.transform=canvas_affine_detail::compose(t,g.transform);
                const auto region=canvas_affine_detail::bounds({{},g.extent},g.transform);
                if (!region) return {DocumentHistoryCode::InvalidArgument};
                board.region=*region; board.geometry=g;
            }
            for (auto &layer:change->afterLayers) {
                auto &p=layerProperties(layer);
                if (!p.artboardId) p.transform=canvas_affine_detail::compose(t,p.transform);
                bool rasterContent=false;
                double contentX=1,contentY=1;
                if (replacePixels) {
                    std::optional<bool> isRaster;
                    std::optional<PixelScale> actualScale;
                    const auto classify=[&](const Asset *asset) {
                        if (!asset) return false;
                        const bool raster=physicalScales.contains(assetId(*asset));
                        if (isRaster && *isRaster!=raster) return false;
                        isRaster=raster;
                        if (raster) {
                            const auto found=physicalScales.find(assetId(*asset));
                            if (found==physicalScales.end() || (actualScale && !actualScale->same(found->second))) return false;
                            actualScale=found->second;
                        }
                        return true;
                    };
                    if (const auto *source=staticLayerSource(layer)) {
                        if (!classify(findAsset(*d,source->assetId))) return {DocumentHistoryCode::InvalidArgument};
                    } else if (const auto *source=keyframedLayerSource(layer)) {
                        for (const auto frame:source->frameIndices)
                            if (!classify(resolveAssetAt(*d,layer,frame))) return {DocumentHistoryCode::EditRejected};
                    } else return {DocumentHistoryCode::EditRejected};
                    rasterContent=isRaster.value_or(false);
                    if (actualScale) { contentX=actualScale->x(); contentY=actualScale->y(); }
                    AffineTransform inverseScale; inverseScale.m11=1/contentX; inverseScale.m22=1/contentY;
                    if (rasterContent && p.motion.empty()) {
                        p.transform=canvas_affine_detail::compose(p.transform,inverseScale);
                    } else if (rasterContent) {
                        // M'(frame) = M(frame) * inverseScale for every
                        // interpolation point, not just at keyframe boundaries.
                        for (auto &key:p.motion) {
                            const auto anchor=key.value.anchor;
                            key.value.anchor={anchor.x*contentX,anchor.y*contentY};
                            key.value.position.x+=anchor.x-key.value.anchor.x;
                            key.value.position.y+=anchor.y-key.value.anchor.y;
                            key.value.scale.x/=contentX; key.value.scale.y/=contentY;
                        }
                    }
                }
                const auto map=[&](LayerMask &mask) {
                    const auto *source=findAsset(*d,mask.assetId);
                    if (!source) return false;
                    const bool rasterMask=replacePixels && std::holds_alternative<RasterAsset>(*source);
                    if (mask.linkedToLayer && replacePixels && rasterContent) {
                        AffineTransform contentScale; contentScale.m11=contentX; contentScale.m22=contentY;
                        mask.transform=canvas_affine_detail::compose(contentScale,mask.transform);
                    }
                    if (!mask.linkedToLayer && !p.artboardId)
                        mask.transform=canvas_affine_detail::compose(t,mask.transform);
                    if (rasterMask) {
                        const auto found=physicalScales.find(mask.assetId);
                        if (found==physicalScales.end()) return false;
                        const double maskX=found->second.x(),maskY=found->second.y();
                        AffineTransform inverseMask; inverseMask.m11=1/maskX; inverseMask.m22=1/maskY;
                        mask.transform=canvas_affine_detail::compose(mask.transform,inverseMask);
                        if (mask.featherRadius!=0) {
                            if (found->second.xNumerator*found->second.yDenominator!=
                                found->second.yNumerator*found->second.xDenominator || mask.featherRadius*maskX>128) return false;
                            mask.featherRadius*=maskX;
                        }
                    }
                    return true;
                };
                if (p.appearance.rasterMask && !map(*p.appearance.rasterMask)) return {DocumentHistoryCode::EditRejected};
                if (p.appearance.vectorMask && !map(*p.appearance.vectorMask)) return {DocumentHistoryCode::EditRejected};
                if (p.appearance.filterMask && !map(*p.appearance.filterMask)) return {DocumentHistoryCode::EditRejected};
                for (auto &entry:p.appearance.maskStack) if (!map(entry.mask)) return {DocumentHistoryCode::EditRejected};
            }
            constexpr std::uint64_t maxSelectionPixels=64ULL*1024*1024;
            const auto det=t.m11*t.m22-t.m12*t.m21;
            for (const auto &channel:d->relations.selections.channels()) {
                const auto region=canvas_affine_detail::bounds(channel.mask->domain(),t);
                if (!region || std::uint64_t(region->extent.width)*region->extent.height>maxSelectionPixels)
                    return {DocumentHistoryCode::LimitExceeded};
                RasterMask raster{region->extent,{}};
                raster.alpha.resize(std::size_t(region->extent.width)*region->extent.height);
                for (int y=0;y<region->extent.height;++y) for (int x=0;x<region->extent.width;++x) {
                    const double dx=region->origin.x+x+0.5-t.translationX,dy=region->origin.y+y+0.5-t.translationY;
                    const double sx=std::floor((dx*t.m22-dy*t.m21)/det);
                    const double sy=std::floor((-dx*t.m12+dy*t.m11)/det);
                    if (sx>=std::numeric_limits<std::int32_t>::min() && sx<=std::numeric_limits<std::int32_t>::max() &&
                        sy>=std::numeric_limits<std::int32_t>::min() && sy<=std::numeric_limits<std::int32_t>::max())
                        raster.alpha[std::size_t(y)*region->extent.width+x]=channel.mask->coverage(std::int32_t(sx),std::int32_t(sy));
                }
                auto transformed=TiledMask::fromRaster(*region,region->origin,raster);
                if (!transformed.ok()) return {DocumentHistoryCode::LimitExceeded};
                NamedSelectionChannel next{channel.id,channel.name,std::make_shared<const TiledMask>(std::move(transformed.mask))};
                if (!change->afterRelations.selections.replace(channel.id,std::move(next))) return {DocumentHistoryCode::InvalidArgument};
            }
            std::size_t bytes=sizeof(Entry)+sizeof(CanvasGeometryChange);
            if (!addSize(bytes,label.size()) || !addSize(bytes,assetBytes) || !addElements(bytes,d->layers.size(),2*sizeof(Layer)) ||
                !artboardBytes(bytes,change->beforeArtboards) || !artboardBytes(bytes,change->afterArtboards) ||
                !relationsSize(bytes,change->beforeRelations) || !relationsSize(bytes,change->afterRelations))
                return {DocumentHistoryCode::BudgetExceeded};
            const std::vector<KeyframePlacement> keys;
            for (const auto *layers:std::array<const std::vector<Layer> *,2>{&change->beforeLayers,&change->afterLayers})
                for (const auto &layer:*layers) if (!layerStructureSize(bytes,layer,keys)) return {DocumentHistoryCode::BudgetExceeded};
            if (!canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<std::unique_ptr<CanvasGeometryChange>>(std::move(change));
            const auto code=applyCanvasGeometryChange(*std::get<std::unique_ptr<CanvasGeometryChange>>(entry->change),true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult translate(std::span<const LayerOffset> offsets,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            if (offsets.empty()) return {DocumentHistoryCode::InvalidArgument};
            const auto *d=document(); auto layers=d->layers; std::unordered_set<std::string_view> seen;
            for (const auto &offset:offsets) {
                if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !seen.insert(offset.id).second)
                    return {DocumentHistoryCode::InvalidArgument};
                const auto index=layerIndex(*d,offset.id);
                if (!index) return {DocumentHistoryCode::InvalidArgument};
                auto &transform=layerProperties(layers[*index]).transform;
                transform.translationX+=offset.x; transform.translationY+=offset.y;
                if (!validTransform(transform)) return {DocumentHistoryCode::InvalidArgument};
            }
            return commitLayerBatch(std::move(layers),label);
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    // Replace one complete vector value while retaining the old value and
    // authorship until validation and any durable publication succeed.
    DocumentHistoryCode applyVectorAsset(const VectorAssetChange &change,bool forward) {
        const auto &expected=forward ? change.before : change.after;
        const auto &next=forward ? change.after : change.before;
        const std::string id(expected.id()); const auto *d=document();
        const auto *asset=d ? findAsset(*d,id) : nullptr;
        if (!asset || !expected.matches(*asset)) return DocumentHistoryCode::Conflict;
        const auto protection=d->relations.locks.assetLocks(*d,id);
        if (hasLayerLock(protection,LayerLock::Pixels) || hasLayerLock(protection,LayerLock::Transparency))
            return DocumentHistoryCode::EditRejected;
        auto restored=next.restore(); auto *replacement=std::get_if<VectorAsset>(&restored);
        if (!replacement) return DocumentHistoryCode::InvalidArgument;
        DocumentHistoryCode failure=DocumentHistoryCode::None;
        const auto mutate=[&](Document &target) {
            auto *vector=findVectorAsset(target,id);
            if (!vector) { failure=DocumentHistoryCode::Conflict; return false; }
            struct Guard {
                Document &document; VectorAsset &vector; VectorAsset saved;
                DocumentAuthorship author; FormatVersion version; bool committed=false;
                Guard(Document &d,VectorAsset &v) : document(d),vector(v),saved(v),author(d.authorship),version(d.formatVersion) {
                    using std::swap; swap(vector,saved); swap(document.authorship,author);
                }
                ~Guard() noexcept {
                    if (!committed) { using std::swap; swap(vector,saved); swap(document.authorship,author); document.formatVersion=version; }
                }
            } guard(target,*vector);
            *vector=std::move(*replacement); target.formatVersion.minor=CurrentFormatMinor;
            if (!validate(target).ok()) { failure=DocumentHistoryCode::InvalidArgument; return false; }
            recordDocumentChange(target); guard.committed=true; return true;
        };
        if (file) {
            const auto result=file->edit(mutate);
            if (!result.ok()) return failure!=DocumentHistoryCode::None ? failure : fileFailure(file);
            return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        return mutate(*memory) ? DocumentHistoryCode::None : failure;
    }
    DocumentHistoryResult vectorPath(std::string_view id,std::size_t index,VectorPath path,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const auto *d=document(); const std::string key(id); const auto *original=findVectorAsset(*d,key);
            if (!original || index>=original->paths.size()) return {DocumentHistoryCode::InvalidArgument};
            const auto locks=d->relations.locks.assetLocks(*d,key);
            if (hasLayerLock(locks,LayerLock::Pixels) || hasLayerLock(locks,LayerLock::Transparency))
                return {DocumentHistoryCode::EditRejected};
            auto edited=*original; edited.paths[index]=std::move(path);
            const Asset originalAsset=*original,editedAsset=std::move(edited);
            std::size_t bytes=sizeof(Entry)+sizeof(VectorAssetChange)+label.size();
            detail::AssetHistorySnapshot before,after;
            auto code=detail::AssetHistorySnapshot::capture(originalAsset,rasterAllowance(bytes),before);
            if (code==detail::AssetHistorySnapshot::Code::BudgetExceeded) return {DocumentHistoryCode::BudgetExceeded};
            if (code!=detail::AssetHistorySnapshot::Code::None) return {DocumentHistoryCode::InvalidArgument};
            if (before.matches(editedAsset)) {
                if (file) { const auto result=file->edit([](Document &) { return true; });
                    if (!result.ok()) return {fileFailure(file)}; }
                return {};
            }
            if (!addSize(bytes,before.bytes())) return {DocumentHistoryCode::BudgetExceeded};
            code=detail::AssetHistorySnapshot::capture(editedAsset,rasterAllowance(bytes),after);
            if (code==detail::AssetHistorySnapshot::Code::BudgetExceeded) return {DocumentHistoryCode::BudgetExceeded};
            if (code!=detail::AssetHistorySnapshot::Code::None) return {DocumentHistoryCode::InvalidArgument};
            if (!addSize(bytes,after.bytes()) || !canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<std::unique_ptr<VectorAssetChange>>(std::make_unique<VectorAssetChange>(
                VectorAssetChange{std::move(before),std::move(after)}));
            const auto result=applyVectorAsset(*std::get<std::unique_ptr<VectorAssetChange>>(entry->change),true);
            if (result!=DocumentHistoryCode::None) return {result};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }

    // Match only the frame/key values relevant to structural history, without
    // assuming equality operators on the public aggregate types.
    static bool sameFrames(const std::vector<Frame> &a,const std::vector<Frame> &b) noexcept {
        if (a.size()!=b.size()) return false;
        for (std::size_t i=0;i<a.size();++i) {
            if (a[i].index!=b[i].index || a[i].keyframes.size()!=b[i].keyframes.size()) return false;
            for (std::size_t k=0;k<a[i].keyframes.size();++k)
                if (a[i].keyframes[k].layerId!=b[i].keyframes[k].layerId ||
                    a[i].keyframes[k].assetId!=b[i].keyframes[k].assetId) return false;
        }
        return true;
    }
    static bool frameBytes(std::size_t &bytes,const std::vector<Frame> &frames) noexcept {
        if (!addElements(bytes,frames.size(),sizeof(Frame))) return false;
        for (const auto &frame:frames) {
            if (!addElements(bytes,frame.keyframes.size(),sizeof(Keyframe))) return false;
            for (const auto &key:frame.keyframes)
                if (!addSize(bytes,key.layerId.size()) || !addSize(bytes,key.assetId.size())) return false;
        }
        return true;
    }
    static bool artboardBytes(std::size_t &bytes,const std::vector<Artboard> &boards) noexcept {
        if (!addElements(bytes,boards.size(),sizeof(Artboard))) return false;
        for (const auto &board:boards) if (!addSize(bytes,board.id.size()) || !addSize(bytes,board.name.size())) return false;
        return true;
    }

    // Replay the exact changed assets and structural values using a rollback
    // guard. Each working-file replay is one durable scoped transaction.
    DocumentHistoryCode applyLayerTransactionChange(const LayerTransactionChange &change,bool forward) {
        const auto *d=document(); if (!d) return DocumentHistoryCode::NotBound;
        const auto &expectedLayers=forward ? change.beforeLayers : change.afterLayers;
        const auto &nextLayers=forward ? change.afterLayers : change.beforeLayers;
        const auto &expectedFrames=forward ? change.beforeFrames : change.afterFrames;
        const auto &nextFrames=forward ? change.afterFrames : change.beforeFrames;
        const auto &expectedBoards=forward ? change.beforeArtboards : change.afterArtboards;
        const auto &nextBoards=forward ? change.afterArtboards : change.beforeArtboards;
        const auto &expectedRelations=forward ? change.beforeRelations : change.afterRelations;
        const auto &nextRelations=forward ? change.afterRelations : change.beforeRelations;
        const auto &expectedAssets=forward ? change.beforeAssets : change.afterAssets;
        const auto &remove=forward ? change.removed : change.added;
        const auto &insert=forward ? change.added : change.removed;
        if (d->layers.size()!=expectedLayers.size() || !sameFrames(d->frames,expectedFrames) ||
            d->artboards!=expectedBoards || d->relations!=expectedRelations || d->assets.size()!=expectedAssets.size())
            return DocumentHistoryCode::Conflict;
        for (std::size_t i=0;i<d->layers.size();++i)
            if (!sameLayer(d->layers[i],expectedLayers[i])) return DocumentHistoryCode::Conflict;
        for (std::size_t i=0;i<d->assets.size();++i)
            if (assetId(d->assets[i])!=expectedAssets[i]) return DocumentHistoryCode::Conflict;
        for (const auto &guard:change.guards) {
            const auto *asset=findAsset(*d,std::string(guard.id()));
            if (!asset || !guard.matches(*asset)) return DocumentHistoryCode::Conflict;
        }
        for (const auto &item:remove) {
            if (item.index>=d->assets.size() || assetId(d->assets[item.index])!=item.id ||
                detail::encodeHistoryAsset(d->assets[item.index])!=item.bytes) return DocumentHistoryCode::Conflict;
        }
        std::vector<Asset> restored; restored.reserve(insert.size());
        for (const auto &item:insert) {
            auto decoded=detail::decodeHistoryAsset(item.bytes);
            if (decoded.error.code!=IiscErrorCode::None || assetId(decoded.asset)!=item.id)
                return DocumentHistoryCode::EditRejected;
            restored.push_back(std::move(decoded.asset));
        }
        DocumentHistoryCode failure=DocumentHistoryCode::None;
        const auto mutate=[&](Document &target) {
            detail::MultiAssetLayerGuard guard(target,remove.size(),insert.size());
            for (auto i=remove.rbegin();i!=remove.rend();++i) guard.remove(i->index);
            for (std::size_t i=0;i<insert.size();++i) guard.insert(insert[i].index,std::move(restored[i]));
            target.layers=nextLayers; target.frames=nextFrames; target.artboards=nextBoards;
            target.relations=nextRelations; target.formatVersion.minor=CurrentFormatMinor;
            if (!validate(target).ok()) { failure=DocumentHistoryCode::InvalidArgument; return false; }
            recordDocumentChange(target); guard.publish(); return true;
        };
        if (file) {
            const auto result=file->edit(mutate);
            if (!result.ok()) return failure!=DocumentHistoryCode::None ? failure : fileFailure(file);
            return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        return mutate(*memory) ? DocumentHistoryCode::None : failure;
    }

    DocumentHistoryResult layerTransaction(LayerTransactionPlan plan,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const auto *d=document();
            if (plan.sourceRevision!=d->authorship.revision() || plan.resultLayerId.empty() ||
                findLayer(*d,plan.resultLayerId) || plan.addedAssets.empty()) return {DocumentHistoryCode::Conflict};
            std::unordered_set<std::string_view> removed;
            for (const auto &id:plan.removedLayers) {
                if (!removed.insert(id).second || !findLayer(*d,id)) return {DocumentHistoryCode::InvalidArgument};
                if (hasLayerLock(d->relations.locks.effective(*d,id),LayerLock::Structure))
                    return {DocumentHistoryCode::EditRejected};
            }
            if (plan.flatten && removed.size()!=d->layers.size()) return {DocumentHistoryCode::InvalidArgument};
            if (plan.flatten) for (const auto &lock:d->relations.locks.groups())
                if (hasLayerLock(lock.flags,LayerLock::Structure)) return {DocumentHistoryCode::EditRejected};
            if (plan.removedAssets!=detail::removableLayerAssets(*d,plan.removedLayers)) return {DocumentHistoryCode::Conflict};
            if (plan.layers.size()!=d->layers.size()-removed.size()+1 ||
                (plan.flatten ? !plan.artboards.empty() : plan.artboards!=d->artboards)) return {DocumentHistoryCode::InvalidArgument};
            std::vector<const Layer *> untouchedBefore,untouchedAfter; std::size_t resultCount=0;
            for (const auto &layer:d->layers) if (!removed.contains(layerProperties(layer).id)) untouchedBefore.push_back(&layer);
            for (const auto &layer:plan.layers) {
                if (layerProperties(layer).id==plan.resultLayerId) ++resultCount;
                else untouchedAfter.push_back(&layer);
            }
            if (resultCount!=1 || untouchedBefore.size()!=untouchedAfter.size()) return {DocumentHistoryCode::InvalidArgument};
            for (std::size_t i=0;i<untouchedBefore.size();++i)
                if (!sameLayer(*untouchedBefore[i],*untouchedAfter[i])) return {DocumentHistoryCode::InvalidArgument};
            auto oldFrames=d->frames,afterFrames=plan.frames;
            const auto eraseKeys=[](std::vector<Frame> &frames,const auto &predicate) {
                for (auto &frame:frames) frame.keyframes.erase(std::remove_if(frame.keyframes.begin(),frame.keyframes.end(),predicate),frame.keyframes.end());
                frames.erase(std::remove_if(frames.begin(),frames.end(),[](const Frame &frame) { return frame.keyframes.empty(); }),frames.end());
            };
            eraseKeys(oldFrames,[&](const Keyframe &key) { return removed.contains(key.layerId); });
            eraseKeys(afterFrames,[&](const Keyframe &key) { return key.layerId==plan.resultLayerId; });
            if (!sameFrames(oldFrames,afterFrames)) return {DocumentHistoryCode::InvalidArgument};
            auto relations=d->relations;
            std::string group;
            if (!plan.flatten && !plan.removedLayers.empty())
                group=relations.groups.groupOf(plan.removedLayers.front());
            for (const auto &id:plan.removedLayers) relations.removeLayer(id);
            if (plan.flatten) { SelectionLayerRelations stripped; stripped.selections=relations.selections; relations=std::move(stripped); }
            else if (!group.empty() && !relations.groups.assign(plan.resultLayerId,group)) return {DocumentHistoryCode::InvalidArgument};
            if (relations!=plan.relations) return {DocumentHistoryCode::InvalidArgument};
            std::unordered_set<std::string_view> newIds;
            std::size_t inputBytes=0;
            for (const auto &asset:plan.addedAssets) {
                const auto *raster=std::get_if<RasterAsset>(&asset);
                if (!raster || raster->id.empty() || findAsset(*d,raster->id) || findAudioAsset(*d,raster->id) ||
                    !newIds.insert(raster->id).second || raster->pixels.width!=d->extent.width || raster->pixels.height!=d->extent.height ||
                    raster->pixels.pixels.size()!=std::uint64_t(d->extent.width)*d->extent.height || !bitmapInputSize(asset,inputBytes))
                    return {DocumentHistoryCode::InvalidArgument};
            }
            const auto resultPosition=std::find_if(plan.layers.begin(),plan.layers.end(),[&](const Layer &layer) {
                return layerProperties(layer).id==plan.resultLayerId;
            });
            const auto *resultLayer=&*resultPosition;
            std::unordered_set<std::string_view> referenced;
            if (const auto *source=staticLayerSource(*resultLayer)) referenced.insert(source->assetId);
            else for (const auto &frame:plan.frames) for (const auto &key:frame.keyframes)
                if (key.layerId==plan.resultLayerId) referenced.insert(key.assetId);
            if (referenced!=newIds) return {DocumentHistoryCode::InvalidArgument};
            for (const auto &id:plan.removedAssets) {
                const auto *asset=findAsset(*d,id); if (!asset || !bitmapInputSize(*asset,inputBytes)) return {DocumentHistoryCode::InvalidArgument};
            }
            if (inputBytes>limits.maximumBitmapInputBytes) return {DocumentHistoryCode::LimitExceeded};
            auto change=std::make_unique<LayerTransactionChange>();
            change->beforeLayers=d->layers; change->afterLayers=std::move(plan.layers);
            change->beforeFrames=d->frames; change->afterFrames=std::move(plan.frames);
            change->beforeArtboards=d->artboards; change->afterArtboards=std::move(plan.artboards);
            change->beforeRelations=d->relations; change->afterRelations=std::move(plan.relations);
            std::size_t bytes=sizeof(Entry)+sizeof(LayerTransactionChange);
            if (!addSize(bytes,label.size()) || !addElements(bytes,d->layers.size(),sizeof(Layer)) ||
                !addElements(bytes,change->afterLayers.size(),sizeof(Layer)) ||
                !frameBytes(bytes,change->beforeFrames) || !frameBytes(bytes,change->afterFrames) ||
                !artboardBytes(bytes,change->beforeArtboards) || !artboardBytes(bytes,change->afterArtboards) ||
                !relationsSize(bytes,change->beforeRelations) || !relationsSize(bytes,change->afterRelations))
                return {DocumentHistoryCode::BudgetExceeded};
            const std::vector<KeyframePlacement> keys;
            for (const auto *layers:std::array<const std::vector<Layer> *,2>{&change->beforeLayers,&change->afterLayers})
                for (const auto &layer:*layers) if (!layerStructureSize(bytes,layer,keys)) return {DocumentHistoryCode::BudgetExceeded};
            const std::unordered_set<std::string_view> removeAssets(plan.removedAssets.begin(),plan.removedAssets.end());
            for (std::size_t i=0;i<d->assets.size();++i) {
                const auto &asset=d->assets[i]; const auto &id=assetId(asset); change->beforeAssets.push_back(id);
                if (removeAssets.contains(id)) {
                    auto encoded=detail::encodeHistoryAsset(asset);
                    if (!addSize(bytes,sizeof(LayerTransactionAsset)) || !addSize(bytes,id.size()) || !addSize(bytes,encoded.size()))
                        return {DocumentHistoryCode::BudgetExceeded};
                    change->removed.push_back({id,i,std::move(encoded)});
                } else change->afterAssets.push_back(id);
            }
            for (const auto &asset:plan.addedAssets) {
                const auto &id=assetId(asset); auto encoded=detail::encodeHistoryAsset(asset);
                if (!addSize(bytes,sizeof(LayerTransactionAsset)) || !addSize(bytes,id.size()) || !addSize(bytes,encoded.size()))
                    return {DocumentHistoryCode::BudgetExceeded};
                change->added.push_back({id,change->afterAssets.size(),std::move(encoded)}); change->afterAssets.push_back(id);
            }
            for (const auto *list:std::array<const std::vector<std::string> *,2>{&change->beforeAssets,&change->afterAssets}) {
                if (!addElements(bytes,list->size(),sizeof(std::string))) return {DocumentHistoryCode::BudgetExceeded};
                for (const auto &id:*list) if (!addSize(bytes,id.size())) return {DocumentHistoryCode::BudgetExceeded};
            }
            if (!canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<std::unique_ptr<LayerTransactionChange>>(std::move(change));
            const auto code=applyLayerTransactionChange(*std::get<std::unique_ptr<LayerTransactionChange>>(entry->change),true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }

    // One reversible structural change owns the new raster and the exact
    // before/after exposure table. The source asset is never modified, so
    // other layers and held frames cannot observe the selected frame's edit.
    DocumentHistoryResult layerFrameRaster(std::string_view layerId,FrameIndex frame,const RasterLayer &expectedSource,RasterAsset content,
                                           std::uint64_t sourceRevision,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const auto *d=document();
            if (!d || sourceRevision!=d->authorship.revision()) return {DocumentHistoryCode::Conflict};
            const auto position=layerIndex(*d,std::string(layerId));
            if (!position || frame>=d->timeline.frameCount) return {DocumentHistoryCode::InvalidArgument};
            const auto &sourceLayer=d->layers[*position];
            if (!std::holds_alternative<StaticBitmapLayer>(sourceLayer) &&
                !std::holds_alternative<DynamicBitmapLayer>(sourceLayer)) return {DocumentHistoryCode::InvalidArgument};
            const auto *resolved=resolveAssetAt(*d,sourceLayer,frame);
            const auto *original=resolved ? std::get_if<RasterAsset>(resolved) : nullptr;
            if (!original || content.id.empty() || findAsset(*d,content.id) || findAudioAsset(*d,content.id) ||
                content.pixels.width!=original->pixels.width || content.pixels.height!=original->pixels.height ||
                content.pixels.width<=0 || content.pixels.height<=0 ||
                std::uint64_t(content.pixels.width)*content.pixels.height!=content.pixels.pixels.size())
                return {DocumentHistoryCode::InvalidArgument};
            if (expectedSource.width!=original->pixels.width || expectedSource.height!=original->pixels.height ||
                expectedSource.pixels!=original->pixels.pixels) return {DocumentHistoryCode::Conflict};
            if (content.pixels.pixels==original->pixels.pixels) return {};
            const auto protection=d->relations.locks.effective(*d,layerId);
            if (hasLayerLock(protection,LayerLock::Pixels) || hasLayerLock(protection,LayerLock::Structure))
                return {DocumentHistoryCode::EditRejected};
            if (hasLayerLock(protection,LayerLock::Transparency))
                for (std::size_t i=0;i<content.pixels.pixels.size();++i)
                    if ((content.pixels.pixels[i]>>24)!=(original->pixels.pixels[i]>>24))
                        return {DocumentHistoryCode::EditRejected};
            const auto pixelCount=content.pixels.pixels.size();
            if (pixelCount>limits.maximumBitmapInputBytes/sizeof(std::uint32_t))
                return {DocumentHistoryCode::LimitExceeded};
            // History retains the complete new asset. Reject before encoding a
            // payload that cannot possibly fit the configured logical budget.
            if (pixelCount>limits.maximumBytes/sizeof(std::uint32_t))
                return {DocumentHistoryCode::BudgetExceeded};
            const auto originalId=original->id,newId=content.id;
            auto change=std::make_unique<LayerTransactionChange>();
            change->beforeLayers=d->layers; change->afterLayers=d->layers;
            change->beforeFrames=d->frames; change->afterFrames=d->frames;
            change->beforeArtboards=d->artboards; change->afterArtboards=d->artboards;
            change->beforeRelations=d->relations; change->afterRelations=d->relations;
            auto &replacement=change->afterLayers[*position];

            // Keys are kept sorted by frame and layer ID, as required by the
            // document model. A restore key bounds the edit to one frame.
            const auto putKey=[&](FrameIndex index,const std::string &asset) {
                auto owner=std::lower_bound(change->afterFrames.begin(),change->afterFrames.end(),index,
                    [](const Frame &candidate,FrameIndex value) { return candidate.index<value; });
                if (owner==change->afterFrames.end() || owner->index!=index)
                    owner=change->afterFrames.insert(owner,Frame{index,{}});
                auto key=std::lower_bound(owner->keyframes.begin(),owner->keyframes.end(),layerId,
                    [](const Keyframe &candidate,std::string_view value) { return candidate.layerId<value; });
                if (key!=owner->keyframes.end() && key->layerId==layerId) key->assetId=asset;
                else owner->keyframes.insert(key,Keyframe{std::string(layerId),asset});
            };
            const auto containsKey=[&](FrameIndex index) {
                const auto owner=std::lower_bound(change->afterFrames.begin(),change->afterFrames.end(),index,
                    [](const Frame &candidate,FrameIndex value) { return candidate.index<value; });
                return owner!=change->afterFrames.end() && owner->index==index &&
                    std::any_of(owner->keyframes.begin(),owner->keyframes.end(),
                        [&](const Keyframe &key) { return key.layerId==layerId; });
            };
            if (auto *fixed=std::get_if<StaticBitmapLayer>(&replacement); fixed && d->timeline.frameCount==1) {
                fixed->content.assetId=newId;
            } else {
                if (auto *fixed=std::get_if<StaticBitmapLayer>(&replacement)) {
                    auto properties=fixed->properties;
                    replacement=DynamicBitmapLayer{std::move(properties),{}};
                    putKey(0,originalId);
                }
                putKey(frame,newId);
                if (frame<d->timeline.frameCount-1 && !containsKey(frame+1)) putKey(frame+1,originalId);
                auto &indices=std::get<DynamicBitmapLayer>(replacement).content.frameIndices;
                indices.clear();
                for (const auto &owner:change->afterFrames)
                    if (std::any_of(owner.keyframes.begin(),owner.keyframes.end(),
                        [&](const Keyframe &key) { return key.layerId==layerId; })) indices.push_back(owner.index);
            }
            for (const auto &asset:d->assets) change->beforeAssets.push_back(assetId(asset));
            change->afterAssets=change->beforeAssets;
            change->afterAssets.push_back(newId);

            std::size_t bytes=sizeof(Entry)+sizeof(LayerTransactionChange);
            if (!addSize(bytes,label.size()) || !addElements(bytes,d->layers.size(),2*sizeof(Layer)) ||
                !frameBytes(bytes,change->beforeFrames) || !frameBytes(bytes,change->afterFrames) ||
                !artboardBytes(bytes,change->beforeArtboards) || !artboardBytes(bytes,change->afterArtboards) ||
                !relationsSize(bytes,change->beforeRelations) || !relationsSize(bytes,change->afterRelations))
                return {DocumentHistoryCode::BudgetExceeded};
            const std::vector<KeyframePlacement> keys;
            for (const auto *layers:std::array<const std::vector<Layer> *,2>{&change->beforeLayers,&change->afterLayers})
                for (const auto &layer:*layers)
                    if (!layerStructureSize(bytes,layer,keys)) return {DocumentHistoryCode::BudgetExceeded};
            for (const auto *list:std::array<const std::vector<std::string> *,2>{&change->beforeAssets,&change->afterAssets}) {
                if (!addElements(bytes,list->size(),sizeof(std::string))) return {DocumentHistoryCode::BudgetExceeded};
                for (const auto &id:*list) if (!addSize(bytes,id.size())) return {DocumentHistoryCode::BudgetExceeded};
            }
            const Asset newAsset{std::move(content)};
            auto encoded=detail::encodeHistoryAsset(newAsset);
            if (!addSize(bytes,sizeof(LayerTransactionAsset)) || !addSize(bytes,newId.size()) ||
                !addSize(bytes,encoded.size())) return {DocumentHistoryCode::BudgetExceeded};
            detail::AssetHistorySnapshot sourceGuard;
            const auto captured=detail::AssetHistorySnapshot::capture(*resolved,rasterAllowance(bytes),sourceGuard);
            if (captured==detail::AssetHistorySnapshot::Code::BudgetExceeded) return {DocumentHistoryCode::BudgetExceeded};
            if (captured!=detail::AssetHistorySnapshot::Code::None) return {DocumentHistoryCode::InvalidArgument};
            if (!addSize(bytes,sourceGuard.bytes()) || !canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            change->guards.push_back(std::move(sourceGuard));
            change->added.push_back({newId,d->assets.size(),std::move(encoded)});
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<std::unique_ptr<LayerTransactionChange>>(std::move(change));
            const auto code=applyLayerTransactionChange(*std::get<std::unique_ptr<LayerTransactionChange>>(entry->change),true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryCode applyStructure(const LayerStructureChange &change,bool forward,bool initial) {
        const auto *d=document();
        if (!d) return DocumentHistoryCode::NotBound;
        const auto &id=layerProperties(change.layer).id;
        const bool inserting=forward==change.inserted;
        if (change.relationsBefore && d->relations!=(forward ? *change.relationsBefore : *change.relationsAfter))
            return DocumentHistoryCode::Conflict;
        const auto *existing=findLayer(*d,id);
        if (inserting) {
            if (existing || !sameNeighbours(*d,change,false) || !sameKeys(*d,id,{}))
                return DocumentHistoryCode::Conflict;
        } else if (!existing || !sameNeighbours(*d,change,true) ||
                   layerProperties(d->layers[change.index]).id!=id ||
                   !sameLayer(*existing,change.layer) || !sameKeys(*d,id,change.keys)) {
            return DocumentHistoryCode::Conflict;
        }
        const auto perform=[&](DocumentEditor &editor) {
            if (!inserting) return editor.removeLayer(id);
            if (keyframedLayerSource(change.layer))
                return editor.insertKeyframedLayer(change.layer,change.keys,change.index);
            return editor.insertLayer(change.layer,change.index);
        };
        if (file) {
            if (change.relationsBefore) {
                DocumentHistoryCode failure=DocumentHistoryCode::None;
                const auto outcome=file->edit([&](Document &draft) {
                    DocumentEditor editor(draft); const auto result=perform(editor);
                    if (!result.ok() || !result.changed) {
                        failure=initial ? layerFailure(result,file) : DocumentHistoryCode::Conflict; return false;
                    }
                    draft.relations=forward ? *change.relationsAfter : *change.relationsBefore;
                    if (!validate(draft).ok()) { failure=DocumentHistoryCode::Conflict; return false; }
                    return true;
                });
                if (!outcome.ok()) return failure!=DocumentHistoryCode::None ? failure : fileFailure(file);
                return outcome.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
            }
            DocumentEditor editor(*file);
            if (!editor.isBound()) return DocumentHistoryCode::NotBound;
            const auto result=perform(editor);
            if (!result.ok()) {
                if (!initial && result.code!=DocumentEditCode::PersistenceFailed && result.code!=DocumentEditCode::NotBound)
                    return DocumentHistoryCode::Conflict;
                return layerFailure(result,file);
            }
            return result.changed?DocumentHistoryCode::None:DocumentHistoryCode::Conflict;
        }
        StructureGuard guard(*memory,keyframedLayerSource(change.layer)!=nullptr,bool(change.relationsBefore));
        DocumentEditor editor(*memory);
        const auto result=perform(editor);
        if (!result.ok()) {
            if (!initial && result.code!=DocumentEditCode::NotBound) return DocumentHistoryCode::Conflict;
            return layerFailure(result,nullptr);
        }
        if (!result.changed) return DocumentHistoryCode::Conflict;
        if (change.relationsBefore) {
            memory->relations=forward ? *change.relationsAfter : *change.relationsBefore;
            if (!validate(*memory).ok()) return DocumentHistoryCode::Conflict;
        }
        guard.published=true;
        return DocumentHistoryCode::None;
    }
    DocumentHistoryResult insertStructure(Layer layer,std::vector<KeyframePlacement> keys,
                                          std::size_t index,std::string_view label,bool animated) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            if (layer.valueless_by_exception()) return {DocumentHistoryCode::InvalidArgument};
            auto *source=keyframedLayerSource(layer);
            if (animated!=bool(source) || (!animated && !keys.empty())) return {DocumentHistoryCode::InvalidArgument};
            const auto &id=layerProperties(layer).id; const auto *d=document();
            if (id.empty() || findLayer(*d,id)) return {DocumentHistoryCode::InvalidArgument};
            if (index==AppendDocumentIndex) index=d->layers.size();
            if (index>d->layers.size()) return {DocumentHistoryCode::InvalidArgument};
            if (animated) {
                std::sort(keys.begin(),keys.end(),[](const auto &a,const auto &b){return a.frame<b.frame;});
                if (keys.empty() || keys.front().frame!=0) return {DocumentHistoryCode::InvalidArgument};
                for (std::size_t i=0;i<keys.size();++i)
                    if (keys[i].frame>=d->timeline.frameCount || (i && keys[i-1].frame==keys[i].frame))
                        return {DocumentHistoryCode::InvalidArgument};
                source->frameIndices.clear();
            }
            std::size_t bytes=sizeof(Entry);
            if (!addSize(bytes,sizeof(LayerStructureChange)) || !addSize(bytes,label.size()) ||
                (index && !addSize(bytes,layerProperties(d->layers[index-1]).id.size())) ||
                (index<d->layers.size() && !addSize(bytes,layerProperties(d->layers[index]).id.size())) ||
                !layerStructureSize(bytes,layer,keys) ||
                (animated && !addElements(bytes,keys.size(),sizeof(FrameIndex))) || !canAdmit(bytes))
                return {DocumentHistoryCode::BudgetExceeded};
            if (source) {
                std::vector<FrameIndex> indices; indices.reserve(keys.size());
                for (const auto &key:keys) indices.push_back(key.frame);
                source->frameIndices.swap(indices);
            }
            std::optional<std::string> previous,next;
            if (index) previous=layerProperties(d->layers[index-1]).id;
            if (index<d->layers.size()) next=layerProperties(d->layers[index]).id;
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<std::unique_ptr<LayerStructureChange>>(std::make_unique<LayerStructureChange>(
                LayerStructureChange{std::move(layer),std::move(keys),std::move(previous),std::move(next),index,true}));
            const auto code=applyStructure(*std::get<std::unique_ptr<LayerStructureChange>>(entry->change),true,true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult removeStructure(std::string_view id,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const std::string key(id); const auto *d=document(); const auto index=layerIndex(*d,key);
            if (!index) return {DocumentHistoryCode::InvalidArgument};
            const auto &layer=d->layers[*index];
            auto nextRelations=d->relations;
            nextRelations.removeLayer(key);
            const bool changesRelations=nextRelations!=d->relations;
            std::size_t bytes=sizeof(Entry);
            const std::vector<KeyframePlacement> none;
            if (!addSize(bytes,sizeof(LayerStructureChange)) || !addSize(bytes,label.size()) ||
                (*index && !addSize(bytes,layerProperties(d->layers[*index-1]).id.size())) ||
                (*index+1<d->layers.size() && !addSize(bytes,layerProperties(d->layers[*index+1]).id.size())) ||
                !layerStructureSize(bytes,layer,none) ||
                (changesRelations && (!relationsSize(bytes,d->relations) || !relationsSize(bytes,nextRelations))) || !canAdmit(bytes))
                return {DocumentHistoryCode::BudgetExceeded};
            std::vector<KeyframePlacement> keys;
            for (const auto &frame:d->frames) for (const auto &keyframe:frame.keyframes) {
                if (keyframe.layerId!=key) continue;
                if (!addSize(bytes,sizeof(KeyframePlacement)) || !addSize(bytes,keyframe.assetId.size()) || !canAdmit(bytes))
                    return {DocumentHistoryCode::BudgetExceeded};
                keys.push_back({frame.index,keyframe.assetId});
            }
            std::optional<std::string> previous,next;
            if (*index) previous=layerProperties(d->layers[*index-1]).id;
            if (*index+1<d->layers.size()) next=layerProperties(d->layers[*index+1]).id;
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            auto change=std::make_unique<LayerStructureChange>(LayerStructureChange{layer,std::move(keys),std::move(previous),std::move(next),*index,false});
            if (changesRelations) { change->relationsBefore=d->relations; change->relationsAfter=std::move(nextRelations); }
            entry->change.emplace<std::unique_ptr<LayerStructureChange>>(std::move(change));
            const auto code=applyStructure(*std::get<std::unique_ptr<LayerStructureChange>>(entry->change),true,true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryCode applyAssetLayer(const AssetLayerChange &change,bool inserting,Asset *initial,
                                         bool initialEdit=false) {
        const auto *d=document();
        if (!d) return DocumentHistoryCode::NotBound;
        const std::string id(change.asset.id());
        const auto &layerId=layerProperties(change.layer.layer).id;
        if (change.layer.relationsBefore &&
            d->relations!=(inserting ? *change.layer.relationsAfter : *change.layer.relationsBefore))
            return DocumentHistoryCode::Conflict;
        const auto *existingAsset=findAsset(*d,id);
        const auto *existingLayer=findLayer(*d,layerId);
        if (inserting) {
            if (existingAsset || findAudioAsset(*d,id) || existingLayer ||
                !sameAssetNeighbours(*d,change,false) || !sameNeighbours(*d,change.layer,false) ||
                !sameKeys(*d,layerId,{})) return DocumentHistoryCode::Conflict;
        } else {
            if (initialEdit && hasLayerLock(d->relations.locks.effective(*d,layerId),LayerLock::Structure))
                return DocumentHistoryCode::EditRejected;
            if (!existingAsset || !existingLayer || !sameAssetNeighbours(*d,change,true) ||
                assetId(d->assets[change.assetIndex])!=id || (!initialEdit && !change.asset.matches(*existingAsset)) ||
                !sameNeighbours(*d,change.layer,true) ||
                layerProperties(d->layers[change.layer.index]).id!=layerId ||
                !sameLayer(*existingLayer,change.layer.layer) || !sameKeys(*d,layerId,{}))
                return DocumentHistoryCode::Conflict;
            const auto references=assetReferences(*d,id);
            for (const auto &reference:references)
                if (reference.layerIndex!=change.layer.index) return DocumentHistoryCode::Conflict;
        }
        std::optional<Asset> prepared;
        if (inserting) prepared.emplace(initial ? std::move(*initial) : change.asset.restore());
        DocumentHistoryCode failure=DocumentHistoryCode::None;
        const auto mutate=[&](Document &document,detail::AssetLayerGuard &guard) {
            if (inserting) {
                guard.insert(change.assetIndex,std::move(*prepared));
                document.layers.insert(document.layers.begin()+static_cast<std::ptrdiff_t>(change.layer.index),
                                       change.layer.layer);
            } else {
                document.layers.erase(document.layers.begin()+static_cast<std::ptrdiff_t>(change.layer.index));
                guard.remove(change.assetIndex);
            }
            if (change.layer.relationsBefore)
                document.relations=inserting ? *change.layer.relationsBefore : *change.layer.relationsAfter;
            // Every real edit upgrades authorship to the current model. Validate
            // the candidate against that model before the authorship is stamped.
            document.formatVersion.minor=CurrentFormatMinor;
            if (!validate(document).ok()) {
                failure=initialEdit ? DocumentHistoryCode::InvalidArgument : DocumentHistoryCode::Conflict;
                return false;
            }
            recordDocumentChange(document);
            return true;
        };
        if (file) {
            if (change.layer.relationsBefore) {
                const auto result=file->edit([&](Document &draft) {
                    detail::AssetLayerGuard guard(draft,true);
                    if (!mutate(draft,guard)) return false;
                    guard.publish(); return true;
                });
                if (!result.ok()) return failure!=DocumentHistoryCode::None ? failure : fileFailure(file);
                return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
            }
            bool fallback=false;
#ifdef IISHAREDCANVAS_NATIVE_API
            auto result=file->editAssetLayerImpl(id,mutate,fallback);
            if (fallback)
                result=file->edit([&](Document &draft) {
                    detail::AssetLayerGuard guard(draft);
                    if (!mutate(draft,guard)) return false;
                    guard.publish(); return true;
                });
#else
            auto result=file->edit([&](Document &draft) {
                detail::AssetLayerGuard guard(draft);
                if (!mutate(draft,guard)) return false;
                guard.publish(); return true;
            });
#endif
            if (!result.ok()) return failure!=DocumentHistoryCode::None ? failure : fileFailure(file);
            return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        detail::AssetLayerGuard guard(*memory,bool(change.layer.relationsBefore));
        if (!mutate(*memory,guard)) return failure;
        guard.publish(); return DocumentHistoryCode::None;
    }
    DocumentHistoryResult insertAssetLayer(Asset asset,Layer layer,std::size_t layerIndex,
                                           std::size_t assetPosition,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            if (asset.valueless_by_exception() || layer.valueless_by_exception() || std::holds_alternative<VideoAsset>(asset) ||
                denseControlAsset(asset))
                return {DocumentHistoryCode::InvalidArgument};
            const auto &id=assetId(asset); const auto &layerId=layerProperties(layer).id;
            const auto *source=staticLayerSource(layer); const auto *d=document();
            if (id.empty() || layerId.empty() || !source || source->assetId!=id ||
                contentKind(asset)!=contentKind(layer) || findAsset(*d,id) || findAudioAsset(*d,id) ||
                findLayer(*d,layerId)) return {DocumentHistoryCode::InvalidArgument};
            if (layerIndex==AppendDocumentIndex) layerIndex=d->layers.size();
            if (assetPosition==AppendDocumentIndex) assetPosition=d->assets.size();
            if (layerIndex>d->layers.size() || assetPosition>d->assets.size()) return {DocumentHistoryCode::InvalidArgument};
            std::size_t inputBytes=0;
            if (!bitmapInputSize(asset,inputBytes)) return {DocumentHistoryCode::LimitExceeded};
            if (inputBytes>limits.maximumBitmapInputBytes) return {DocumentHistoryCode::LimitExceeded};
            std::size_t bytes=sizeof(Entry)+sizeof(AssetLayerChange)-sizeof(detail::AssetHistorySnapshot);
            const std::vector<KeyframePlacement> noKeys;
            if (!addSize(bytes,label.size()) || !layerStructureSize(bytes,layer,noKeys) ||
                (layerIndex && !addSize(bytes,layerProperties(d->layers[layerIndex-1]).id.size())) ||
                (layerIndex<d->layers.size() && !addSize(bytes,layerProperties(d->layers[layerIndex]).id.size())) ||
                (assetPosition && !addSize(bytes,assetId(d->assets[assetPosition-1]).size())) ||
                (assetPosition<d->assets.size() && !addSize(bytes,assetId(d->assets[assetPosition]).size())))
                return {DocumentHistoryCode::BudgetExceeded};
            detail::AssetHistorySnapshot snapshot;
            const auto captured=detail::AssetHistorySnapshot::capture(asset,rasterAllowance(bytes),snapshot);
            if (captured==detail::AssetHistorySnapshot::Code::BudgetExceeded) return {DocumentHistoryCode::BudgetExceeded};
            if (captured!=detail::AssetHistorySnapshot::Code::None) return {DocumentHistoryCode::InvalidArgument};
            if (!addSize(bytes,snapshot.bytes()) || !canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            std::optional<std::string> layerBefore,layerAfter,assetBefore,assetAfter;
            if (layerIndex) layerBefore=layerProperties(d->layers[layerIndex-1]).id;
            if (layerIndex<d->layers.size()) layerAfter=layerProperties(d->layers[layerIndex]).id;
            if (assetPosition) assetBefore=assetId(d->assets[assetPosition-1]);
            if (assetPosition<d->assets.size()) assetAfter=assetId(d->assets[assetPosition]);
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            LayerStructureChange structure{std::move(layer),{},std::move(layerBefore),std::move(layerAfter),layerIndex,true};
            entry->change.emplace<std::unique_ptr<AssetLayerChange>>(std::make_unique<AssetLayerChange>(
                AssetLayerChange{std::move(structure),std::move(snapshot),std::move(assetBefore),std::move(assetAfter),assetPosition}));
            const auto code=applyAssetLayer(*std::get<std::unique_ptr<AssetLayerChange>>(entry->change),true,&asset,true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult removeAssetLayer(std::string_view id,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const std::string key(id); const auto *d=document(); const auto layerPosition=layerIndex(*d,key);
            if (!layerPosition) return {DocumentHistoryCode::InvalidArgument};
            const auto &layer=d->layers[*layerPosition]; const auto *source=staticLayerSource(layer);
            if (hasLayerLock(d->relations.locks.effective(*d,key),LayerLock::Structure))
                return {DocumentHistoryCode::EditRejected};
            if (!source) return {DocumentHistoryCode::InvalidArgument};
            const auto position=assetIndex(*d,source->assetId);
            if (!position) return {DocumentHistoryCode::InvalidArgument};
            const auto &asset=d->assets[*position];
            if (std::holds_alternative<VideoAsset>(asset) || denseControlAsset(asset)) return {DocumentHistoryCode::InvalidArgument};
            for (const auto &reference:assetReferences(*d,source->assetId))
                if (reference.layerIndex!=*layerPosition) return {DocumentHistoryCode::Conflict};
            std::size_t inputBytes=0;
            if (!bitmapInputSize(asset,inputBytes) || inputBytes>limits.maximumBitmapInputBytes)
                return {DocumentHistoryCode::LimitExceeded};
            std::size_t bytes=sizeof(Entry)+sizeof(AssetLayerChange)-sizeof(detail::AssetHistorySnapshot);
            auto nextRelations=d->relations; nextRelations.removeLayer(key);
            const bool changesRelations=nextRelations!=d->relations;
            const std::vector<KeyframePlacement> noKeys;
            if (!addSize(bytes,label.size()) || !layerStructureSize(bytes,layer,noKeys) ||
                (changesRelations && (!relationsSize(bytes,d->relations) || !relationsSize(bytes,nextRelations))) ||
                (*layerPosition && !addSize(bytes,layerProperties(d->layers[*layerPosition-1]).id.size())) ||
                (*layerPosition+1<d->layers.size() && !addSize(bytes,layerProperties(d->layers[*layerPosition+1]).id.size())) ||
                (*position && !addSize(bytes,assetId(d->assets[*position-1]).size())) ||
                (*position+1<d->assets.size() && !addSize(bytes,assetId(d->assets[*position+1]).size())))
                return {DocumentHistoryCode::BudgetExceeded};
            detail::AssetHistorySnapshot snapshot;
            const auto captured=detail::AssetHistorySnapshot::capture(asset,rasterAllowance(bytes),snapshot);
            if (captured==detail::AssetHistorySnapshot::Code::BudgetExceeded) return {DocumentHistoryCode::BudgetExceeded};
            if (captured!=detail::AssetHistorySnapshot::Code::None) return {DocumentHistoryCode::InvalidArgument};
            if (!addSize(bytes,snapshot.bytes()) || !canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            std::optional<std::string> layerBefore,layerAfter,assetBefore,assetAfter;
            if (*layerPosition) layerBefore=layerProperties(d->layers[*layerPosition-1]).id;
            if (*layerPosition+1<d->layers.size()) layerAfter=layerProperties(d->layers[*layerPosition+1]).id;
            if (*position) assetBefore=assetId(d->assets[*position-1]);
            if (*position+1<d->assets.size()) assetAfter=assetId(d->assets[*position+1]);
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            LayerStructureChange structure{layer,{},std::move(layerBefore),std::move(layerAfter),*layerPosition,false};
            if (changesRelations) { structure.relationsBefore=d->relations; structure.relationsAfter=std::move(nextRelations); }
            entry->change.emplace<std::unique_ptr<AssetLayerChange>>(std::make_unique<AssetLayerChange>(
                AssetLayerChange{std::move(structure),std::move(snapshot),std::move(assetBefore),std::move(assetAfter),*position}));
            const auto code=applyAssetLayer(*std::get<std::unique_ptr<AssetLayerChange>>(entry->change),false,nullptr,true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryCode applyLinkedCanvas(const LinkedCanvasChange &change, bool forward) {
        const auto &expected = forward ? change.before : change.after;
        const auto &replacement = forward ? change.after : change.before;
        const auto *found = document() ? findAsset(*document(), expected.id) : nullptr;
        const auto *current = found ? std::get_if<LinkedCanvasAsset>(found) : nullptr;
        if (!current || *current != expected) return DocumentHistoryCode::Conflict;
        DocumentEditor editor;
        const auto bound = file ? editor.bind(*file) : editor.bind(*memory);
        if (!bound.ok()) return layerFailure(bound,file);
        const auto result = editor.replaceLinkedCanvasAsset(expected.id,replacement);
        return result.ok() ? (result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict)
                           : layerFailure(result,file);
    }
    // Compare only the two supported procedural alternatives; unrelated asset
    // kinds have different equality and history policies.
    static bool sameProcedural(const Asset &a,const Asset &b) {
        if (const auto *fill=std::get_if<FillLayerContent>(&a)) {
            const auto *other=std::get_if<FillLayerContent>(&b); return other && *fill==*other;
        }
        if (const auto *adjustment=std::get_if<AdjustmentLayerContent>(&a)) {
            const auto *other=std::get_if<AdjustmentLayerContent>(&b); return other && *adjustment==*other;
        }
        return false;
    }
    DocumentHistoryCode applyProcedural(const ProceduralContentChange &change,bool forward) {
        const auto &expected=forward ? change.before : change.after;
        const auto &replacement=forward ? change.after : change.before;
        const auto *found=document() ? findAsset(*document(),assetId(expected)) : nullptr;
        if (!found || !sameProcedural(*found,expected)) return DocumentHistoryCode::Conflict;
        DocumentEditor editor;
        const auto bound=file ? editor.bind(*file) : editor.bind(*memory);
        if (!bound.ok()) return layerFailure(bound,file);
        const auto result=editor.replaceProceduralContent(replacement);
        return result.ok() ? (result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict)
                           : layerFailure(result,file);
    }
    DocumentHistoryResult procedural(Asset replacement,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            if (!std::holds_alternative<FillLayerContent>(replacement) &&
                !std::holds_alternative<AdjustmentLayerContent>(replacement)) return {DocumentHistoryCode::InvalidArgument};
            const auto *current=findAsset(*document(),assetId(replacement));
            if (!current || current->index()!=replacement.index()) return {DocumentHistoryCode::InvalidArgument};
            if (sameProcedural(*current,replacement)) {
                // A no-op on a file still checks its external writer token;
                // otherwise a stale session could report a false success.
                if (file) {
                    DocumentEditor editor(*file);
                    if (!editor.isBound()) return {DocumentHistoryCode::NotBound};
                    const auto result=editor.replaceProceduralContent(replacement);
                    if (!result.ok()) return {layerFailure(result,file)};
                }
                return {};
            }
            std::size_t bytes=sizeof(Entry)+sizeof(ProceduralContentChange);
            const auto charge=[&](const Asset &asset) {
                if (!addSize(bytes,assetId(asset).size())) return false;
                if (const auto *fill=std::get_if<FillLayerContent>(&asset))
                    return addSize(bytes,fill->patternAssetId.size()) && addElements(bytes,fill->stops.size(),sizeof(FillGradientStop));
                return true;
            };
            if (!addSize(bytes,label.size()) || !charge(*current) || !charge(replacement) || !canAdmit(bytes))
                return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<std::unique_ptr<ProceduralContentChange>>(
                std::make_unique<ProceduralContentChange>(ProceduralContentChange{*current,std::move(replacement)}));
            const auto code=applyProcedural(*std::get<std::unique_ptr<ProceduralContentChange>>(entry->change),true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult linkedCanvas(LinkedCanvasAsset replacement,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const auto *found=findAsset(*document(),replacement.id);
            const auto *current=found ? std::get_if<LinkedCanvasAsset>(found) : nullptr;
            if (!current) return {DocumentHistoryCode::InvalidArgument};
            if (*current==replacement) return {};
            std::size_t bytes=sizeof(Entry)+sizeof(LinkedCanvasChange);
            if (!addSize(bytes,label.size()) || !addSize(bytes,current->id.size()) ||
                !addSize(bytes,current->document.size()) || !addSize(bytes,replacement.id.size()) ||
                !addSize(bytes,replacement.document.size()) || !canAdmit(bytes))
                return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<std::unique_ptr<LinkedCanvasChange>>(
                std::make_unique<LinkedCanvasChange>(LinkedCanvasChange{*current,std::move(replacement)}));
            const auto code=applyLinkedCanvas(*std::get<std::unique_ptr<LinkedCanvasChange>>(entry->change),true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    // Replay a previously validated relationship command without letting a lock
    // introduced by that same command prevent its own undo or redo.
    DocumentHistoryCode applyRelations(const RelationsChange &change,bool forward,bool initial) {
        const auto &expected=forward ? change.before : change.after;
        const auto &replacement=forward ? change.after : change.before;
        if (!document() || document()->relations!=expected) return DocumentHistoryCode::Conflict;
        if (initial) {
            DocumentEditor editor;
            const auto bound=file ? editor.bind(*file) : editor.bind(*memory);
            if (!bound.ok()) return layerFailure(bound,file);
            const auto result=editor.setSelectionLayerRelations(replacement);
            return result.ok() ? (result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict)
                               : layerFailure(result,file);
        }
        const auto mutate=[&](Document &d) {
            if (d.relations!=expected) return false;
            d.relations=replacement; d.formatVersion={CurrentFormatMajor,CurrentFormatMinor};
            if (!validate(d).ok()) return false;
            recordDocumentChange(d); return true;
        };
        if (file) {
            const auto result=file->edit(mutate);
            return result.ok() && result.changed ? DocumentHistoryCode::None : fileFailure(file);
        }
        auto replacementCopy=replacement;
        auto previousAuthorship=memory->authorship;
        const auto previousVersion=memory->formatVersion;
        using std::swap; swap(memory->relations,replacementCopy);
        try {
            memory->formatVersion={CurrentFormatMajor,CurrentFormatMinor};
            if (!validate(*memory).ok()) {
                swap(memory->relations,replacementCopy); memory->formatVersion=previousVersion;
                return DocumentHistoryCode::Conflict;
            }
            recordDocumentChange(*memory);
            return DocumentHistoryCode::None;
        } catch (...) {
            swap(memory->relations,replacementCopy); swap(memory->authorship,previousAuthorship);
            memory->formatVersion=previousVersion; throw;
        }
    }
    DocumentHistoryResult relations(SelectionLayerRelations next,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            if (document()->relations==next) return {};
            std::size_t bytes=sizeof(Entry)+sizeof(RelationsChange);
            if (!addSize(bytes,label.size()) || !relationsSize(bytes,document()->relations) ||
                !relationsSize(bytes,next) || !canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<std::unique_ptr<RelationsChange>>(
                std::make_unique<RelationsChange>(RelationsChange{document()->relations,std::move(next)}));
            const auto code=applyRelations(*std::get<std::unique_ptr<RelationsChange>>(entry->change),true,true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }

    // Replay checks both the project and media registry before preparing a
    // detached replacement. The guard restores authorship and format on every
    // rejected in-memory or working-file transaction.
    DocumentHistoryCode applySequenceProject(const SequenceProjectChange &change, bool forward) {
        const auto &expected = forward ? change.before : change.after;
        const auto &next = forward ? change.after : change.before;
        const auto &expectedMedia = forward ? change.mediaBefore : change.mediaAfter;
        const auto &nextMedia = forward ? change.mediaAfter : change.mediaBefore;
        const auto *owner = document();
        if (!owner || owner->sequenceProject != expected || owner->sequenceMedia != expectedMedia)
            return DocumentHistoryCode::Conflict;
        DocumentHistoryCode failure = DocumentHistoryCode::None;
        const auto mutate = [&](Document &target) {
            if (target.sequenceProject != expected || target.sequenceMedia != expectedMedia) {
                failure = DocumentHistoryCode::Conflict;
                return false;
            }
            auto project = next;
            auto media = nextMedia;
            auto authorship = target.authorship;
            const auto version = target.formatVersion;
            using std::swap;
            swap(target.sequenceProject, project);
            swap(target.sequenceMedia, media);
            struct Guard {
                Document &target;
                std::optional<TimelineProject> &project;
                std::vector<TimelineOwnedMedia> &media;
                DocumentAuthorship &authorship;
                FormatVersion version;
                bool committed = false;
                ~Guard() {
                    if (committed) return;
                    using std::swap;
                    swap(target.sequenceProject, project);
                    swap(target.sequenceMedia, media);
                    swap(target.authorship, authorship);
                    target.formatVersion = version;
                }
            } guard{target, project, media, authorship, version};
            target.formatVersion.minor = CurrentFormatMinor;
            if (!validate(target).ok()) {
                failure = DocumentHistoryCode::InvalidArgument;
                return false;
            }
            recordDocumentChange(target);
            guard.committed = true;
            return true;
        };
        if (file) {
            const auto result = file->edit(mutate);
            if (!result.ok()) return failure != DocumentHistoryCode::None ? failure : fileFailure(file);
            return result.changed ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        return mutate(*memory) ? DocumentHistoryCode::None : failure;
    }

    DocumentHistoryResult sequenceProject(std::optional<TimelineProject> next,
        std::vector<TimelineOwnedMedia> media, std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy = true;
        struct Guard { bool &busy; ~Guard() { busy = false; } } guard{busy};
        if (const auto state = current(); state != DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            const auto *owner = document();
            if (owner->sequenceProject == next && owner->sequenceMedia == media) return {};
            // Reuse already detached payloads before calculating the budget.
            // New caller-owned data is copied only after admission succeeds.
            std::unordered_set<const void *> unchangedMedia;
            for (auto &item : media) {
                if (!item.bytes) return {DocumentHistoryCode::InvalidArgument};
                const auto found = std::find_if(owner->sequenceMedia.begin(), owner->sequenceMedia.end(),
                    [&](const auto &old) { return old == item; });
                if (found != owner->sequenceMedia.end()) {
                    item.bytes = found->bytes;
                    unchangedMedia.insert(item.bytes.get());
                }
            }
            std::size_t bytes = sizeof(Entry) + sizeof(SequenceProjectChange);
            if (!addSize(bytes, label.size())) return {DocumentHistoryCode::BudgetExceeded};
            const std::array<const std::optional<TimelineProject> *, 2> projects{&owner->sequenceProject, &next};
            for (const auto *project : projects) {
                if (!*project) continue;
                auto encoded = detail::encodeTimelineProject(**project, {});
                if (encoded.error.code != IiscErrorCode::None) return {DocumentHistoryCode::InvalidArgument};
                if (!addSize(bytes, encoded.bytes.size())) return {DocumentHistoryCode::BudgetExceeded};
            }
            std::unordered_set<const void *> retainedMedia;
            const std::array<const std::vector<TimelineOwnedMedia> *, 2> mediaSets{&owner->sequenceMedia, &media};
            for (const auto *items : mediaSets) {
                if (!addElements(bytes, items->size(), sizeof(TimelineOwnedMedia))) return {DocumentHistoryCode::BudgetExceeded};
                for (const auto &item : *items) {
                    if (!addSize(bytes, item.id.size()) || !addSize(bytes, item.mediaType.size()) ||
                        (item.bytes && !unchangedMedia.contains(item.bytes.get()) &&
                         retainedMedia.insert(item.bytes.get()).second &&
                         !addSize(bytes, item.bytes->size()))) return {DocumentHistoryCode::BudgetExceeded};
                }
            }
            if (!canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            std::unordered_set<const void *> ownedPointers;
            for (const auto &item : owner->sequenceMedia) ownedPointers.insert(item.bytes.get());
            std::unordered_map<const void *, std::shared_ptr<const std::vector<std::uint8_t>>> detached;
            for (auto &item : media) {
                if (ownedPointers.contains(item.bytes.get())) continue;
                const auto *key = item.bytes.get();
                auto [position, inserted] = detached.try_emplace(key);
                if (inserted) position->second = std::make_shared<const std::vector<std::uint8_t>>(*item.bytes);
                item.bytes = position->second;
            }
            undo.reserve(undo.size() + 1);
            auto entry = std::make_unique<Entry>();
            entry->label = label;
            entry->bytes = bytes;
            entry->change.emplace<std::unique_ptr<SequenceProjectChange>>(std::make_unique<SequenceProjectChange>(
                SequenceProjectChange{owner->sequenceProject, std::move(next), owner->sequenceMedia, std::move(media)}));
            const auto code = applySequenceProject(*std::get<std::unique_ptr<SequenceProjectChange>>(entry->change), true);
            if (code != DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }

    DocumentHistoryCode applyTimeline(const TimelineChange &change,bool forward,bool initialEdit,bool allowNoOp=false) {
        const auto &expected=forward?change.before:change.after;
        const auto &next=forward?change.after:change.before;
        if (!document() || !sameTimeline(document()->timeline,expected)) return DocumentHistoryCode::Conflict;
        DocumentHistoryCode failure=DocumentHistoryCode::None;
        const auto mutate=[&](Document &d) {
            if (!sameTimeline(d.timeline,expected)) { failure=DocumentHistoryCode::Conflict; return false; }
            if (sameTimeline(expected,next)) return true;
            d.timeline=next; d.formatVersion.minor=CurrentFormatMinor;
            if (!validate(d).ok()) {
                failure=initialEdit?DocumentHistoryCode::InvalidArgument:DocumentHistoryCode::Conflict;
                return false;
            }
            recordDocumentChange(d); return true;
        };
        if (file) {
            bool fallback=false;
#ifdef IISHAREDCANVAS_NATIVE_API
            auto result=file->editTimelineImpl(mutate,fallback);
            if (fallback) result=file->edit(mutate);
#else
            auto result=file->edit(mutate);
#endif
            if (!result.ok()) return failure!=DocumentHistoryCode::None?failure:fileFailure(file);
            return result.changed || allowNoOp ? DocumentHistoryCode::None : DocumentHistoryCode::Conflict;
        }
        if (allowNoOp && sameTimeline(expected,next)) return DocumentHistoryCode::None;
        detail::TimelineGuard guard(*memory);
        if (!mutate(*memory)) return failure;
        guard.publish(); return DocumentHistoryCode::None;
    }
    DocumentHistoryResult timeline(Timeline next,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        if (!next.frameRate.numerator || !next.frameRate.denominator || !next.frameCount)
            return {DocumentHistoryCode::InvalidArgument};
        try {
            TimelineChange change{document()->timeline,next};
            if (sameTimeline(change.before,next)) {
                if (file) {
                    const auto code=applyTimeline(change,true,true,true);
                    if (code!=DocumentHistoryCode::None) return {code};
                }
                return {};
            }
            std::size_t bytes=sizeof(Entry);
            if (!addSize(bytes,label.size()) || !canAdmit(bytes)) return {DocumentHistoryCode::BudgetExceeded};
            undo.reserve(undo.size()+1);
            auto entry=std::make_unique<Entry>(); entry->label=label; entry->bytes=bytes;
            entry->change.emplace<TimelineChange>(change);
            const auto code=applyTimeline(std::get<TimelineChange>(entry->change),true,true);
            if (code!=DocumentHistoryCode::None) return {code};
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryCode prepareBitmap(std::string_view id, std::string_view label,
                                      std::unique_ptr<Entry> &entry, std::size_t &overhead) {
        // Exhausted identifiers cannot wrap and later identify a different edit.
        if (!nextEntryId) return DocumentHistoryCode::LimitExceeded;
        const std::string key(id); const auto *d=document();
        if (!file) { DocumentEditor probe(*memory); if (!probe.isBound()) return DocumentHistoryCode::InvalidArgument; }
        const auto *asset=findAsset(*d,key);
        const bool finite=asset && std::holds_alternative<RasterAsset>(*asset);
        const bool chunked=asset && std::holds_alternative<ChunkedRasterAsset>(*asset);
        if (!finite && !chunked) return DocumentHistoryCode::InvalidArgument;
        overhead=sizeof(Entry);
        if (!addSize(overhead,finite?sizeof(BitmapEditor):sizeof(ChunkedBitmapEditor)) ||
            !addSize(overhead,id.size()) || !addSize(overhead,label.size()))
            return DocumentHistoryCode::BudgetExceeded;
        RasterHistoryLimits bitmapLimits;
        bitmapLimits.maximumBytes=rasterAllowance(overhead);
        bitmapLimits.maximumEntries=1;
        bitmapLimits.maximumPatches=limits.maximumBitmapPatches;
        bitmapLimits.maximumInputBytes=limits.maximumBitmapInputBytes;
        bitmapLimits.overflow=RasterHistoryOverflow::Reject;
        // Prepare the publication slot and all command ownership before any
        // pixel mutation. A successful write can then be published noexcept.
        undo.reserve(undo.size()+1);
        auto candidate=std::make_unique<Entry>(); candidate->label=label;
        if (finite) {
            auto editor=std::make_unique<BitmapEditor>(bitmapLimits);
            if (!(file?editor->bind(*file,key):editor->bind(*memory,key))) return DocumentHistoryCode::InvalidArgument;
            candidate->change.emplace<std::unique_ptr<BitmapEditor>>(std::move(editor));
        } else {
            auto editor=std::make_unique<ChunkedBitmapEditor>(bitmapLimits);
            if (!(file?editor->bind(*file,key):editor->bind(*memory,key))) return DocumentHistoryCode::InvalidArgument;
            candidate->change.emplace<std::unique_ptr<ChunkedBitmapEditor>>(std::move(editor));
        }
        entry=std::move(candidate);
        return DocumentHistoryCode::None;
    }
    DocumentHistoryResult bitmap(std::string_view id, std::span<const BitmapPatch> patches, std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            std::unique_ptr<Entry> entry; std::size_t overhead=0;
            if (const auto code=prepareBitmap(id,label,entry,overhead); code!=DocumentHistoryCode::None) return {code};
            const bool accepted=withBitmap(*entry,[&](auto &editor) { return editor.replacePatches(patches); });
            const auto result=withBitmap(*entry,[](const auto &editor) { return editor.lastHistoryResult(); });
            if (!accepted) return {bitmapFailure(result.code,file)};
            if (!result.changed) return {};
            entry->bytes=overhead+result.entryBytes;
            return publish(std::move(entry));
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult beginPreview(std::string_view id, std::string_view label,
                                       std::optional<PixelPreviewCanvasMode> staged = {}) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            std::unique_ptr<Entry> entry; std::size_t overhead=0;
            if (const auto code=prepareBitmap(id,label,entry,overhead); code!=DocumentHistoryCode::None) return {code};
            if (staged && !withBitmap(*entry,[&](auto &editor) { return detail::BitmapHistorySeed::stagePixels(editor,*staged); }))
                return {DocumentHistoryCode::InvalidArgument};
            const bool accepted=withBitmap(*entry,[&](auto &editor) { return editor.beginPixelPreview(entry->label); });
            const auto result=withBitmap(*entry,[](const auto &editor) { return editor.lastHistoryResult(); });
            if (!accepted) return {bitmapFailure(result.code,file)};
            previewOverhead=overhead; preview=std::move(entry); previewKind=PreviewKind::Pixels; return {};
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult replacePreview(std::span<const BitmapPatch> patches) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (!preview || previewKind!=PreviewKind::Pixels) return {DocumentHistoryCode::NoPreview};
        try {
            const auto before=revision();
            const bool accepted=withPreview([&](auto &editor) { return editor.replacePixelPreview(patches); });
            const auto result=withPreview([](const auto &editor) { return editor.lastHistoryResult(); });
            if (!accepted) return {bitmapFailure(result.code,file)};
            preview->bytes=result.changed?previewOverhead+result.entryBytes:0;
            const bool staged=withPreview([](const auto &editor) { return detail::BitmapHistorySeed::stagedPixels(editor); });
            const bool viewChanged=withPreview([](const auto &editor) { return !isEmpty(editor.dirtyBounds()); });
            collectPreviewDirty(); expectedRevision=revision();
            return {DocumentHistoryCode::None,staged ? viewChanged : expectedRevision!=before,preview->bytes};
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult finishPreview(bool commit) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        const auto state=current();
        if (!preview || previewKind!=PreviewKind::Pixels) {
            if (state!=DocumentHistoryCode::None && (commit || state!=DocumentHistoryCode::Conflict)) return {state};
            return {DocumentHistoryCode::NoPreview};
        }
        const bool staged=withPreview([](const auto &editor) { return detail::BitmapHistorySeed::stagedPixels(editor); });
        // A cancel can still safely restore the touched pixels after a tracked
        // outside edit; the bitmap journal checks those pixels first. The old
        // history remains conflicted until the caller explicitly resets it.
        if (state!=DocumentHistoryCode::None && (commit || (!staged && state!=DocumentHistoryCode::Conflict))) return {state};
        try {
            const auto before=revision();
            const bool accepted=withPreview([&](auto &editor) {
                if (!commit && staged) return detail::BitmapHistorySeed::discardStagedPixels(editor);
                return commit?editor.commitPixelPreview():editor.cancelPixelPreview();
            });
            const auto result=withPreview([](const auto &editor) { return editor.lastHistoryResult(); });
            if (!accepted) return {bitmapFailure(result.code,file)};
            const bool viewChanged=withPreview([](const auto &editor) { return !isEmpty(editor.dirtyBounds()); });
            collectPreviewDirty();
            if (commit && result.changed) {
                previewOverhead=0; previewKind=PreviewKind::None;
                return publish(std::move(preview));
            }
            preview.reset(); previewOverhead=0; previewKind=PreviewKind::None;
            if (state==DocumentHistoryCode::None) expectedRevision=revision();
            return {DocumentHistoryCode::None,!commit && (staged ? viewChanged : revision()!=before)};
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult abandonPreview() {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (!preview || previewKind!=PreviewKind::Pixels) return {DocumentHistoryCode::NoPreview};
        preview.reset(); previewOverhead=0; previewKind=PreviewKind::None;
        undo.clear(); redo.clear(); retained=0;
        if (bound()) expectedRevision=revision();
        return {};
    }
    DocumentHistoryResult beginStroke(std::string_view id,const BitmapBrush &brush,
                                      const StrokePoint &sample,StrokeCanvasMode mode,std::string_view label) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        if (!detail::validStrokeSample(sample)) return {DocumentHistoryCode::InvalidArgument};
        try {
            std::unique_ptr<Entry> entry; std::size_t overhead=0;
            if (const auto code=prepareBitmap(id,label,entry,overhead); code!=DocumentHistoryCode::None) return {code};
            if (!withBitmap(*entry,[&](auto &editor) { return detail::BitmapHistorySeed::canvasMode(editor,mode); }))
                return {DocumentHistoryCode::InvalidArgument};
            withBitmap(*entry,[&](auto &editor) { detail::BitmapHistorySeed::set(editor,nextStrokeSeed); });
            if (!withBitmap(*entry,[&](auto &editor) { return editor.setBrush(brush); }))
                return {DocumentHistoryCode::InvalidArgument};
            const auto before=revision();
            const bool accepted=withBitmap(*entry,[&](auto &editor) { return editor.beginStrokeInput(sample); });
            const auto result=withBitmap(*entry,[](const auto &editor) { return editor.lastHistoryResult(); });
            if (!accepted) return {bitmapFailure(result.code,file)};
            previewOverhead=overhead; preview=std::move(entry); previewKind=PreviewKind::Stroke;
            strokeTime=sample.time;
            preview->bytes=result.changed ? overhead+result.entryBytes : 0;
            collectPreviewDirty(); expectedRevision=revision();
            return {DocumentHistoryCode::None,expectedRevision!=before,preview->bytes};
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    template <class Function>
    DocumentHistoryResult strokeInput(double time,Function &&input,bool finish,bool validInput=true) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (!preview || previewKind!=PreviewKind::Stroke) return {DocumentHistoryCode::NoPreview};
        if (!validInput || !std::isfinite(time) || std::abs(time)>1e12 || time<strokeTime)
            return {DocumentHistoryCode::InvalidArgument};
        try {
            const auto before=revision();
            const bool accepted=withPreview(std::forward<Function>(input));
            const auto result=withPreview([](const auto &editor) { return editor.lastHistoryResult(); });
            if (!accepted) return {bitmapFailure(result.code,file)};
            strokeTime=time;
            preview->bytes=result.changed ? previewOverhead+result.entryBytes : 0;
            collectPreviewDirty(); expectedRevision=revision();
            if (finish) {
                previewOverhead=0; previewKind=PreviewKind::None;
                if (result.changed) { ++nextStrokeSeed; return publish(std::move(preview)); }
                preview.reset(); return {};
            }
            return {DocumentHistoryCode::None,expectedRevision!=before,preview->bytes};
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult sampleStroke(const StrokePoint &sample,bool finish) {
        return strokeInput(sample.time,[&](auto &editor) {
            return finish ? editor.endStrokeInput(sample) : editor.continueStrokeInput(sample);
        },finish,detail::validStrokeSample(sample));
    }
    DocumentHistoryResult clockStroke(double time,bool rebase) {
        return strokeInput(time,[&](auto &editor) {
            return rebase ? editor.rebaseStrokeTime(time) : editor.advanceStrokeTime(time);
        },false);
    }
    DocumentHistoryResult cancelStroke() {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        const auto state=current();
        if (state!=DocumentHistoryCode::None && state!=DocumentHistoryCode::Conflict) return {state};
        if (!preview || previewKind!=PreviewKind::Stroke) return {DocumentHistoryCode::NoPreview};
        // Shrinking the canvas after an outside revision could hide unrelated
        // content outside its old region. Preserve the complete live state.
        if (state==DocumentHistoryCode::Conflict && withPreview([](const auto &editor) {
                return detail::BitmapHistorySeed::growing(editor);
            })) return {DocumentHistoryCode::Conflict};
        try {
            const auto before=revision();
            const bool accepted=withPreview([](auto &editor) { editor.cancelStroke(); return !editor.strokeActive(); });
            const auto result=withPreview([](const auto &editor) { return editor.lastHistoryResult(); });
            if (!accepted) return {bitmapFailure(result.code,file)};
            collectPreviewDirty(); preview.reset(); previewOverhead=0; previewKind=PreviewKind::None;
            if (state==DocumentHistoryCode::None) expectedRevision=revision();
            return {DocumentHistoryCode::None,revision()!=before};
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult abandonStroke() {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (!preview || previewKind!=PreviewKind::Stroke) return {DocumentHistoryCode::NoPreview};
        if (preview->bytes) ++nextStrokeSeed;
        preview.reset(); previewOverhead=0; previewKind=PreviewKind::None;
        undo.clear(); redo.clear(); retained=0;
        if (bound()) expectedRevision=revision();
        return {};
    }
    DocumentHistoryResult step(bool forward) {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (const auto state=current(); state!=DocumentHistoryCode::None) return {state};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        auto &from=forward?redo:undo; auto &to=forward?undo:redo;
        if (from.empty()) return {forward?DocumentHistoryCode::NoRedo:DocumentHistoryCode::NoUndo};
        try {
            to.reserve(to.size()+1);
            Entry &entry=*from.back(); DocumentHistoryCode code=DocumentHistoryCode::None;
            if (auto *finite=std::get_if<std::unique_ptr<BitmapEditor>>(&entry.change)) {
                const bool ok=forward?(*finite)->redo():(*finite)->undo();
                if (!ok) code=bitmapFailure((*finite)->lastHistoryResult().code,file);
            } else if (auto *chunked=std::get_if<std::unique_ptr<ChunkedBitmapEditor>>(&entry.change)) {
                const bool ok=forward?(*chunked)->redo():(*chunked)->undo();
                if (!ok) code=bitmapFailure((*chunked)->lastHistoryResult().code,file);
            } else if (auto *layer=std::get_if<LayerChange>(&entry.change)) code=applyLayer(*layer,forward);
            else if (auto *structure=std::get_if<std::unique_ptr<LayerStructureChange>>(&entry.change))
                code=applyStructure(**structure,forward,false);
            else if (auto *assetLayer=std::get_if<std::unique_ptr<AssetLayerChange>>(&entry.change))
                code=applyAssetLayer(**assetLayer,forward==(*assetLayer)->layer.inserted,nullptr);
            else if (auto *timeline=std::get_if<TimelineChange>(&entry.change))
                code=applyTimeline(*timeline,forward,false);
            else if (auto *linked=std::get_if<std::unique_ptr<LinkedCanvasChange>>(&entry.change))
                code=applyLinkedCanvas(**linked,forward);
            else if (auto *relations=std::get_if<std::unique_ptr<RelationsChange>>(&entry.change))
                code=applyRelations(**relations,forward,false);
            else if (auto *procedural=std::get_if<std::unique_ptr<ProceduralContentChange>>(&entry.change))
                code=applyProcedural(**procedural,forward);
            else if (auto *batch=std::get_if<std::unique_ptr<LayerBatchChange>>(&entry.change))
                code=applyLayerBatch(**batch,forward);
            else if (auto *transaction=std::get_if<std::unique_ptr<LayerTransactionChange>>(&entry.change))
                code=applyLayerTransactionChange(**transaction,forward);
            else if (auto *vector=std::get_if<std::unique_ptr<VectorAssetChange>>(&entry.change))
                code=applyVectorAsset(**vector,forward);
            else if (auto *canvas=std::get_if<std::unique_ptr<CanvasGeometryChange>>(&entry.change))
                code=applyCanvasGeometryChange(**canvas,forward);
            else if (auto *aspect=std::get_if<PixelAspectChange>(&entry.change))
                code=applyPixelAspect(*aspect,forward);
            else if (auto *sequence=std::get_if<std::unique_ptr<SequenceProjectChange>>(&entry.change))
                code=applySequenceProject(**sequence,forward);
            else code=DocumentHistoryCode::EditRejected;
            if (code!=DocumentHistoryCode::None) return {code};
            const auto bytes=entry.bytes;
            to.push_back(std::move(from.back())); from.pop_back();
            expectedRevision=revision();
            return {DocumentHistoryCode::None,true,bytes};
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
    DocumentHistoryResult reset() {
        if (busy) return {DocumentHistoryCode::Reentrant};
        busy=true; struct Guard { bool &b; ~Guard(){b=false;} } guard{busy};
        if (!bound()) return {DocumentHistoryCode::NotBound};
        if (!policyValid()) return {DocumentHistoryCode::InvalidArgument};
        if (preview) return {DocumentHistoryCode::PreviewActive};
        try {
            if (!file) { DocumentEditor probe(*memory); if (!probe.isBound()) return {DocumentHistoryCode::InvalidArgument}; }
            undo.clear(); redo.clear(); retained=0; expectedRevision=revision(); return {};
        } catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
          catch (...) { return {DocumentHistoryCode::EditRejected}; }
    }
};

std::string_view DocumentHistoryResult::message() const noexcept {
    switch (code) {
    case DocumentHistoryCode::None: return "";
    case DocumentHistoryCode::NotBound: return "the document history is not bound";
    case DocumentHistoryCode::InvalidArgument: return "the edit or history policy is invalid";
    case DocumentHistoryCode::BudgetExceeded: return "the history budget would be exceeded";
    case DocumentHistoryCode::LimitExceeded: return "a resource limit was exceeded";
    case DocumentHistoryCode::Conflict: return "the document changed outside this history";
    case DocumentHistoryCode::EditRejected: return "the edit was rejected";
    case DocumentHistoryCode::PersistenceFailure: return "the working file could not be updated";
    case DocumentHistoryCode::NoUndo: return "there is no edit to undo";
    case DocumentHistoryCode::NoRedo: return "there is no edit to redo";
    case DocumentHistoryCode::Reentrant: return "a history operation is already active";
    case DocumentHistoryCode::PreviewActive: return "resolve the active preview or brush stroke first";
    case DocumentHistoryCode::NoPreview: return "there is no active session of that kind";
    }
    return "unknown history error";
}
DocumentHistory::DocumentHistory(Document &d,DocumentHistoryLimits l):m_impl(std::make_unique<Impl>(d,l)){}
DocumentHistory::DocumentHistory(DocumentFile &f,DocumentHistoryLimits l):m_impl(std::make_unique<Impl>(f,l)){}
DocumentHistory::~DocumentHistory()=default;
bool DocumentHistory::isBound() const noexcept { return m_impl->bound(); }
bool DocumentHistory::canUndo() const noexcept { return !m_impl->preview && m_impl->current()==DocumentHistoryCode::None && !m_impl->undo.empty(); }
bool DocumentHistory::canRedo() const noexcept { return !m_impl->preview && m_impl->current()==DocumentHistoryCode::None && !m_impl->redo.empty(); }
std::string_view DocumentHistory::undoLabel() const noexcept { return canUndo()?std::string_view(m_impl->undo.back()->label):std::string_view{}; }
std::string_view DocumentHistory::redoLabel() const noexcept { return canRedo()?std::string_view(m_impl->redo.back()->label):std::string_view{}; }
std::optional<std::uint64_t> DocumentHistory::undoEntryId() const noexcept { return m_impl->undo.empty()?std::nullopt:std::optional{m_impl->undo.back()->id}; }
std::optional<std::uint64_t> DocumentHistory::redoEntryId() const noexcept { return m_impl->redo.empty()?std::nullopt:std::optional{m_impl->redo.back()->id}; }
const Document *DocumentHistory::document() const noexcept { return m_impl->bound()?m_impl->document():nullptr; }
DocumentHistoryStatistics DocumentHistory::statistics() const noexcept { return {m_impl->undo.size(),m_impl->redo.size(),m_impl->retained}; }
const DocumentHistoryLimits &DocumentHistory::limits() const noexcept { return m_impl->limits; }
DocumentHistoryResult DocumentHistory::replaceBitmapPatches(std::string_view id,std::span<const BitmapPatch> patches,std::string_view label) { return m_impl->bitmap(id,patches,label); }
DocumentHistoryResult DocumentHistory::setSequenceProject(std::optional<TimelineProject> project,
    std::vector<TimelineOwnedMedia> media,std::string_view label) {
    return m_impl->sequenceProject(std::move(project),std::move(media),label);
}
DocumentHistoryResult DocumentHistory::beginPixelPreview(std::string_view id,std::string_view label) { return m_impl->beginPreview(id,label); }
DocumentHistoryResult DocumentHistory::beginStagedPixelPreview(std::string_view id,PixelPreviewCanvasMode mode,std::string_view label) {
    return m_impl->beginPreview(id,label,mode);
}
std::vector<BitmapPreviewTile> DocumentHistory::stagedPixelPreviewTiles() const {
    return pixelPreviewActive() ? m_impl->withPreview([](const auto &editor) {
        return detail::BitmapHistorySeed::stagedTiles(editor);
    }) : std::vector<BitmapPreviewTile>{};
}
std::optional<CanvasRegion> DocumentHistory::stagedPixelPreviewRegion() const noexcept {
    return pixelPreviewActive() ? m_impl->withPreview([](const auto &editor) {
        return detail::BitmapHistorySeed::stagedRegion(editor);
    }) : std::nullopt;
}
DocumentHistoryResult DocumentHistory::replacePixelPreview(std::span<const BitmapPatch> patches) { return m_impl->replacePreview(patches); }
DocumentHistoryResult DocumentHistory::commitPixelPreview() { return m_impl->finishPreview(true); }
DocumentHistoryResult DocumentHistory::cancelPixelPreview() { return m_impl->finishPreview(false); }
DocumentHistoryResult DocumentHistory::abandonPixelPreview() { return m_impl->abandonPreview(); }
bool DocumentHistory::pixelPreviewActive() const noexcept {
    return bool(m_impl->preview) && m_impl->previewKind==Impl::PreviewKind::Pixels;
}
std::optional<std::uint32_t> DocumentHistory::pixelPreviewSourceAt(int x,int y) const noexcept {
    if (!pixelPreviewActive() || m_impl->current()!=DocumentHistoryCode::None) return std::nullopt;
    return m_impl->withPreview([&](const auto &editor) { return editor.pixelPreviewSourceAt(x,y); });
}
std::size_t DocumentHistory::previewStorageBytes() const noexcept {
    return pixelPreviewActive() ? m_impl->withPreview([](const auto &editor) { return editor.previewStorageBytes(); }) : 0;
}
std::size_t DocumentHistory::previewPendingHistoryBytes() const noexcept { return pixelPreviewActive() ? m_impl->preview->bytes : 0; }
DevicePixelRect DocumentHistory::previewDirtyBounds() const noexcept { return m_impl->previewDirty; }
void DocumentHistory::clearPreviewDirtyBounds() noexcept { m_impl->previewDirty={}; }
DocumentHistoryResult DocumentHistory::beginStroke(std::string_view id,const BitmapBrush &brush,
                                                   const StrokePoint &sample,std::string_view label) {
    return m_impl->beginStroke(id,brush,sample,StrokeCanvasMode::Clip,label);
}
DocumentHistoryResult DocumentHistory::beginStroke(std::string_view id,const BitmapBrush &brush,
    const StrokePoint &sample,StrokeCanvasMode mode,std::string_view label) {
    return m_impl->beginStroke(id,brush,sample,mode,label);
}
DocumentHistoryResult DocumentHistory::continueStroke(const StrokePoint &sample) { return m_impl->sampleStroke(sample,false); }
DocumentHistoryResult DocumentHistory::endStroke(const StrokePoint &sample) { return m_impl->sampleStroke(sample,true); }
DocumentHistoryResult DocumentHistory::advanceStrokeTime(double time) { return m_impl->clockStroke(time,false); }
DocumentHistoryResult DocumentHistory::rebaseStrokeTime(double time) { return m_impl->clockStroke(time,true); }
DocumentHistoryResult DocumentHistory::cancelStroke() { return m_impl->cancelStroke(); }
DocumentHistoryResult DocumentHistory::abandonStroke() { return m_impl->abandonStroke(); }
bool DocumentHistory::strokeActive() const noexcept {
    return bool(m_impl->preview) && m_impl->previewKind==Impl::PreviewKind::Stroke;
}
std::size_t DocumentHistory::strokeStorageBytes() const noexcept {
    return strokeActive() ? m_impl->withPreview([](const auto &editor) { return editor.previewStorageBytes(); }) : 0;
}
std::size_t DocumentHistory::strokePendingHistoryBytes() const noexcept { return strokeActive() ? m_impl->preview->bytes : 0; }
DevicePixelRect DocumentHistory::strokeDirtyBounds() const noexcept { return m_impl->strokeDirty; }
void DocumentHistory::clearStrokeDirtyBounds() noexcept { m_impl->strokeDirty={}; }
DocumentHistoryResult DocumentHistory::setLayerName(std::string_view id,std::string_view name) {
    try { return m_impl->layer(Field::Name,id,std::string(name),"Rename layer"); }
    catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
}
DocumentHistoryResult DocumentHistory::setLayerVisible(std::string_view id,bool visible) { return m_impl->layer(Field::Visible,id,visible,"Layer visibility"); }
DocumentHistoryResult DocumentHistory::setLayerOpacity(std::string_view id,double opacity) { return m_impl->layer(Field::Opacity,id,opacity,"Layer opacity"); }
DocumentHistoryResult DocumentHistory::setLayerTransform(std::string_view id,AffineTransform transform) { return m_impl->layer(Field::Transform,id,transform,"Layer transform"); }
DocumentHistoryResult DocumentHistory::setLayerBlendMode(std::string_view id,RasterBlendMode mode) { return m_impl->layer(Field::Blend,id,mode,"Layer blend mode"); }
DocumentHistoryResult DocumentHistory::setLayerAppearance(std::string_view id,LayerAppearance appearance) {
    try { return m_impl->layer(Field::Appearance,id,std::make_shared<const LayerAppearance>(std::move(appearance)),"Layer appearance"); }
    catch (const std::bad_alloc &) { return {DocumentHistoryCode::LimitExceeded}; }
}
DocumentHistoryResult DocumentHistory::replaceProceduralContent(Asset content,std::string_view label) {
    return m_impl->procedural(std::move(content),label);
}
DocumentHistoryResult DocumentHistory::moveLayer(std::string_view id,std::size_t destination) { return m_impl->layer(Field::Order,id,destination,"Move layer"); }
DocumentHistoryResult DocumentHistory::applyLayerOrder(std::span<const std::string> order,std::string_view label) {
    return m_impl->layerOrder(order,label);
}
DocumentHistoryResult DocumentHistory::translateLayers(std::span<const LayerOffset> offsets,std::string_view label) {
    return m_impl->translate(offsets,label);
}
DocumentHistoryResult DocumentHistory::applyLayerTransaction(LayerTransactionPlan plan,std::string_view label) {
    return m_impl->layerTransaction(std::move(plan),label);
}
DocumentHistoryResult DocumentHistory::replaceLayerFrameRaster(std::string_view layer,FrameIndex frame,const RasterLayer &expected,RasterAsset content,
                                                               std::uint64_t revision,std::string_view label) {
    return m_impl->layerFrameRaster(layer,frame,expected,std::move(content),revision,label);
}
DocumentHistoryResult DocumentHistory::applyCanvasGeometry(const CanvasGeometryPlan &plan,std::string_view label) {
    return m_impl->canvasGeometry(plan,label);
}
DocumentHistoryResult DocumentHistory::applyCanvasAffine(const CanvasAffinePlan &plan,std::string_view label) {
    return m_impl->canvasAffine(plan,label);
}
DocumentHistoryResult DocumentHistory::setPixelAspectRatio(DocumentPixelAspectRatio ratio,std::string_view label) {
    return m_impl->pixelAspect(ratio,label);
}
DocumentHistoryResult DocumentHistory::replaceVectorPath(std::string_view id,std::size_t index,VectorPath path,std::string_view label) {
    return m_impl->vectorPath(id,index,std::move(path),label);
}
DocumentHistoryResult DocumentHistory::setTimeline(Timeline timeline,std::string_view label) {
    return m_impl->timeline(timeline,label);
}
DocumentHistoryResult DocumentHistory::setSelectionLayerRelations(SelectionLayerRelations relations,std::string_view label) {
    return m_impl->relations(std::move(relations),label);
}
DocumentHistoryResult DocumentHistory::insertLayer(Layer layer,std::size_t index,std::string_view label) {
    return m_impl->insertStructure(std::move(layer),{},index,label,false);
}
DocumentHistoryResult DocumentHistory::insertKeyframedLayer(Layer layer,std::vector<KeyframePlacement> keys,
                                                              std::size_t index,std::string_view label) {
    return m_impl->insertStructure(std::move(layer),std::move(keys),index,label,true);
}
DocumentHistoryResult DocumentHistory::removeLayer(std::string_view id,std::string_view label) {
    return m_impl->removeStructure(id,label);
}
DocumentHistoryResult DocumentHistory::insertLayerWithAsset(Asset asset,Layer layer,
    std::size_t layerIndex,std::size_t assetIndex,std::string_view label) {
    return m_impl->insertAssetLayer(std::move(asset),std::move(layer),layerIndex,assetIndex,label);
}
DocumentHistoryResult DocumentHistory::removeLayerWithAsset(std::string_view id,std::string_view label) {
    return m_impl->removeAssetLayer(id,label);
}
DocumentHistoryResult DocumentHistory::replaceLinkedCanvasAsset(LinkedCanvasAsset asset,std::string_view label) {
    return m_impl->linkedCanvas(std::move(asset),label);
}
DocumentHistoryResult DocumentHistory::undo(){return m_impl->step(false);}
DocumentHistoryResult DocumentHistory::redo(){return m_impl->step(true);}
DocumentHistoryResult DocumentHistory::reset(){return m_impl->reset();}
} // namespace iiSharedCanvas
