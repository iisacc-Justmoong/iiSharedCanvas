#include <Bitmap/BitmapEditor.h>
#include <Brush/BrushResolve.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderDatabase.h>
#include <iiFileProviderNative.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <string_view>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STROKE_HISTORY_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(STROKE_HISTORY_ASAN)
#define STROKE_HISTORY_ASAN 1
#endif
#ifndef STROKE_HISTORY_ASAN
#define STROKE_HISTORY_ASAN 0
#endif
namespace allocations {
thread_local bool active=false;
thread_local std::size_t maximum=0,total=0;
}
#if !STROKE_HISTORY_ASAN
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
Document fixture(bool chunked) {
    Document d; d.extent={128,128};
    if (chunked) {
        d.canvasMode=CanvasMode::Infinite; d.infiniteCanvas={{-64,-64},32};
        d.assets.emplace_back(ChunkedRasterAsset{"paint",{{-1,-1,makeRasterLayer(32,32)}}});
    } else d.assets.emplace_back(RasterAsset{"paint",makeRasterLayer(128,128)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer","Paint"},StaticSource{"paint"}});
    return d;
}
std::uint32_t pixel(const Document &d,int x,int y) {
    if (const auto *r=findRasterAsset(d,"paint"))
        return r->pixels.pixels[std::size_t(y)*r->pixels.width+x];
    for (const auto &c:findChunkedRasterAsset(d,"paint")->chunks) {
        const int left=c.column*32, top=c.row*32;
        if (x>=left && x<left+32 && y>=top && y<top+32)
            return c.pixels.pixels[std::size_t(y-top)*32+x-left];
    }
    return 0;
}
std::vector<std::uint32_t> pixels(const Document &d,bool chunked) {
    std::vector<std::uint32_t> result; result.reserve(128*128);
    const int first=chunked?-64:0;
    for (int y=first;y<first+128;++y)
        for (int x=first;x<first+128;++x) result.push_back(pixel(d,x,y));
    return result;
}
StrokePoint point(double x,double y,double time) {
    StrokePoint p; p.position={x,y}; p.time=time; p.pressure=1;
    p.tiltX=.3; p.tiltY=-.2; p.rotationRadians=.5; p.custom[0]=.8;
    return p;
}
BitmapBrush brush() {
    BitmapBrush b; b.argb=0xff3579bd; b.size=7; b.flow=1; b.opacity=1;
    return b;
}
void memory(bool chunked) {
    auto d=fixture(chunked); DocumentHistory h(d);
    const auto original=pixels(d,chunked);
    expect(h.setLayerName("layer","Ink").changed && h.undo().changed,
           "memory: establish a redo branch");
    const auto stats=h.statistics(); const auto revision=d.authorship.revision();
    const double first=chunked?-40:12, second=chunked?-20:30, third=chunked?4:54;
    const auto started=h.beginStroke("paint",brush(),point(first,first,1),"Paint");
    expect(started.ok() && started.changed && h.strokeActive() && !h.pixelPreviewActive() &&
           h.strokeStorageBytes()>0 && h.strokePendingHistoryBytes()>0 &&
           h.statistics().redoEntries==stats.redoEntries && d.authorship.revision()>revision,
           "memory: first dab is visible while redo and cursor are preserved");
    expect(!h.canUndo() && !h.canRedo() &&
           h.undo().code==DocumentHistoryCode::PreviewActive &&
           h.setLayerName("layer","Busy").code==DocumentHistoryCode::PreviewActive &&
           h.beginPixelPreview("paint").code==DocumentHistoryCode::PreviewActive &&
           h.commitPixelPreview().code==DocumentHistoryCode::NoPreview,
           "memory: active stroke excludes unrelated commands and is not a pixel preview");
    const auto stable=pixels(d,chunked); const auto stableRevision=d.authorship.revision();
    auto invalid=point(second,second,.5);
    expect(h.continueStroke(invalid).code==DocumentHistoryCode::InvalidArgument &&
           pixels(d,chunked)==stable && d.authorship.revision()==stableRevision && h.strokeActive(),
           "memory: backwards sample preserves preview and active stroke");
    invalid=point(second,second,2); invalid.tiltX=std::numeric_limits<double>::quiet_NaN();
    expect(h.continueStroke(invalid).code==DocumentHistoryCode::InvalidArgument &&
           pixels(d,chunked)==stable && h.strokeActive(),"memory: non-finite sensor rejected");
    expect(h.continueStroke(point(second,second,2)).changed &&
           h.strokeDirtyBounds().width>0,"memory: continued stroke exposes dirty region");
    h.clearStrokeDirtyBounds();
    expect(h.strokeDirtyBounds().width==0,"memory: dirty region can be consumed");
    const auto ended=h.endStroke(point(third,third,3));
    const auto painted=pixels(d,chunked);
    expect(ended.ok() && ended.changed && ended.discardedRedoEntries==1 && ended.entryBytes>0 &&
           !h.strokeActive() && h.statistics().undoEntries==1 && painted!=original &&
           h.strokeDirtyBounds().width>0,"memory: one completed stroke replaces redo exactly once");
    expect(h.setLayerOpacity("layer",.5).changed && h.undo().changed && h.undo().changed &&
           pixels(d,chunked)==original && (!chunked || findChunkedRasterAsset(d,"paint")->chunks.size()==1),
           "memory: mixed metadata and stroke undo restore pixels and chunk presence");
    expect(h.redo().changed && h.redo().changed && pixels(d,chunked)==painted,
           "memory: mixed redo restores exact stroke pixels");
    expect(h.undo().changed && h.undo().changed &&
           h.beginStroke("paint",brush(),point(first,first,4)).changed,
           "memory: begin a cancellable alternate stroke");
    h.clearStrokeDirtyBounds();
    expect(h.cancelStroke().changed && pixels(d,chunked)==original && h.canRedo() &&
           h.strokeDirtyBounds().width>0 && !h.strokeActive(),
           "memory: cancel restores pixels, reports dirty region and keeps redo");
    expect(h.beginStroke("paint",brush(),point(-100,-100,5)).ok() &&
           !h.endStroke(point(-100,-100,6)).changed && h.canRedo(),
           "memory: a stroke outside the canvas does not consume redo");
    expect(h.cancelStroke().code==DocumentHistoryCode::NoPreview &&
           h.continueStroke(point(1,1,1)).code==DocumentHistoryCode::NoPreview,
           "memory: no active stroke is explicit");
    expect(h.beginPixelPreview("paint").ok() &&
           h.beginStroke("paint",brush(),point(first,first,8)).code==DocumentHistoryCode::PreviewActive &&
           h.cancelStroke().code==DocumentHistoryCode::NoPreview && h.cancelPixelPreview().ok(),
           "memory: an active pixel preview excludes stroke-specific operations");
}
BitmapBrush airbrush() {
    BitmapBrush b; b.size=4; b.flow=.02; b.argb=0xff3366cc;
    BrushPreset p; p.size=b.size; p.stroke.airbrushEnabled=true; p.stroke.airbrushRate=10;
    b.engineState=resolveBrushPreset(p,b.argb).brush; return b;
}
void clock(bool chunked) {
    auto d=fixture(chunked); DocumentHistory h(d);
    expect(h.beginStroke("paint",airbrush(),point(16,16,1)).changed &&
           h.advanceStrokeTime(1.2).changed,"clock: stationary airbrush emits with elapsed time");
    const auto before=pixel(d,16,16); const auto revision=d.authorship.revision();
    expect(h.rebaseStrokeTime(100).ok() && d.authorship.revision()==revision &&
           pixel(d,16,16)==before,"clock: resume rebases without drawing");
    expect(h.advanceStrokeTime(100.1).changed &&
           (pixel(d,16,16)>>24)==unsigned(std::lround(255*(1-std::pow(250.0/255,4)))),
           "clock: suspended time adds no catch-up spray");
    const auto stable=pixel(d,16,16);
    expect(h.advanceStrokeTime(99).code==DocumentHistoryCode::InvalidArgument &&
           h.rebaseStrokeTime(std::numeric_limits<double>::infinity()).code==DocumentHistoryCode::InvalidArgument &&
           pixel(d,16,16)==stable && h.strokeActive(),"clock: invalid times preserve session");
    expect(h.endStroke(point(16,16,100.1)).changed && h.undo().changed && pixel(d,16,16)==0 &&
           h.redo().changed && pixel(d,16,16)==stable,"clock: timed stroke is one undo command");
}
std::pair<int,int> footprint(const Document &d,bool chunked) {
    const int first=chunked?-64:0;
    int left=first+128,right=first-1,top=first+128,bottom=first-1;
    for (int y=first;y<first+128;++y) for (int x=first;x<first+128;++x)
        if (pixel(d,x,y)>>24) {
            left=std::min(left,x); right=std::max(right,x);
            top=std::min(top,y); bottom=std::max(bottom,y);
        }
    return {right-left+1,bottom-top+1};
}
void richInput(bool chunked) {
    auto configured=brush(); configured.size=16;
    BrushPreset preset; preset.size=configured.size;
    configured.engineState=resolveBrushPreset(preset,configured.argb).brush;
    configured.engineState->rasterizer.shape.roundness=.25;
    configured.engineState->rasterizer.shape.angleMode=BrushAngleMode::StylusRotation;
    auto horizontal=fixture(chunked), vertical=fixture(chunked);
    DocumentHistory first(horizontal), second(vertical);
    auto input=point(16,16,1); input.rotationRadians=0;
    expect(first.beginStroke("paint",configured,input).changed && first.endStroke(input).changed,
           "rich input: horizontal stylus stroke");
    input.rotationRadians=std::numbers::pi/2;
    expect(second.beginStroke("paint",configured,input).changed && second.endStroke(input).changed,
           "rich input: vertical stylus stroke");
    const auto a=footprint(horizontal,chunked), b=footprint(vertical,chunked);
    expect(a.first>a.second && b.second>b.first,
           "rich input: stylus rotation reaches the actual brush footprint");
    configured.engineState=resolveBrushPreset(preset,configured.argb).brush;
    BrushDynamicsBinding binding; binding.source=BrushDynamicsSource::Custom;
    binding.target=BrushDynamicsTarget::Opacity; binding.customInput=7;
    configured.engineState->dynamics.bindings.push_back(binding);
    auto low=fixture(chunked), high=fixture(chunked); DocumentHistory lowHistory(low), highHistory(high);
    input.rotationRadians=0; input.custom[7]=0;
    expect(lowHistory.beginStroke("paint",configured,input).ok() && lowHistory.endStroke(input).ok(),
           "rich input: zero custom channel stroke");
    input.custom[7]=1;
    expect(highHistory.beginStroke("paint",configured,input).changed && highHistory.endStroke(input).changed &&
           (pixel(high,16,16)>>24)>(pixel(low,16,16)>>24),
           "rich input: custom channel changes actual pixel opacity");
}
void randomSeed(bool chunked) {
    auto d=fixture(chunked); DocumentHistory h(d);
    auto configured=brush(); configured.size=8;
    BrushPreset preset; preset.size=configured.size;
    configured.engineState=resolveBrushPreset(preset,configured.argb).brush;
    configured.engineState->material.scatter.enabled=true;
    configured.engineState->material.scatter.radius=2;
    configured.engineState->material.scatter.count=8;
    const auto start=point(16,16,1), end=point(24,24,2);
    expect(h.beginStroke("paint",configured,start).changed && h.endStroke(end).changed,
           "seed: first scattered stroke");
    const auto first=pixels(d,chunked);
    expect(h.undo().changed && h.beginStroke("paint",configured,start).changed,
           "seed: preview second scattered stroke");
    const auto secondPreview=pixels(d,chunked);
    expect(h.cancelStroke().changed && h.beginStroke("paint",configured,start).changed &&
           pixels(d,chunked)==secondPreview && h.endStroke(end).changed,
           "seed: cancel and retry keep the same pending random sequence");
    const auto second=pixels(d,chunked);
    expect(first!=second,"seed: successive committed strokes do not reuse one scatter pattern");
    expect(h.undo().changed && h.redo().changed && pixels(d,chunked)==second,
           "seed: undo and redo preserve the committed random pixels exactly");
}
void limitsAndConflicts() {
    auto d=fixture(false); DocumentHistoryLimits tiny; tiny.maximumBytes=1;
    DocumentHistory h(d,tiny); const auto before=encodeIisc(d).bytes;
    expect(h.beginStroke("paint",brush(),point(12,12,1)).code==DocumentHistoryCode::BudgetExceeded &&
           !h.strokeActive() && encodeIisc(d).bytes==before,
           "budget: an inadmissible first dab is rejected before mutation");
    auto e=fixture(false); DocumentHistoryLimits limits; limits.maximumEntries=2;
    limits.overflow=DocumentHistoryOverflow::EvictOldest; DocumentHistory evict(e,limits);
    expect(evict.setLayerName("layer","Ink").changed && evict.setLayerVisible("layer",false).changed &&
           evict.beginStroke("paint",brush(),point(12,12,1)).changed &&
           evict.statistics().undoEntries==2,"eviction: old entries remain during preview");
    const auto done=evict.endStroke(point(20,20,2));
    expect(done.changed && done.evictedEntries==1 && evict.undo().changed && evict.undo().changed &&
           layerProperties(*findLayer(e,"layer")).name=="Ink" &&
           layerProperties(*findLayer(e,"layer")).visible,
           "eviction: completion reports the single oldest discarded entry");
    auto c=fixture(false); DocumentHistory conflict(c);
    expect(conflict.setLayerName("layer","Ink").changed &&
           conflict.beginStroke("paint",brush(),point(12,12,1)).changed,"conflict: begin");
    const auto painted=pixel(c,12,12);
    findRasterAsset(c,"paint")->pixels.pixels[12*128+12]=0xffabcdef;
    expect(conflict.cancelStroke().code==DocumentHistoryCode::Conflict && conflict.strokeActive() &&
           pixel(c,12,12)==0xffabcdef && conflict.statistics().undoEntries==1,
           "conflict: cancellation refuses to overwrite external pixel changes");
    findRasterAsset(c,"paint")->pixels.pixels[12*128+12]=painted;
    expect(conflict.cancelStroke().changed && conflict.canUndo(),
           "conflict: cancellation can be retried when pixels are restored");
    expect(conflict.beginStroke("paint",brush(),point(12,12,2)).changed,"abandon: begin");
    findRasterAsset(c,"paint")->pixels.pixels[12*128+12]=0xffabcdef;
    expect(conflict.abandonStroke().ok() && !conflict.strokeActive() && !conflict.canUndo() &&
           pixel(c,12,12)==0xffabcdef && conflict.statistics().retainedBytes==0,
           "abandon: explicit recovery retains pixels but discards existing history");
    auto x=fixture(false); DocumentHistory tracked(x);
    expect(tracked.setLayerName("layer","Ink").changed &&
           tracked.beginStroke("paint",brush(),point(12,12,1)).changed,"external: begin");
    DocumentEditor external(x);
    expect(external.setLayerVisible("layer",false).changed && tracked.cancelStroke().changed &&
           pixel(x,12,12)==0 && !layerProperties(*findLayer(x,"layer")).visible &&
           tracked.undo().code==DocumentHistoryCode::Conflict && tracked.reset().ok(),
           "external: safe cancel preserves an unrelated edit and requires explicit history rebase");
    auto badBrush=brush(); badBrush.size=std::numeric_limits<double>::infinity();
    const auto stable=encodeIisc(x).bytes;
    expect(tracked.beginStroke("paint",badBrush,point(12,12,3)).code==DocumentHistoryCode::InvalidArgument &&
           encodeIisc(x).bytes==stable && !tracked.strokeActive(),
           "invalid: brush configuration rejects without changing the document");
}
void file(bool chunked) {
    namespace sql=iiFileProvider::native::sql;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory tmp(parent);
    if (!tmp.isValid()) { expect(false,"file: temporary directory"); return; }
    const auto path=(tmp.path()/"stroke.iisc").string(); auto d=fixture(chunked); DocumentFile owner;
    if (!owner.create(path,d).ok()) { expect(false,"file: create"); return; }
    DocumentHistory h(owner); const auto original=pixels(*owner.document(),chunked);
    const double first=chunked?-40:12, second=chunked?-20:30;
    expect(h.setLayerName("layer","Ink").changed &&
           h.beginStroke("paint",brush(),point(first,first,1)).changed,"file: start durable stroke");
    Document snapshot;
    expect(DocumentFile::readSnapshot(path,snapshot).ok() && pixels(snapshot,chunked)!=original,
           "file: a reader observes the durable first dab");
    const auto stable=pixels(*owner.document(),chunked); const auto revision=owner.revision();
    auto db=sql::Database::open(std::filesystem::u8path(path));
    if (!db) { expect(false,"file: lock connection"); return; }
    auto lock=sql::Transaction::begin(*db,true);
    if (!lock) { expect(false,"file: acquire lock"); return; }
    expect(!h.continueStroke(point(second,second,2)).ok() && owner.revision()==revision &&
           pixels(*owner.document(),chunked)==stable && h.strokeActive(),
           "file: locked write preserves active preview and revision");
    expect(lock->rollback().has_value(),"file: release lock");
    expect(h.continueStroke(point(second,second,2)).changed &&
           h.endStroke(point(second+5,second+5,3)).changed,"file: retry and finish stroke");
    const auto painted=pixels(*owner.document(),chunked);
    expect(h.undo().changed && pixels(*owner.document(),chunked)==original &&
           h.undo().changed && layerProperties(*findLayer(*owner.document(),"layer")).name=="Paint" &&
           h.redo().changed && h.redo().changed &&
           DocumentFile::readSnapshot(path,snapshot).ok() && pixels(snapshot,chunked)==painted,
           "file: mixed undo/redo persist exact pixels and metadata");
    expect(h.beginStroke("paint",brush(),point(first,first,4)).changed && h.cancelStroke().changed &&
           DocumentFile::readSnapshot(path,snapshot).ok() && pixels(snapshot,chunked)==painted,
           "file: cancellation is durable");
    expect(h.beginStroke("paint",brush(),point(first,first,5)).changed,"file conflict: begin");
    DocumentFile other; expect(other.open(path).ok(),"file conflict: open second handle");
    DocumentEditor outside(other);
    expect(outside.setLayerVisible("layer",false).changed &&
           h.cancelStroke().code==DocumentHistoryCode::Conflict && h.strokeActive(),
           "file conflict: stale cancellation preserves the pending session");
    expect(h.abandonStroke().ok() && !h.strokeActive() && !h.isBound() &&
           h.statistics().retainedBytes==0,"file conflict: explicit abandon releases the invalid binding");
}
std::uint64_t checksum(const RasterLayer &r,std::uint64_t &checked) {
    std::uint64_t hash=1469598103934665603ULL;
    for (const auto p:r.pixels) { hash^=p; hash*=1099511628211ULL; ++checked; }
    return hash;
}
void largeFile() {
    constexpr int side=8192;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory tmp(parent);
    if (!tmp.isValid()) { expect(false,"8K: temporary directory"); return; }
    const auto path=(tmp.path()/"stroke-8k.iisc").string();
    Document d; d.extent={side,side}; d.timeline={{24,1},8};
    std::uint32_t random=1;
    for (const auto *id:{"back","paint"}) {
        auto image=makeRasterLayer(side,side);
        for (auto &p:image.pixels) { random^=random<<13; random^=random>>17; random^=random<<5; p=random; }
        d.assets.emplace_back(RasterAsset{id,std::move(image)});
    }
    d.layers.emplace_back(StaticBitmapLayer{{"back-layer","Back"},StaticSource{"back"}});
    d.layers.emplace_back(StaticBitmapLayer{{"layer","Paint"},StaticSource{"paint"}});
    layerProperties(d.layers.back()).appearance.rasterMask=LayerMask{.assetId="back"};
    LayerFilter filter; filter.name="Invert"; filter.operation=RasterEffect{RasterEffectKind::Invert};
    layerProperties(d.layers.back()).appearance.filters.push_back(filter);
    std::uint64_t checked=0;
    const auto sourcePaint=checksum(findRasterAsset(d,"paint")->pixels,checked);
    const auto sourceBack=checksum(findRasterAsset(d,"back")->pixels,checked);
    DocumentFile owner;
    if (!owner.create(path,d).ok()) { expect(false,"8K: create working file"); return; }
    d={};
    DocumentHistoryLimits limits; limits.maximumBytes=1024*1024;
    limits.maximumBitmapInputBytes=8*1024*1024;
    DocumentHistory h(owner,limits);
    expect(h.setLayerName("layer","Ink").changed,"8K: begin mixed history");
    const auto *paintBuffer=findRasterAsset(*owner.document(),"paint")->pixels.pixels.data();
    const auto *backBuffer=findRasterAsset(*owner.document(),"back")->pixels.pixels.data();
    allocations::maximum=0; allocations::total=0; allocations::active=true;
    expect(h.beginStroke("paint",brush(),point(2048,2048,1)).changed,"8K: begin brush stroke");
    std::size_t maxCache=h.strokeStorageBytes(), maxPending=h.strokePendingHistoryBytes();
    bool preservedBuffers=true;
    for (int i=1;i<=12;++i) {
        expect(h.continueStroke(point(2048+i*4,2048+i*2,1+i*.01)).ok(),"8K: continuous sample");
        maxCache=std::max(maxCache,h.strokeStorageBytes());
        maxPending=std::max(maxPending,h.strokePendingHistoryBytes());
        preservedBuffers=preservedBuffers && findRasterAsset(*owner.document(),"paint")->pixels.pixels.data()==paintBuffer &&
                         findRasterAsset(*owner.document(),"back")->pixels.pixels.data()==backBuffer;
    }
    expect(h.endStroke(point(2104,2076,1.14)).changed,"8K: finish single brush command");
    allocations::active=false;
    expect(preservedBuffers && findRasterAsset(*owner.document(),"paint")->pixels.pixels.data()==paintBuffer &&
           findRasterAsset(*owner.document(),"back")->pixels.pixels.data()==backBuffer &&
           maxCache<=limits.maximumBitmapInputBytes && maxPending<limits.maximumBytes,
           "8K: original image buffers survive the stroke within pending cache and history limits");
#if !STROKE_HISTORY_ASAN
    expect(allocations::maximum<32*1024*1024,
           "8K: measured stroke does not make a single full-image C++ allocation");
#endif
    const std::array<std::uint32_t,16> values{0xff202020,0xff303030,0xff404040,0xff505050,
        0xff606060,0xff707070,0xff808080,0xff909090,0xffa0a0a0,0xffb0b0b0,0xffc0c0c0,0xffd0d0d0,
        0xffe0e0e0,0xfff0f0f0,0xff010101,0xff101010};
    const std::array patch{BitmapPatch{{{2500,2500},4,4},values}};
    expect(h.replaceBitmapPatches("back",patch,"Mask edit").changed &&
           h.setLayerOpacity("layer",.75).changed,"8K: mix mask and metadata edits");
    const auto retained=h.statistics().retainedBytes;
    const auto painted=checksum(findRasterAsset(*owner.document(),"paint")->pixels,checked);
    const auto maskEdited=checksum(findRasterAsset(*owner.document(),"back")->pixels,checked);
    expect(painted!=sourcePaint && maskEdited!=sourceBack && retained<limits.maximumBytes &&
           h.statistics().undoEntries==4,"8K: mixed changes retain a bounded command history");
    expect(h.beginStroke("paint",brush(),point(3000,3000,2)).changed &&
           h.continueStroke(point(3012,3012,2.1)).changed,"8K: start cancellation sample");
    const auto started=std::chrono::steady_clock::now();
    expect(h.cancelStroke().changed,"8K: cancel restores original stroke pixels");
    const auto cancelUs=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-started).count();
    expect(checksum(findRasterAsset(*owner.document(),"paint")->pixels,checked)==painted &&
           h.statistics().undoEntries==4,"8K: cancel preserves the completed commands and pixels");
    for (int i=0;i<4;++i) expect(h.undo().changed,"8K: undo mixed commands");
    expect(checksum(findRasterAsset(*owner.document(),"paint")->pixels,checked)==sourcePaint &&
           checksum(findRasterAsset(*owner.document(),"back")->pixels,checked)==sourceBack &&
           layerProperties(*findLayer(*owner.document(),"layer")).name=="Paint",
           "8K: undo restores both high-entropy images and metadata");
    for (int i=0;i<4;++i) expect(h.redo().changed,"8K: redo mixed commands");
    Document reopened;
    expect(DocumentFile::readSnapshot(path,reopened).ok(),"8K: reopen durable file");
    const auto *finalPaint=findRasterAsset(reopened,"paint");
    const auto *finalBack=findRasterAsset(reopened,"back");
    expect(finalPaint && finalBack && checksum(finalPaint->pixels,checked)==painted &&
           checksum(finalBack->pixels,checked)==maskEdited &&
           layerProperties(*findLayer(reopened,"layer")).appearance.filters.size()==1,
           "8K: reopened redo retains exact images and filter settings");
    std::cout << "DOCUMENT_STROKE_FILE_8K retained=" << retained << " checked_pixels=" << checked
              << " cancel_us=" << cancelUs << " file_bytes=" << std::filesystem::file_size(path)
              << " max_cpp=" << allocations::maximum << " total_cpp=" << allocations::total
              << " max_cache=" << maxCache << " max_pending=" << maxPending
              << " buffers_preserved=" << preservedBuffers
              << " continuous_samples=14 filter_executed=false\n";
}
}
int main(int argc,char **argv) {
    if (argc==2 && std::string_view(argv[1])=="--large") largeFile();
    else if (argc==2 && std::string_view(argv[1])=="--seed") { randomSeed(false); randomSeed(true); }
    else { memory(false); memory(true); clock(false); clock(true); richInput(false); richInput(true);
           randomSeed(false); randomSeed(true);
           limitsAndConflicts(); file(false); file(true); }
    return failures?1:0;
}
