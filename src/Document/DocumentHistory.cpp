#include "Document/DocumentHistory.h"
#include "Document/AssetHistorySnapshot_p.hpp"
#include "Document/AssetLayerGuard_p.hpp"
#include "Document/TimelineGuard_p.hpp"
#include "Bitmap/BitmapEditor.h"
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
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
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
enum class Field { Name, Visible, Opacity, Transform, Blend, Order };
using Value = std::variant<std::string, bool, double, AffineTransform, RasterBlendMode, std::size_t>;
struct LayerChange {
    std::string id;
    Field field;
    Value before, after;
};
struct LayerStructureChange {
    Layer layer;
    std::vector<KeyframePlacement> keys;
    std::optional<std::string> previous, next;
    std::size_t index = 0;
    bool inserted = false;
};
struct AssetLayerChange {
    LayerStructureChange layer;
    detail::AssetHistorySnapshot asset;
    std::optional<std::string> previousAsset, nextAsset;
    std::size_t assetIndex=0;
};
struct TimelineChange { Timeline before, after; };
struct Entry {
    std::string label;
    std::size_t bytes = 0;
    std::variant<std::monostate, std::unique_ptr<BitmapEditor>,
                 std::unique_ptr<ChunkedBitmapEditor>, LayerChange,
                 std::unique_ptr<LayerStructureChange>,std::unique_ptr<AssetLayerChange>,
                 TimelineChange> change;
};
bool addSize(std::size_t &total, std::size_t value) noexcept {
    if (value > std::numeric_limits<std::size_t>::max() - total) return false;
    total += value; return true;
}
bool addElements(std::size_t &total,std::size_t count,std::size_t width) noexcept {
    return !width || (count<=std::numeric_limits<std::size_t>::max()/width && addSize(total,count*width));
}
bool bitmapInputSize(const Asset &asset,std::size_t &bytes) noexcept {
    const auto addPixels=[&](const RasterLayer &pixels) {
        return pixels.pixels.size()<=std::numeric_limits<std::size_t>::max()/sizeof(std::uint32_t) &&
               addSize(bytes,pixels.pixels.size()*sizeof(std::uint32_t));
    };
    if (const auto *raster=std::get_if<RasterAsset>(&asset)) return addPixels(raster->pixels);
    if (const auto *chunks=std::get_if<ChunkedRasterAsset>(&asset))
        for (const auto &chunk:chunks->chunks) if (!addPixels(chunk.pixels)) return false;
    return true;
}
bool sameTimeline(const Timeline &a,const Timeline &b) noexcept {
    const auto &[rateA,countA]=a; const auto &[rateB,countB]=b;
    const auto &[numeratorA,denominatorA]=rateA; const auto &[numeratorB,denominatorB]=rateB;
    return numeratorA==numeratorB && denominatorA==denominatorB && countA==countB;
}
bool sameTransform(const AffineTransform &a, const AffineTransform &b) noexcept {
    return a.m11==b.m11 && a.m12==b.m12 && a.m21==b.m21 && a.m22==b.m22 &&
           a.translationX==b.translationX && a.translationY==b.translationY;
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
    switch (field) {
    case Field::Name: return std::get<std::string>(a)==std::get<std::string>(b);
    case Field::Visible: return std::get<bool>(a)==std::get<bool>(b);
    case Field::Opacity: return std::get<double>(a)==std::get<double>(b);
    case Field::Transform: return sameTransform(std::get<AffineTransform>(a),std::get<AffineTransform>(b));
    case Field::Blend: return std::get<RasterBlendMode>(a)==std::get<RasterBlendMode>(b);
    case Field::Order: return std::get<std::size_t>(a)==std::get<std::size_t>(b);
    }
    return false;
}
Value currentValue(Field field, const Document &document, const Layer &layer) {
    const auto &p = layerProperties(layer);
    switch (field) {
    case Field::Name: return p.name;
    case Field::Visible: return p.visible;
    case Field::Opacity: return p.opacity;
    case Field::Transform: return p.transform;
    case Field::Blend: return p.blendMode;
    case Field::Order: return *layerIndex(document,p.id);
    }
    return false;
}
DocumentEditResult editValue(DocumentEditor &editor, const LayerChange &change, const Value &value) {
    switch (change.field) {
    case Field::Name: return editor.setLayerName(change.id,std::get<std::string>(value));
    case Field::Visible: return editor.setLayerVisible(change.id,std::get<bool>(value));
    case Field::Opacity: return editor.setLayerOpacity(change.id,std::get<double>(value));
    case Field::Transform: return editor.setLayerTransform(change.id,std::get<AffineTransform>(value));
    case Field::Blend: return editor.setLayerBlendMode(change.id,std::get<RasterBlendMode>(value));
    case Field::Order: return editor.moveLayer(change.id,std::get<std::size_t>(value));
    }
    return {DocumentEditCode::InvalidArgument,false,{},"unknown layer field"};
}

// In-memory DocumentEditor operations can allocate while updating authorship.
// Work on copies of only the affected structural values and restore the exact
// original buffers on rejection or exception, without copying raster assets.
struct PropertiesGuard {
    Document &document; LayerProperties &target; LayerProperties saved;
    DocumentAuthorship author; FormatVersion version; bool published = false;
    PropertiesGuard(Document &d, LayerProperties &p)
        : document(d),target(p),saved(p),author(d.authorship),version(d.formatVersion) {
        using std::swap; swap(target,saved); swap(document.authorship,author);
    }
    ~PropertiesGuard() noexcept {
        if (!published) { using std::swap; swap(target,saved); swap(document.authorship,author); document.formatVersion=version; }
    }
};
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
struct StructureGuard {
    Document &document; std::vector<Layer> layers; std::optional<std::vector<Frame>> frames;
    DocumentAuthorship author; FormatVersion version; bool published = false;
    StructureGuard(Document &d,bool changesFrames)
        : document(d),layers(d.layers),author(d.authorship),version(d.formatVersion) {
        if (changesFrames) frames.emplace(d.frames);
        using std::swap; swap(document.layers,layers);
        if (frames) swap(document.frames,*frames);
        swap(document.authorship,author);
    }
    ~StructureGuard() noexcept {
        if (!published) {
            using std::swap; swap(document.layers,layers);
            if (frames) swap(document.frames,*frames);
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
        if (!limits.maximumEntries || bytes>limits.maximumBytes) return false;
        if (limits.overflow==DocumentHistoryOverflow::EvictOldest) return true;
        const auto used=undoBytes();
        return undo.size()<limits.maximumEntries && used<=limits.maximumBytes && bytes<=limits.maximumBytes-used;
    }
    DocumentHistoryResult publish(std::unique_ptr<Entry> entry) noexcept {
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
        auto *editable=findLayer(*memory,change.id);
        PropertiesGuard guard(*memory,layerProperties(*editable));
        DocumentEditor editor(*memory);
        const auto result=editValue(editor,change,next);
        if (!result.ok()) return layerFailure(result,nullptr);
        if (!result.changed) return DocumentHistoryCode::Conflict;
        guard.published=true; return DocumentHistoryCode::None;
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
    DocumentHistoryCode applyStructure(const LayerStructureChange &change,bool forward,bool initial) {
        const auto *d=document();
        if (!d) return DocumentHistoryCode::NotBound;
        const auto &id=layerProperties(change.layer).id;
        const bool inserting=forward==change.inserted;
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
        StructureGuard guard(*memory,keyframedLayerSource(change.layer)!=nullptr);
        DocumentEditor editor(*memory);
        const auto result=perform(editor);
        if (!result.ok()) {
            if (!initial && result.code!=DocumentEditCode::NotBound) return DocumentHistoryCode::Conflict;
            return layerFailure(result,nullptr);
        }
        if (!result.changed) return DocumentHistoryCode::Conflict;
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
            std::size_t bytes=sizeof(Entry);
            const std::vector<KeyframePlacement> none;
            if (!addSize(bytes,sizeof(LayerStructureChange)) || !addSize(bytes,label.size()) ||
                (*index && !addSize(bytes,layerProperties(d->layers[*index-1]).id.size())) ||
                (*index+1<d->layers.size() && !addSize(bytes,layerProperties(d->layers[*index+1]).id.size())) ||
                !layerStructureSize(bytes,layer,none) || !canAdmit(bytes))
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
            entry->change.emplace<std::unique_ptr<LayerStructureChange>>(std::make_unique<LayerStructureChange>(
                LayerStructureChange{layer,std::move(keys),std::move(previous),std::move(next),*index,false}));
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
        const auto *existingAsset=findAsset(*d,id);
        const auto *existingLayer=findLayer(*d,layerId);
        if (inserting) {
            if (existingAsset || findAudioAsset(*d,id) || existingLayer ||
                !sameAssetNeighbours(*d,change,false) || !sameNeighbours(*d,change.layer,false) ||
                !sameKeys(*d,layerId,{})) return DocumentHistoryCode::Conflict;
        } else {
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
        detail::AssetLayerGuard guard(*memory);
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
            if (asset.valueless_by_exception() || layer.valueless_by_exception()) return {DocumentHistoryCode::InvalidArgument};
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
            if (!source) return {DocumentHistoryCode::InvalidArgument};
            const auto position=assetIndex(*d,source->assetId);
            if (!position) return {DocumentHistoryCode::InvalidArgument};
            const auto &asset=d->assets[*position];
            for (const auto &reference:assetReferences(*d,source->assetId))
                if (reference.layerIndex!=*layerPosition) return {DocumentHistoryCode::Conflict};
            std::size_t inputBytes=0;
            if (!bitmapInputSize(asset,inputBytes) || inputBytes>limits.maximumBitmapInputBytes)
                return {DocumentHistoryCode::LimitExceeded};
            std::size_t bytes=sizeof(Entry)+sizeof(AssetLayerChange)-sizeof(detail::AssetHistorySnapshot);
            const std::vector<KeyframePlacement> noKeys;
            if (!addSize(bytes,label.size()) || !layerStructureSize(bytes,layer,noKeys) ||
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
            entry->change.emplace<std::unique_ptr<AssetLayerChange>>(std::make_unique<AssetLayerChange>(
                AssetLayerChange{std::move(structure),std::move(snapshot),std::move(assetBefore),std::move(assetAfter),*position}));
            const auto code=applyAssetLayer(*std::get<std::unique_ptr<AssetLayerChange>>(entry->change),false,nullptr,true);
            if (code!=DocumentHistoryCode::None) return {code};
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
DocumentHistoryStatistics DocumentHistory::statistics() const noexcept { return {m_impl->undo.size(),m_impl->redo.size(),m_impl->retained}; }
const DocumentHistoryLimits &DocumentHistory::limits() const noexcept { return m_impl->limits; }
DocumentHistoryResult DocumentHistory::replaceBitmapPatches(std::string_view id,std::span<const BitmapPatch> patches,std::string_view label) { return m_impl->bitmap(id,patches,label); }
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
DocumentHistoryResult DocumentHistory::moveLayer(std::string_view id,std::size_t destination) { return m_impl->layer(Field::Order,id,destination,"Move layer"); }
DocumentHistoryResult DocumentHistory::setTimeline(Timeline timeline,std::string_view label) {
    return m_impl->timeline(timeline,label);
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
DocumentHistoryResult DocumentHistory::undo(){return m_impl->step(false);}
DocumentHistoryResult DocumentHistory::redo(){return m_impl->step(true);}
DocumentHistoryResult DocumentHistory::reset(){return m_impl->reset();}
} // namespace iiSharedCanvas
