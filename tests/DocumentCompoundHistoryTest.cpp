#include <Bitmap/BitmapEditor.h>
#include <Document/DocumentEditor.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderNative.h>
#include "LegacyWorkingFileFixture_p.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <string_view>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define COMPOUND_HISTORY_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(COMPOUND_HISTORY_ASAN)
#define COMPOUND_HISTORY_ASAN 1
#endif
#ifndef COMPOUND_HISTORY_ASAN
#define COMPOUND_HISTORY_ASAN 0
#endif
namespace allocations {
thread_local bool active=false;
thread_local std::size_t maximum=0,total=0;
}
#if !COMPOUND_HISTORY_ASAN
void *operator new(std::size_t n) {
    if (allocations::active) { allocations::maximum=std::max(allocations::maximum,n); allocations::total+=n; }
    if (auto *p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
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
Document fixture() {
    Document d; d.extent={32,32}; d.timeline={{24,1},12};
    auto back=makeRasterLayer(32,32,0xff012345);
    auto removed=makeRasterLayer(32,32,0); removed.pixels[5]=0xffabcdef;
    d.assets.emplace_back(RasterAsset{"back",std::move(back)});
    d.assets.emplace_back(RasterAsset{"removed",std::move(removed)});
    d.layers.emplace_back(StaticBitmapLayer{{"back-layer","Back"},StaticSource{"back"}});
    d.layers.emplace_back(StaticBitmapLayer{{"removed-layer","Ink"},StaticSource{"removed"}});
    layerProperties(d.layers.back()).appearance.rasterMask=LayerMask{.assetId="back"};
    return d;
}
bool sameTimeline(const Timeline &a,const Timeline &b) {
    return a.frameRate.numerator==b.frameRate.numerator &&
           a.frameRate.denominator==b.frameRate.denominator && a.frameCount==b.frameCount;
}
StrokePoint sample(double x,double y,double time) {
    StrokePoint p; p.position={x,y}; p.time=time; return p;
}
BitmapBrush brush() { BitmapBrush b; b.size=5; b.argb=0xffeedd99; return b; }
void roundtrip(DocumentHistory &h,const Document &d) {
    const auto *back=findRasterAsset(d,"back"), *removed=findRasterAsset(d,"removed");
    if (!back || !removed) { expect(false,"roundtrip: initial assets"); return; }
    const auto beforeBack=back->pixels.pixels, beforeRemoved=removed->pixels.pixels;
    const auto *backPixels=back->pixels.pixels.data();
    const Timeline initial=d.timeline, next{{30000,1001},36};
    const auto revision=d.authorship.revision();
    auto result=h.setTimeline(next,"Document timing");
    expect(result.changed && result.entryBytes>0 && h.undoLabel()=="Document timing" &&
           sameTimeline(d.timeline,next) && d.authorship.revision()==revision+1 &&
           findRasterAsset(d,"back")->pixels.pixels.data()==backPixels,
           "roundtrip: timing is one revision without replacing raster buffers");
    const auto afterTiming=d.authorship.revision();
    result=h.removeLayerWithAsset("removed-layer","Delete layer");
    expect(result.changed && h.undoLabel()=="Delete layer" &&
           d.authorship.revision()==afterTiming+1 && !findLayer(d,"removed-layer") &&
           !findAsset(d,"removed") && findRasterAsset(d,"back")->pixels.pixels.data()==backPixels,
           "roundtrip: a single command removes the layer and its unshared asset");
    expect(h.beginStroke("back",brush(),sample(12,12,1)).changed &&
           h.endStroke(sample(18,18,2)).changed,"roundtrip: subsequent brush stroke");
    const auto painted=findRasterAsset(d,"back")->pixels.pixels;
    expect(painted!=beforeBack && h.undo().changed && findRasterAsset(d,"back")->pixels.pixels==beforeBack &&
           h.undo().changed && findRasterAsset(d,"removed") && findLayer(d,"removed-layer") &&
           findRasterAsset(d,"removed")->pixels.pixels==beforeRemoved &&
           layerProperties(*findLayer(d,"removed-layer")).appearance.rasterMask->assetId=="back" &&
           h.undo().changed && sameTimeline(d.timeline,initial),
           "roundtrip: mixed undo restores stroke, asset, layer, mask and timing in order");
    expect(h.redo().changed && sameTimeline(d.timeline,next) && h.redo().changed &&
           !findLayer(d,"removed-layer") && !findAsset(d,"removed") && h.redo().changed &&
           findRasterAsset(d,"back")->pixels.pixels==painted,
           "roundtrip: mixed redo restores the exact edited state");
}
void memory() {
    auto d=fixture(); DocumentHistory h(d); roundtrip(h,d);
    auto c=fixture(); DocumentHistory conflict(c);
    expect(conflict.setTimeline({{30,1},24}).changed,"timeline conflict: setup");
    c.timeline.frameRate.numerator=31;
    const auto before=encodeIisc(c).bytes;
    expect(conflict.undo().code==DocumentHistoryCode::Conflict && encodeIisc(c).bytes==before &&
           conflict.statistics().undoEntries==1,
           "timeline conflict: direct external timing mutation is never overwritten");
    {
        auto v=fixture(); VectorPath path;
        path.commands={MoveTo{{0,1}},LineTo{{2,3}},QuadraticTo{{4,5},{6,7}},
                       CubicTo{{8,9},{10,11},{12,13}},ClosePath{}};
        path.stroke=StrokeStyle{{0xff55aacc},2};
        v.assets.emplace_back(VectorAsset{"vector",{32,32},{path}});
        v.layers.emplace_back(StaticVectorLayer{{"vector-layer","Vector"},StaticSource{"vector"}});
        DocumentHistory history(v);
        expect(history.removeLayerWithAsset("vector-layer").changed && !findAsset(v,"vector") &&
               history.undo().changed && findVectorAsset(v,"vector") &&
               findVectorAsset(v,"vector")->paths[0].commands.size()==5 && history.redo().changed &&
               !findAsset(v,"vector"),"vector: deletion restores complete path data before redo");
        expect(history.undo().changed,"vector conflict: restore");
        auto *restored=findVectorAsset(v,"vector");
        if (!restored) { expect(false,"vector conflict: restored asset"); return; }
        std::get<MoveTo>(restored->paths[0].commands[0]).point.x=-0.0;
        expect(history.redo().code==DocumentHistoryCode::Conflict && history.canRedo(),
               "vector conflict: direct signed-zero changes cannot be discarded on redo");
    }
    {
        Document c; c.extent={128,128}; c.canvasMode=CanvasMode::Infinite;
        c.infiniteCanvas={{-64,-64},32}; auto pixels=makeRasterLayer(32,32); pixels.pixels[9]=0xff123456;
        c.assets.emplace_back(ChunkedRasterAsset{"chunks",{{-1,-1,std::move(pixels)}}});
        c.layers.emplace_back(StaticBitmapLayer{{"chunks-layer","Chunks"},StaticSource{"chunks"}});
        DocumentHistory history(c);
        expect(history.removeLayerWithAsset("chunks-layer").changed && !findAsset(c,"chunks") &&
               history.undo().changed && findChunkedRasterAsset(c,"chunks") &&
               findChunkedRasterAsset(c,"chunks")->chunks.size()==1 &&
               findChunkedRasterAsset(c,"chunks")->chunks[0].column==-1 &&
               findChunkedRasterAsset(c,"chunks")->chunks[0].row==-1 &&
               findChunkedRasterAsset(c,"chunks")->chunks[0].pixels.pixels[9]==0xff123456 &&
               history.redo().changed && !findAsset(c,"chunks"),
               "chunks: deletion restores negative chunk coordinates and exact pixels");
    }
}
void constraints() {
    {
        auto d=fixture(); DocumentHistory h(d);
        expect(h.setLayerVisible("back-layer",false).changed && h.undo().changed,"timeline: establish redo");
        const auto before=encodeIisc(d).bytes; const auto revision=d.authorship.revision();
        const auto stats=h.statistics();
        expect(!h.setTimeline({{0,1},12}).ok() && !h.setTimeline({{24,0},12}).ok() &&
               !h.setTimeline({{24,1},0}).ok() && h.setTimeline(d.timeline).ok() &&
               encodeIisc(d).bytes==before && d.authorship.revision()==revision && h.canRedo() &&
               h.statistics().retainedBytes==stats.retainedBytes,
               "timeline: invalid and no-op changes preserve bytes, revision and redo");
    }
    {
        auto d=fixture(); DocumentEditor editor(d);
        expect(editor.insertKeyframedLayer(DynamicBitmapLayer{{"dynamic","Animated"},KeyframedSource{}},
               {{0,"back"},{7,"back"}}).changed,"timeline: keyed fixture");
        DocumentHistory h(d); const auto before=encodeIisc(d).bytes;
        expect(!h.setTimeline({{24,1},7}).ok() && encodeIisc(d).bytes==before && !h.canUndo(),
               "timeline: shortening cannot silently discard a frame key");
        expect(h.setTimeline({{60,1},8}).changed && h.undo().changed && h.redo().changed,
               "timeline: valid boundary preserves keyframe data through undo and redo");
    }
    {
        auto d=fixture(); layerProperties(d.layers.front()).motion.push_back(MotionKeyframe{.frame=7});
        DocumentHistory h(d); const auto before=encodeIisc(d).bytes;
        expect(!h.setTimeline({{24,1},7}).ok() && encodeIisc(d).bytes==before && !h.canUndo(),
               "timeline: shortening cannot silently discard a motion key");
    }
    {
        auto d=fixture(); AudioAsset audio; audio.id="audio"; audio.sampleRate=24000;
        audio.channelCount=1; audio.samples.resize(12000);
        d.audioAssets.push_back(audio);
        AudioTrackLayer track; track.id="track"; track.name="Track";
        track.clips.push_back(AudioClip{.id="clip",.name="Clip",.assetId="audio",.durationFrames=12});
        d.audioTracks.push_back(std::move(track));
        DocumentHistory h(d); const auto before=encodeIisc(d).bytes;
        expect(!h.setTimeline({{12,1},12}).ok() && encodeIisc(d).bytes==before && !h.canUndo(),
               "timeline: slower frame rate cannot overrun existing audio samples");
        expect(h.setTimeline({{30,1},12}).changed && h.undo().changed && h.redo().changed &&
               d.audioAssets.front().samples==audio.samples && d.audioTracks.front().clips.size()==1,
               "timeline: valid rate changes preserve audio through undo and redo");
    }
    {
        auto d=fixture();
        d.layers.emplace_back(StaticBitmapLayer{{"shared","Shared"},StaticSource{"removed"}});
        DocumentHistory h(d); const auto before=encodeIisc(d).bytes;
        expect(h.removeLayerWithAsset("removed-layer").code==DocumentHistoryCode::Conflict &&
               encodeIisc(d).bytes==before && !h.canUndo(),
               "references: another layer's content prevents removing the shared asset");
        expect(h.removeLayer("removed-layer").changed && findAsset(d,"removed"),
               "references: callers can explicitly remove only the layer and retain a shared asset");
    }
    {
        auto d=fixture(); layerProperties(d.layers.front()).appearance.rasterMask=LayerMask{.assetId="removed"};
        DocumentHistory h(d); const auto before=encodeIisc(d).bytes;
        expect(h.removeLayerWithAsset("removed-layer").code==DocumentHistoryCode::Conflict &&
               encodeIisc(d).bytes==before,"references: another layer's mask prevents asset removal");
    }
    {
        auto d=fixture(); DocumentEditor editor(d);
        expect(editor.insertKeyframedLayer(DynamicBitmapLayer{{"dynamic","Animated"},KeyframedSource{}},
               {{0,"removed"}}).changed,"references: frame-key fixture");
        DocumentHistory h(d); const auto before=encodeIisc(d).bytes;
        expect(h.removeLayerWithAsset("removed-layer").code==DocumentHistoryCode::Conflict &&
               encodeIisc(d).bytes==before,"references: another layer's key prevents asset removal");
    }
    {
        auto d=fixture(); DocumentHistoryLimits limits; limits.maximumBytes=256;
        DocumentHistory h(d,limits);
        const auto before=encodeIisc(d).bytes;
        expect(h.removeLayerWithAsset("removed-layer").code==DocumentHistoryCode::BudgetExceeded &&
               encodeIisc(d).bytes==before && !h.canUndo(),
               "budget: asset deletion is rejected before mutation when its history cannot fit");
    }
    {
        auto d=fixture(); DocumentHistoryLimits limits; limits.maximumBitmapInputBytes=100;
        DocumentHistory h(d,limits); const auto before=encodeIisc(d).bytes;
        expect(h.removeLayerWithAsset("removed-layer").code==DocumentHistoryCode::LimitExceeded &&
               encodeIisc(d).bytes==before,"input: asset deletion obeys raw pixel input limit");
    }
    {
        auto d=fixture(); DocumentHistory h(d);
        expect(h.beginPixelPreview("back").ok() &&
               h.setTimeline({{30,1},12}).code==DocumentHistoryCode::PreviewActive &&
               h.removeLayerWithAsset("removed-layer").code==DocumentHistoryCode::PreviewActive &&
               h.cancelPixelPreview().ok(),"preview: both commands preserve an active session");
    }
    {
        auto d=fixture(); DocumentHistoryLimits limits; limits.maximumEntries=1;
        limits.overflow=DocumentHistoryOverflow::EvictOldest; DocumentHistory h(d,limits);
        expect(h.setLayerVisible("back-layer",false).changed,"eviction: establish entry");
        const auto changed=h.setTimeline({{30,1},24});
        expect(changed.changed && changed.evictedEntries==1 && h.undo().changed && !h.canUndo() &&
               sameTimeline(d.timeline,{{24,1},12}),"eviction: timeline reports the discarded entry");
    }
}
void file() {
    namespace sql=iiFileProvider::native::sql;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory tmp(parent);
    if (!tmp.isValid()) { expect(false,"file: temporary directory"); return; }
    const auto path=(tmp.path()/"compound.iisc").string(); auto d=fixture(); DocumentFile owner;
    if (!owner.create(path,d).ok()) { expect(false,"file: create"); return; }
    static_assert(DefaultNativeSnapshotTimeoutMs==300000);
    auto preserved=fixture(); const auto original=encodeIisc(preserved).bytes;
    for (const auto timeout:{0,-1}) {
        const auto failed=DocumentFile::readSnapshot(path,preserved,{},timeout);
        expect(failed.code==DocumentFileCode::InvalidArgument && !failed.changed &&
               encodeIisc(preserved).bytes==original,
               "snapshot: nonpositive backup deadline preserves the output");
    }
    Document customSnapshot;
    expect(DocumentFile::readSnapshot(path,customSnapshot,{},60000).ok() &&
           encodeIisc(customSnapshot).bytes==encodeIisc(*owner.document()).bytes,
           "snapshot: an explicit positive backup deadline reads the complete document");
    DocumentHistory h(owner); roundtrip(h,*owner.document());
    Document snapshot;
    expect(DocumentFile::readSnapshot(path,snapshot).ok() && !findAsset(snapshot,"removed") &&
           !findLayer(snapshot,"removed-layer") && sameTimeline(snapshot.timeline,{{30000,1001},36}),
           "file: redone timing and deletion survive reopening");
    expect(h.undo().changed && h.undo().changed && h.undo().changed,"file: restore before lock checks");
    auto database=sql::Database::open(std::filesystem::u8path(path));
    if (!database) { expect(false,"file: lock connection"); return; }
    auto lock=sql::Transaction::begin(*database,true);
    if (!lock) { expect(false,"file: acquire lock"); return; }
    const auto before=encodeIisc(*owner.document()).bytes; const auto revision=owner.revision();
    expect(!h.setTimeline({{48,1},24}).ok() && owner.revision()==revision &&
           encodeIisc(*owner.document()).bytes==before && h.canRedo(),
           "file: locked timing edit preserves durable state and redo");
    expect(!h.removeLayerWithAsset("removed-layer").ok() && owner.revision()==revision &&
           encodeIisc(*owner.document()).bytes==before && h.canRedo(),
           "file: locked combined delete preserves durable state and redo");
    expect(lock->rollback().has_value(),"file: release lock");
    expect(h.setTimeline({{48,1},24}).changed && owner.lastWriteStatistics().recordsWritten==2 &&
           h.undo().changed && h.removeLayerWithAsset("removed-layer").changed && h.undo().changed,
           "file: both commands can be retried and timeline writes header plus authorship");
    const auto expected=owner.document()->timeline; const auto saved=h.statistics();
    DocumentFile other; expect(other.open(path).ok(),"file: open external writer");
    DocumentEditor editor(other); expect(editor.setLayerVisible("back-layer",false).changed,"file: external edit");
    expect(h.setTimeline(expected).code==DocumentHistoryCode::Conflict &&
           h.statistics().undoEntries==saved.undoEntries && h.statistics().redoEntries==saved.redoEntries,
           "file: a no-op timing request detects a stale backing store");
}
bool writeLegacy(const std::filesystem::path &path) {
    return writeLegacyWorkingFile(path, std::filesystem::u8path(IISHAREDCANVAS_TEST_RESOURCE_DIR) /
                                         "fixtures/legacy-working-v1.tsv");
}
void legacy() {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory tmp(parent);
    if (!tmp.isValid()) { expect(false,"legacy: temporary directory"); return; }
    const auto timelinePath=tmp.path()/"timeline-v1.iisc";
    const auto removePath=tmp.path()/"remove-v1.iisc";
    if (!writeLegacy(timelinePath) || !writeLegacy(removePath)) { expect(false,"legacy: fixture"); return; }
    DocumentFile timing;
    if (!timing.open(timelinePath.string()).ok()) { expect(false,"legacy: open timing file"); return; }
    DocumentHistory h(timing); const auto old=timing.document()->timeline;
    expect(timing.storageVersion()==1 && h.setTimeline(old).ok() && timing.storageVersion()==1 &&
           timing.revision()==0,"legacy: no-op timing does not migrate the file");
    expect(h.setTimeline({{30,1},12}).changed && timing.storageVersion()==2 && timing.revision()==1 &&
           h.undo().changed && sameTimeline(timing.document()->timeline,old) && h.redo().changed,
           "legacy: timing edit migrates once and supports undo and redo");
    Document reopened;
    expect(DocumentFile::readSnapshot(timelinePath.string(),reopened).ok() &&
           sameTimeline(reopened.timeline,{{30,1},12}),"legacy: migrated timing survives reopen");
    DocumentFile removed;
    if (!removed.open(removePath.string()).ok()) { expect(false,"legacy: open removal file"); return; }
    DocumentHistory deletion(removed);
    expect(removed.storageVersion()==1 && deletion.removeLayerWithAsset("layer").changed &&
           removed.storageVersion()==2 && removed.revision()==1 && !findAsset(*removed.document(),"paint") &&
           deletion.undo().changed && findAsset(*removed.document(),"paint") &&
           findLayer(*removed.document(),"layer") && deletion.redo().changed,
           "legacy: combined removal migrates and remains reversible");
    expect(DocumentFile::readSnapshot(removePath.string(),reopened).ok() &&
           !findAsset(reopened,"paint") && !findLayer(reopened,"layer"),
           "legacy: migrated removal survives reopen");
}
std::uint64_t checksum(const RasterLayer &pixels,std::uint64_t &checked) {
    std::uint64_t result=1469598103934665603ULL;
    for (const auto pixel:pixels.pixels) { result^=pixel; result*=1099511628211ULL; ++checked; }
    return result;
}
void largeFile() {
    constexpr int side=8192;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory tmp(parent);
    if (!tmp.isValid()) { expect(false,"8K: temporary directory"); return; }
    const auto path=(tmp.path()/"compound-8k.iisc").string();
    Document d; d.extent={side,side}; d.timeline={{24,1},12};
    std::uint32_t random=37;
    for (const auto *id:{"back","heavy"}) {
        auto pixels=makeRasterLayer(side,side);
        for (auto &value:pixels.pixels) { random^=random<<13; random^=random>>17; random^=random<<5; value=random; }
        d.assets.emplace_back(RasterAsset{id,std::move(pixels)});
    }
    d.assets.emplace_back(RasterAsset{"blank",makeRasterLayer(side,side)});
    d.layers.emplace_back(StaticBitmapLayer{{"back-layer","Back"},StaticSource{"back"}});
    d.layers.emplace_back(StaticBitmapLayer{{"heavy-layer","Dense"},StaticSource{"heavy"}});
    d.layers.emplace_back(StaticBitmapLayer{{"blank-layer","Blank"},StaticSource{"blank"}});
    layerProperties(d.layers.back()).appearance.rasterMask=LayerMask{.assetId="back"};
    AudioAsset audio; audio.id="audio"; audio.channelCount=1; audio.samples.resize(131072);
    for (std::size_t i=0;i<audio.samples.size();++i) audio.samples[i]=std::int16_t((i*17)%32768);
    d.audioAssets.push_back(std::move(audio));
    std::uint64_t checked=0;
    const auto originalBack=checksum(findRasterAsset(d,"back")->pixels,checked);
    const auto originalHeavy=checksum(findRasterAsset(d,"heavy")->pixels,checked);
    const auto originalAudio=d.audioAssets.front().samples;
    DocumentFile owner;
    if (!owner.create(path,d).ok()) { expect(false,"8K: create working file"); return; }
    d={};
    DocumentHistory h(owner);
    const auto *backBuffer=findRasterAsset(*owner.document(),"back")->pixels.pixels.data();
    const auto *heavyBuffer=findRasterAsset(*owner.document(),"heavy")->pixels.pixels.data();
    const auto *audioBuffer=owner.document()->audioAssets.front().samples.data();
    allocations::maximum=0; allocations::total=0; allocations::active=true;
    const auto startedTiming=std::chrono::steady_clock::now();
    const auto timing=h.setTimeline({{30000,1001},36});
    const auto timingUs=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-startedTiming).count();
    allocations::active=false;
    const auto timingMax=allocations::maximum, timingTotal=allocations::total;
    expect(timing.changed && h.undo().changed && h.canRedo(),"8K: timing establishes redo branch");
    const auto revision=owner.revision(); const auto stats=h.statistics();
    expect(h.removeLayerWithAsset("heavy-layer").code==DocumentHistoryCode::BudgetExceeded &&
           owner.revision()==revision && h.canRedo() && h.statistics().retainedBytes==stats.retainedBytes &&
           findRasterAsset(*owner.document(),"heavy")->pixels.pixels.data()==heavyBuffer,
           "8K: over-budget dense asset deletion is rejected before mutation and preserves redo");
    expect(h.redo().changed,"8K: redo survives dense asset rejection");
    allocations::maximum=0; allocations::total=0; allocations::active=true;
    const auto startedRemove=std::chrono::steady_clock::now();
    const auto removal=h.removeLayerWithAsset("blank-layer");
    const auto removeUs=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-startedRemove).count();
    allocations::active=false;
    const auto removeMax=allocations::maximum, removeTotal=allocations::total;
    const auto retained=h.statistics().retainedBytes;
    expect(removal.changed && removal.entryBytes>0 && !findAsset(*owner.document(),"blank") &&
           findRasterAsset(*owner.document(),"back")->pixels.pixels.data()==backBuffer &&
           findRasterAsset(*owner.document(),"heavy")->pixels.pixels.data()==heavyBuffer &&
           owner.document()->audioAssets.front().samples.data()==audioBuffer,
           "8K: removing a blank asset preserves existing raster and audio buffers");
#if !COMPOUND_HISTORY_ASAN
    expect(timingMax<32*1024*1024 && removeMax<32*1024*1024,
           "8K: timing and deletion do not allocate a full image in one C++ allocation");
#endif
    expect(h.undo().changed && findRasterAsset(*owner.document(),"blank") && h.undo().changed &&
           sameTimeline(owner.document()->timeline,{{24,1},12}),"8K: undo restores blank asset and timing");
    const auto *restoredBlank=findRasterAsset(*owner.document(),"blank");
    expect(restoredBlank && std::all_of(restoredBlank->pixels.pixels.begin(),restoredBlank->pixels.pixels.end(),
                                       [](auto pixel){return pixel==0;}),"8K: restored blank pixels are exact");
    expect(checksum(findRasterAsset(*owner.document(),"back")->pixels,checked)==originalBack &&
           checksum(findRasterAsset(*owner.document(),"heavy")->pixels,checked)==originalHeavy &&
           owner.document()->audioAssets.front().samples==originalAudio,
           "8K: undo preserves both dense images and audio");
    expect(h.redo().changed && h.redo().changed && !findAsset(*owner.document(),"blank"),
           "8K: redo restores timing and deletes the blank asset");
    auto probe=fixture(); const auto probeBefore=encodeIisc(probe).bytes;
    const auto shortRead=DocumentFile::readSnapshot(path,probe,{},1);
    expect(shortRead.ok() || (shortRead.code==DocumentFileCode::Conflict &&
           encodeIisc(probe).bytes==probeBefore),
           "8K: an expired backup reports conflict and preserves the caller's prior output");
    probe={};
    Document reopened;
    const auto read=DocumentFile::readSnapshot(path,reopened);
    if (!read.ok()) std::cerr << "8K_READ_ERROR code=" << static_cast<int>(read.code)
                              << " message=" << read.message << '\n';
    expect(read.ok(),"8K: reopen durable file");
    const auto *finalBack=findRasterAsset(reopened,"back"), *finalHeavy=findRasterAsset(reopened,"heavy");
    expect(finalBack && finalHeavy && checksum(finalBack->pixels,checked)==originalBack &&
           checksum(finalHeavy->pixels,checked)==originalHeavy && reopened.audioAssets.front().samples==originalAudio &&
           sameTimeline(reopened.timeline,{{30000,1001},36}) && !findAsset(reopened,"blank"),
           "8K: reopened state preserves dense pixels, audio and final timing");
    std::cout << "COMPOUND_HISTORY_8K retained=" << retained << " checked_pixels=" << checked
              << " file_bytes=" << std::filesystem::file_size(path) << " timing_max_cpp=" << timingMax
              << " timing_total_cpp=" << timingTotal << " delete_max_cpp=" << removeMax
              << " delete_total_cpp=" << removeTotal << " timing_us=" << timingUs
              << " delete_us=" << removeUs << " dense_rejected=1"
              << " short_timeout=" << (shortRead.ok()?"completed":"conflict") << '\n';
}
}
int main(int argc,char **argv) {
    if (argc==2 && std::string_view(argv[1])=="--large") { largeFile(); return failures?1:0; }
    memory(); constraints();
    if (!(argc==2 && std::string_view(argv[1])=="--memory")) { file(); legacy(); }
    return failures?1:0;
}
