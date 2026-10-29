#include <Document/DocumentEditor.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderNative.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LAYER_HISTORY_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(LAYER_HISTORY_ASAN)
#define LAYER_HISTORY_ASAN 1
#endif
#ifndef LAYER_HISTORY_ASAN
#define LAYER_HISTORY_ASAN 0
#endif
namespace allocation {
thread_local bool active=false;
thread_local std::size_t largest=0,total=0;
}
#if !LAYER_HISTORY_ASAN
void *operator new(std::size_t size) {
    if (allocation::active) { allocation::largest=std::max(allocation::largest,size); allocation::total+=size; }
    if (auto *p=std::malloc(size?size:1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }
void operator delete[](void *p,std::size_t) noexcept { std::free(p); }
#endif

using namespace iiSharedCanvas;
namespace {
int failures=0;
void expect(bool value,std::string_view message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
std::uint64_t checksum(const RasterLayer &r) {
    std::uint64_t hash=1469598103934665603ULL;
    for (auto p:r.pixels) { hash^=p; hash*=1099511628211ULL; }
    return hash;
}
Document fixture(int side,bool dense=false) {
    Document d; d.extent={side,side}; d.timeline={{24,1},8};
    std::uint32_t random=17;
    for (const auto *id:{"paint","mask"}) {
        auto pixels=makeRasterLayer(side,side);
        if (dense) for (auto &p:pixels.pixels) {
            random^=random<<13; random^=random>>17; random^=random<<5; p=random;
        }
        d.assets.emplace_back(RasterAsset{id,std::move(pixels)});
    }
    d.assets.emplace_back(VectorAsset{"vector",{side,side},{}});
    d.layers.emplace_back(StaticBitmapLayer{{"base","Base"},StaticSource{"paint"}});
    d.layers.emplace_back(DynamicBitmapLayer{{"anim","Animation"},KeyframedSource{{0,3}}});
    d.layers.emplace_back(StaticVectorLayer{{"vector-layer","Vector"},StaticSource{"vector"}});
    d.frames={{0,{{"anim","paint"}}},{3,{{"anim","mask"}}}};
    auto &p=layerProperties(d.layers[1]);
    p.opacity=0.75; p.appearance.rasterMask=LayerMask{.assetId="mask"};
    LayerFilter filter; filter.name="Invert"; filter.operation=RasterEffect{RasterEffectKind::Invert};
    p.appearance.filters.push_back(filter);
    MotionKeyframe motion; motion.frame=3; motion.value.position={2,4}; p.motion.push_back(motion);
    return d;
}
bool keys(const Document &d,std::string_view layer,std::string_view first,std::string_view second,FrameIndex time) {
    const auto *a=findKeyframe(d,std::string(layer),0), *b=findKeyframe(d,std::string(layer),time);
    return a && b && a->assetId==first && b->assetId==second;
}
void workflow(DocumentHistory &h,const Document &d) {
    const auto *paint=findRasterAsset(d,"paint"), *mask=findRasterAsset(d,"mask");
    if (!paint || !mask) { expect(false,"workflow: initial raster assets"); return; }
    const auto *paintData=paint->pixels.pixels.data(); const auto *maskData=mask->pixels.pixels.data();
    const auto originalPixel=paint->pixels.pixels[std::size_t(paint->pixels.width)+1];
    const std::array<std::uint32_t,1> value{originalPixel^0x00ffffffU};
    const std::array patch{BitmapPatch{{{1,1},1,1},value}};
    expect(h.replaceBitmapPatches("paint",patch,"Paint").changed,"workflow: paint");
    Layer extra=StaticVectorLayer{{"extra","Extra"},StaticSource{"vector"}};
    const auto inserted=h.insertLayer(std::move(extra),1,"Add vector");
    expect(inserted.changed && inserted.entryBytes>0 && h.undoLabel()=="Add vector",
           "workflow: insertion enters shared history");
    Layer animated=DynamicBitmapLayer{{"ghost","Ghost"},KeyframedSource{}};
    expect(h.insertKeyframedLayer(std::move(animated),{{4,"mask"},{0,"paint"}},2,"Add animation").changed &&
           keys(d,"ghost","paint","mask",4),"workflow: unsorted keys are inserted canonically");
    expect(h.removeLayer("anim","Remove animation").changed && !findLayer(d,"anim") &&
           !findKeyframe(d,"anim",0) && findRasterAsset(d,"mask"),
           "workflow: removal keeps assets and removes only owned keys");
    expect(h.setLayerName("base","Ink").changed && h.moveLayer("vector-layer",0).changed &&
           h.statistics().undoEntries==6,"workflow: pixel, structure and properties share order");
    expect(h.undo().changed && layerIndex(d,"vector-layer")==3,"workflow: undo order");
    expect(h.undo().changed && layerProperties(*findLayer(d,"base")).name=="Base","workflow: undo name");
    expect(h.undo().changed && layerIndex(d,"anim")==3 && keys(d,"anim","paint","mask",3),
           "workflow: undo removal restores keys and index");
    if (const auto *restored=findLayer(d,"anim")) {
        const auto &p=layerProperties(*restored);
        expect(p.opacity==0.75 && p.appearance.rasterMask && p.appearance.rasterMask->assetId=="mask" &&
               p.appearance.filters.size()==1 && p.motion.size()==1,
               "workflow: undo removal restores full layer appearance and motion");
    } else expect(false,"workflow: restored animated layer exists");
    expect(h.undo().changed && !findLayer(d,"ghost") && !findKeyframe(d,"ghost",0),
           "workflow: undo animated insertion");
    expect(h.undo().changed && !findLayer(d,"extra"),"workflow: undo static insertion");
    expect(h.undo().changed && findRasterAsset(d,"paint")->pixels.pixels[std::size_t(paint->pixels.width)+1]==originalPixel &&
           h.statistics().redoEntries==6,"workflow: undo paint restores the original and all redo entries");
    for (int i=0;i<6;++i) expect(h.redo().changed,"workflow: redo command");
    expect(layerIndex(d,"vector-layer")==0 && !findLayer(d,"anim") &&
           keys(d,"ghost","paint","mask",4) && h.statistics().undoEntries==6,
           "workflow: redo restores structure and keys");
    expect(findRasterAsset(d,"paint")->pixels.pixels.data()==paintData &&
           findRasterAsset(d,"mask")->pixels.pixels.data()==maskData,
           "workflow: structure history does not replace asset buffers");
}
void memory() {
    auto d=fixture(32); DocumentHistory h(d); workflow(h,d);
}
void conflicts() {
    {
        auto d=fixture(32); DocumentHistory h(d);
        expect(h.insertLayer(StaticBitmapLayer{{"extra","Extra"},StaticSource{"paint"}},1).changed,
               "conflict layer: insert");
        auto *p=findLayer(d,"extra"); layerProperties(*p).name="External";
        const auto stats=h.statistics();
        expect(h.undo().code==DocumentHistoryCode::Conflict && layerProperties(*findLayer(d,"extra")).name=="External" &&
               h.statistics().undoEntries==stats.undoEntries,"conflict layer: direct edit is not overwritten");
    }
    {
        auto d=fixture(32); DocumentHistory h(d);
        expect(h.insertKeyframedLayer(DynamicBitmapLayer{{"new","New"},KeyframedSource{}},
                                      {{0,"paint"},{4,"mask"}},1).changed,"conflict keys: insert");
        auto *key=findKeyframe(d,"new",4); key->assetId="paint";
        expect(h.undo().code==DocumentHistoryCode::Conflict && findKeyframe(d,"new",4)->assetId=="paint" &&
               h.statistics().undoEntries==1,"conflict keys: direct key edit is not overwritten");
    }
    {
        auto d=fixture(32); DocumentHistory h(d);
        expect(h.removeLayer("anim").changed,"conflict neighbours: remove");
        std::swap(d.layers[0],d.layers[1]);
        expect(h.undo().code==DocumentHistoryCode::Conflict && !findLayer(d,"anim") &&
               h.statistics().undoEntries==1,"conflict neighbours: reorder is not overwritten");
    }
    {
        auto d=fixture(32); DocumentHistory h(d);
        expect(h.insertLayer(StaticBitmapLayer{{"extra","Extra"},StaticSource{"paint"}},1).changed,
               "conflict index: insert");
        std::swap(d.layers[1],d.layers[3]); // Both original neighbours remain at the expected gap.
        expect(h.undo().code==DocumentHistoryCode::Conflict && layerIndex(d,"extra")==3 &&
               h.statistics().undoEntries==1,"conflict index: moved target is not removed elsewhere");
    }
}
void limits() {
    auto d=fixture(32); DocumentHistoryLimits tiny; tiny.maximumBytes=1;
    DocumentHistory limited(d,tiny); const auto revision=d.authorship.revision();
    expect(limited.insertLayer(StaticBitmapLayer{{"extra","Extra"},StaticSource{"paint"}},1).code==DocumentHistoryCode::BudgetExceeded &&
           limited.removeLayer("anim").code==DocumentHistoryCode::BudgetExceeded &&
           d.authorship.revision()==revision && findLayer(d,"anim") && !findLayer(d,"extra"),
           "limits: insertion and removal reject before mutation");
    auto e=fixture(32); DocumentHistoryLimits cap; cap.maximumEntries=1;
    cap.overflow=DocumentHistoryOverflow::EvictOldest; DocumentHistory evict(e,cap);
    expect(evict.insertLayer(StaticBitmapLayer{{"extra","Extra"},StaticSource{"paint"}},1).changed,
           "limits: first structural entry");
    const auto removed=evict.removeLayer("anim");
    expect(removed.changed && removed.evictedEntries==1 && evict.undo().changed && findLayer(e,"anim") &&
           !evict.canUndo() && findLayer(e,"extra"),"limits: explicit eviction reports discarded command");
    auto b=fixture(32); DocumentHistory branch(b);
    expect(branch.insertLayer(StaticBitmapLayer{{"extra","Extra"},StaticSource{"paint"}},1).changed &&
           branch.undo().changed,"limits: create redo branch");
    const auto saved=branch.statistics();
    expect(!branch.insertLayer(StaticBitmapLayer{{"base","Duplicate"},StaticSource{"paint"}},0).ok() &&
           !branch.insertKeyframedLayer(DynamicBitmapLayer{{"bad","Bad"},KeyframedSource{}},
                                        {{0,"paint"},{0,"mask"}},0).ok() &&
           branch.statistics().redoEntries==saved.redoEntries && branch.canRedo(),
           "limits: invalid structural edits preserve redo");
    expect(branch.beginPixelPreview("paint").ok() &&
           branch.removeLayer("anim").code==DocumentHistoryCode::PreviewActive &&
           branch.cancelPixelPreview().ok(),"limits: active preview blocks structure edits");
}
void typedLayers() {
    {
        auto d=fixture(32);
        d.assets.emplace_back(VideoAsset{"movie",{24,1},{makeRasterLayer(1,1,0xff000000)}});
        DocumentHistory h(d); VideoLayer video{{"video","Video"},StaticSource{"movie"}};
        expect(h.insertLayer(video).changed && h.undo().changed && h.redo().changed,
               "types: video insertion round-trips");
        auto *layer=findVideoLayer(d,"video");
        if (!layer) { expect(false,"types: video layer exists"); return; }
        layer->playback.endBehavior=VideoEndBehavior::Hold;
        expect(h.undo().code==DocumentHistoryCode::Conflict && findVideoLayer(d,"video"),
               "types: changed video playback is not overwritten");
    }
    {
        auto d=fixture(32);
        d.assets.emplace_back(ReferenceAsset{"ref",{1,1},{{1,2,3}}});
        DocumentHistory h(d); ReferenceLayer reference;
        reference.properties={"reference","Reference"}; reference.source=StaticSource{"ref"};
        reference.control.modelId="adapter"; reference.reference.styleFidelity=0.4;
        expect(h.insertLayer(reference).changed && h.undo().changed && h.redo().changed,
               "types: reference insertion round-trips");
        auto *layer=findReferenceLayer(d,"reference");
        if (!layer) { expect(false,"types: reference layer exists"); return; }
        layer->reference.styleFidelity=0.8;
        expect(h.undo().code==DocumentHistoryCode::Conflict && findReferenceLayer(d,"reference"),
               "types: changed reference settings are not overwritten");
    }
    {
        auto d=fixture(32);
        d.assets.emplace_back(RasterAsset{"semantic-mask",makeRasterLayer(1,1,0xff000000)});
        auto other=d;
        SemanticSegmentLayer semantic;
        semantic.properties={"semantic","Semantic"}; semantic.source=StaticSource{"semantic-mask"};
        semantic.control.modelId="semantic-model";
        semantic.segmentation.taxonomy.id="taxonomy";
        SemanticClass person; person.id=1; person.key="person"; person.name="Person";
        person.description=std::string(5000,'x'); person.aliases={"human","pedestrian"};
        semantic.segmentation.taxonomy.classes.push_back(person);
        DocumentHistory h(d); const auto result=h.insertLayer(semantic);
        expect(result.changed && result.entryBytes>person.description.size() && h.undo().changed && h.redo().changed,
               "types: semantic content is accounted and round-trips");
        if (result.entryBytes>0) {
            DocumentHistoryLimits small; small.maximumBytes=result.entryBytes-1;
            DocumentHistory limited(other,small);
            expect(limited.insertLayer(semantic).code==DocumentHistoryCode::BudgetExceeded &&
                   !findLayer(other,"semantic"),"types: nested semantic metadata counts against budget");
        }
        auto *layer=findSemanticSegmentLayer(d,"semantic");
        if (!layer) { expect(false,"types: semantic layer exists"); return; }
        layer->segmentation.taxonomy.classes[0].description="External";
        expect(h.undo().code==DocumentHistoryCode::Conflict && findSemanticSegmentLayer(d,"semantic"),
               "types: changed semantic taxonomy is not overwritten");
    }
}
void fileFailures() {
    namespace sql=iiFileProvider::native::sql;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"file failure: temporary directory"); return; }
    const auto path=(temporary.path()/"failures.iisc").string();
    auto d=fixture(32); DocumentFile file;
    if (!file.create(path,d).ok()) { expect(false,"file failure: create"); return; }
    DocumentHistory h(file);
    expect(h.insertLayer(StaticBitmapLayer{{"extra","Extra"},StaticSource{"paint"}},1).changed,
           "file failure: initial insert");
    auto connection=sql::Database::open(std::filesystem::u8path(path));
    if (!connection) { expect(false,"file failure: second database connection"); return; }
    const auto stable=encodeIisc(*file.document()).bytes; const auto revision=file.revision();
    {
        auto lock=sql::Transaction::begin(*connection,true);
        if (!lock) { expect(false,"file failure: acquire undo lock"); return; }
        expect(h.undo().code==DocumentHistoryCode::Conflict && h.canUndo() &&
               file.isOpen() && file.revision()==revision && encodeIisc(*file.document()).bytes==stable,
               "file failure: locked undo preserves layer and history cursor");
        expect(lock->rollback().has_value(),"file failure: release undo lock");
    }
    Document snapshot;
    expect(DocumentFile::readSnapshot(path,snapshot).ok() && encodeIisc(snapshot).bytes==stable,
           "file failure: locked undo preserves durable state");
    expect(h.undo().changed && h.canRedo() && !findLayer(*file.document(),"extra"),
           "file failure: undo succeeds after lock release");
    const auto undone=encodeIisc(*file.document()).bytes; const auto undoneRevision=file.revision();
    {
        auto lock=sql::Transaction::begin(*connection,true);
        if (!lock) { expect(false,"file failure: acquire redo lock"); return; }
        expect(h.redo().code==DocumentHistoryCode::Conflict && h.canRedo() &&
               file.isOpen() && file.revision()==undoneRevision && encodeIisc(*file.document()).bytes==undone,
               "file failure: locked redo preserves document and cursor");
        expect(lock->rollback().has_value(),"file failure: release redo lock");
    }
    expect(h.redo().changed && findLayer(*file.document(),"extra"),
           "file failure: redo succeeds after lock release");
    DocumentFile stale;
    if (!stale.open(path).ok()) { expect(false,"file failure: open stale binding"); return; }
    DocumentHistory staleHistory(stale);
    expect(h.setLayerName("base","Changed").changed,"file failure: first writer commits");
    const auto staleResult=staleHistory.removeLayer("anim");
    expect(staleResult.code==DocumentHistoryCode::Conflict && !staleResult.changed &&
           staleHistory.statistics().undoEntries==0,"file failure: stale removal preserves history");
    expect(DocumentFile::readSnapshot(path,snapshot).ok() && findLayer(snapshot,"anim") &&
           layerProperties(*findLayer(snapshot,"base")).name=="Changed",
           "file failure: winning writer and animation remain durable");
}
void file(int side,bool dense) {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"file: temporary directory"); return; }
    const auto path=(temporary.path()/"layer-history.iisc").string();
    auto d=fixture(side,dense); const auto originalMask=checksum(findRasterAsset(d,"mask")->pixels);
    DocumentFile owner;
    if (!owner.create(path,d).ok()) { expect(false,"file: create"); return; }
    d={};
    const auto bytes=std::filesystem::file_size(path);
    if (dense) expect(bytes>400ULL*1024*1024,"file: dense 8K assets are in the actual file");
    DocumentHistory h(owner);
    allocation::largest=allocation::total=0; allocation::active=dense;
    workflow(h,*owner.document());
    allocation::active=false;
    const auto retained=h.statistics().retainedBytes;
    if (dense) expect(retained<1024*1024,"file: six history commands retain bounded structural and pixel deltas");
#if !LAYER_HISTORY_ASAN
    if (dense) expect(allocation::largest<4*1024*1024 && allocation::total<32*1024*1024,
                      "file: structural history does not allocate complete 8K assets");
#endif
    const auto revision=owner.revision();
    const auto finalPaint=checksum(findRasterAsset(*owner.document(),"paint")->pixels);
    expect(checksum(findRasterAsset(*owner.document(),"mask")->pixels)==originalMask,
           "file: mask pixels survive structure and history");
    owner.close();
    DocumentFile reopened;
    if (!reopened.open(path).ok()) { expect(false,"file: reopen"); return; }
    expect(reopened.revision()==revision && layerIndex(*reopened.document(),"vector-layer")==0 &&
           !findLayer(*reopened.document(),"anim") && keys(*reopened.document(),"ghost","paint","mask",4) &&
           checksum(findRasterAsset(*reopened.document(),"paint")->pixels)==finalPaint &&
           checksum(findRasterAsset(*reopened.document(),"mask")->pixels)==originalMask,
           "file: reopened structure, keys and both full raster checksums match");
    std::cout << "LAYER_HISTORY_FILE side=" << side << " dense=" << dense << " file_bytes=" << bytes
              << " checked_pixels=" << 2ULL*side*side << " max_cpp=" << allocation::largest
              << " total_cpp=" << allocation::total << " retained_bytes=" << retained
              << " reopened=" << reopened.isOpen() << '\n';
}
}
int main(int argc,char **argv) {
    std::cout << std::unitbuf;
    if (argc>1 && std::string_view(argv[1])=="--large") file(8192,true);
    else { memory(); conflicts(); limits(); typedLayers(); file(32,false); fileFailures(); }
    return failures?1:0;
}
