#include <Bitmap/BitmapEditor.h>
#include <Bitmap/ChunkedBitmapEditor.h>
#include <Brush/BrushResolve.h>
#include <File/DocumentFile.h>
#include <iiFileProviderNative.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>

namespace observed {
thread_local bool enabled = false;
thread_local std::size_t bytes = 0;
thread_local std::ptrdiff_t failAfter = -1;
thread_local bool failed = false;
}
void *operator new(std::size_t n) {
    if (observed::enabled) observed::bytes += n;
    if (observed::failAfter == 0) {
        observed::failAfter = -1;
        observed::failed = true;
        throw std::bad_alloc();
    }
    if (observed::failAfter > 0) --observed::failAfter;
    if (auto *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

using namespace iiSharedCanvas;
namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
Document makeDocument(bool chunked) {
    Document d; d.extent = {1024, 192};
    if (chunked) {
        d.infiniteCanvas.chunkSize = 256;
        d.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    } else d.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(1024, 192)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return d;
}
StrokePoint point(double x, double y, double t, double pressure) {
    StrokePoint p; p.position = {x,y}; p.time = t; p.pressure = pressure;
    p.tiltX = .2; p.tiltY = -.1; return p;
}
template<class Editor> bool setPixel(Editor &editor, int x, int y, std::uint32_t value) {
    const std::array pixels{value};
    const std::array patches{BitmapPatch{{{x,y},1,1},pixels}};
    return editor.replacePatches(patches);
}
template<class Editor> void exercise(bool chunked, bool deferred=false) {
    auto d = makeDocument(chunked); Editor e(d, "paint");
    BitmapBrush b; b.size=6; b.argb=0xff6533ff; b.pressureToOpacityEnabled=true;
    check(setPixel(e,4,4,0xff010203), "establish internal history");
    check((deferred?e.setDeferredStrokeHistory(true):e.setExternalStrokeHistory(true)) && e.setBrush(b),
          "enable incremental stroke history");
    if (!e.beginStrokeInput(point(16,32,1,.25)))
        throw std::runtime_error("begin stroke: " + e.lastError());
    check(!e.setExternalStrokeHistory(false) && e.strokeActive(),"mode cannot change during a stroke");
    auto measure = [&](StrokePoint p) {
        observed::bytes=0; observed::enabled=true;
        const auto start=std::chrono::steady_clock::now();
        const bool ok=e.continueStrokeInput(p);
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-start).count();
        observed::enabled=false; check(ok,"continue stroke");
        return std::pair{observed::bytes,elapsed};
    };
    const auto early=measure(point(18,32,1.01,.25));
    double time=1.01;
    for (int i=1;i<=15;++i) check(e.continueStrokeInput(point(16+i*64,32,time+=.01,1)),"upper path");
    check(e.continueStrokeInput(point(976,96,time+=.01,1)),"path turn");
    for (int i=14;i>=0;--i) check(e.continueStrokeInput(point(16+i*64,96,time+=.01,1)),"lower path");
    const auto late=measure(point(18,96,time+=.01,1));
    const auto first=e.pixelAt(16,32).value_or(0), last=e.pixelAt(18,96).value_or(0);
    check((first>>24)>0 && (last>>24)>(first>>24),"pressure and intermediate path remain visible");
    check(e.pixelAt(512,32).value_or(0) && e.pixelAt(976,64).value_or(0) &&
          e.pixelAt(512,96).value_or(0),"continuous path and corner remain visible");
    std::cout << (chunked?"chunked":"finite") << (deferred?" deferred":" external") << " early_bytes=" << early.first
              << " late_bytes=" << late.first << " early_us=" << early.second
              << " late_us=" << late.second << std::endl;
    check(late.first <= early.first*3 + 65536, "one sample's allocations grow with previous stroke length");
    check(e.endStrokeInput(point(20,96,time+=.01,1)),"finish stroke");
    if (deferred) {
        const auto painted=e.pixelAt(512,32).value_or(0);
        check(e.canUndo() && e.undo() && e.pixelAt(512,32).value_or(0)==0 &&
              e.pixelAt(4,4).value_or(0)==0xff010203 && e.redo() &&
              e.pixelAt(512,32).value_or(0)==painted,
              "deferred history must undo and redo the whole stroke");
    } else check(!e.canUndo() && e.pixelAt(4,4).value_or(0)==0xff010203,
                 "external completion invalidates internal history, not prior pixels");

    auto cancelled=makeDocument(chunked); Editor c(cancelled,"paint");
    check(setPixel(c,4,4,0xff010203) && c.undo(),"establish redo");
    check((deferred?c.setDeferredStrokeHistory(true):c.setExternalStrokeHistory(true)) &&
          c.setBrush(b),"enable cancellable stroke");
    check(c.beginStrokeInput(point(20,20,1,1)) && c.continueStrokeInput(point(150,20,2,1)),"paint before cancel");
    c.cancelStroke();
    check(!c.strokeActive() && c.pixelAt(20,20).value_or(0)==0 && c.canRedo(),
          "cancel restores pixels and previous redo");
    check(c.redo() && c.pixelAt(4,4).value_or(0)==0xff010203,"prior redo survives cancellation");

    auto unchanged=makeDocument(chunked); Editor n(unchanged,"paint");
    check(setPixel(n,4,4,0xff010203) && n.undo(),"establish no-op redo");
    auto transparent=b; transparent.opacity=0;
    check(n.setExternalStrokeHistory(true) && n.setBrush(transparent) &&
          n.beginStrokeInput(point(20,20,1,1)) && n.endStrokeInput(point(100,20,2,1)) &&
          n.canRedo(),"no-op stroke must preserve redo");
}
template<class Editor> void equivalence(bool chunked, bool eraser, bool multiply) {
    auto streamed=makeDocument(chunked), reference=makeDocument(chunked);
    Editor a(streamed,"paint"), b(reference,"paint");
    std::vector<std::uint32_t> background(256*128,0xff607080);
    const std::array patches{BitmapPatch{{{0,0},256,128},background}};
    check(a.replacePatches(patches) && b.replacePatches(patches),"create equivalent backgrounds");
    BitmapBrush brush; brush.argb=0xffdf7030; brush.size=13; brush.opacity=.72; brush.flow=.43;
    brush.eraser=eraser;
    if (multiply) {
        BrushPreset preset; preset.size=brush.size;
        brush.engineState=resolveBrushPreset(preset,brush.argb).brush;
        check(bool(brush.engineState),"resolve multiply brush");
        brush.engineState->rasterizer.blendMode=RasterBlendMode::Multiply;
    }
    check(a.setExternalStrokeHistory(true) && a.setBrush(brush) && b.setBrush(brush),"set comparison brushes");
    auto start=point(20,25,1,.15);
    check(a.beginStrokeInput(start) && b.beginStrokeInput(start),"begin comparison");
    for (int i=1;i<=22;++i) {
        const auto p=point(20+i*9,25+(i%6)*12,1+i*.01,.15+.8*(i%5)/4.);
        check(a.continueStrokeInput(p) && b.continueStrokeInput(p),"sample comparison path");
        for(int y=0;y<110;++y) for(int x=0;x<240;++x)
            check(a.pixelAt(x,y)==b.pixelAt(x,y),"live streamed pixels differ from existing compositor");
    }
    const auto end=point(218,37,1.24,.8);
    check(a.endStrokeInput(end) && b.endStrokeInput(end),"finish comparison");
    for(int y=0;y<110;++y) for(int x=0;x<240;++x)
        check(a.pixelAt(x,y)==b.pixelAt(x,y),"finished streamed pixels differ from existing compositor");
}
template<class Editor> void deferredBudget(bool chunked) {
    auto d=makeDocument(chunked);
    RasterHistoryLimits limits; limits.maximumBytes=1;
    Editor e(d,"paint",limits);
    BitmapBrush brush;brush.size=8;
    check(e.setDeferredStrokeHistory(true) && e.setBrush(brush) &&
          e.beginStrokeInput(point(20,20,1,1)) && e.continueStrokeInput(point(40,20,2,1)),
          "deferred preview is visible before history admission");
    const auto painted=e.pixelAt(20,20);
    check(!e.endStrokeInput(point(40,20,3,1)) &&
          e.lastHistoryResult().code==RasterHistoryCode::BudgetExceeded && e.strokeActive() &&
          e.pixelAt(20,20)==painted,"rejected final history must preserve active preview");
    e.cancelStroke();
    check(!e.strokeActive() && e.pixelAt(20,20).value_or(0)==0,
          "budget rejection remains cancellable");
}
void conflictAndAllocations() {
    auto d=makeDocument(false); BitmapEditor editor(d,"paint");
    BitmapBrush brush; brush.size=8;
    check(editor.setExternalStrokeHistory(true) && editor.setBrush(brush) &&
          editor.beginStrokeInput(point(20,20,1,1)),"begin conflict case");
    auto *asset=findRasterAsset(d,"paint");
    const auto index=std::size_t(20)*asset->pixels.width+20;
    const auto saved=asset->pixels.pixels[index];
    asset->pixels.pixels[index]=0xffabcdef;
    check(!editor.continueStrokeInput(point(24,20,2,1)) &&
          editor.lastHistoryResult().code==RasterHistoryCode::Conflict &&
          asset->pixels.pixels[index]==0xffabcdef,"changed tile conflict preserves outside edit");
    editor.cancelStroke();
    check(editor.strokeActive() && asset->pixels.pixels[index]==0xffabcdef,"cancel conflict preserves outside edit");
    asset->pixels.pixels[index]=saved;
    editor.cancelStroke();
    check(!editor.strokeActive() && asset->pixels.pixels[index]==0,"cancel succeeds once conflict is restored");

    auto copied=makeDocument(false); BitmapEditor original(copied,"paint");
    check(original.setExternalStrokeHistory(true) && original.setBrush(brush) &&
          original.beginStrokeInput(point(20,20,1,1)),"begin copy-on-write stroke");
    auto stale=original;
    check(original.continueStrokeInput(point(25,20,2,1)),"update original stroke copy");
    check(!stale.continueStrokeInput(point(26,20,2,1)) &&
          stale.lastHistoryResult().code==RasterHistoryCode::Conflict,
          "copied cursor must not overwrite another cursor's preview");
    original.cancelStroke();

    auto deferred=makeDocument(false); BitmapEditor pending(deferred,"paint");
    check(pending.setDeferredStrokeHistory(true) && pending.setBrush(brush) &&
          pending.beginStrokeInput(point(20,20,1,1)) && pending.continueStrokeInput(point(150,20,2,1)),
          "begin deferred conflict case");
    auto *pendingAsset=findRasterAsset(deferred,"paint");
    const auto prior=pendingAsset->pixels.pixels[index];
    pendingAsset->pixels.pixels[index]=0xffabcdef;
    check(!pending.endStrokeInput(point(155,20,3,1)) &&
          pending.lastHistoryResult().code==RasterHistoryCode::Conflict && pending.strokeActive() &&
          pendingAsset->pixels.pixels[index]==0xffabcdef,"deferred release detects earlier tile conflict");
    pendingAsset->pixels.pixels[index]=prior;
    pending.cancelStroke();
    check(!pending.strokeActive(),"deferred conflict remains cancellable");

    int rejected=0, succeeded=0;
    for (std::ptrdiff_t n=0;n<100;++n) {
        auto candidate=makeDocument(false); BitmapEditor e(candidate,"paint");
        check(e.setExternalStrokeHistory(true) && e.setBrush(brush) &&
              e.beginStrokeInput(point(20,20,1,1)),"begin allocation case");
        auto *pixels=findRasterAsset(candidate,"paint");
        const auto before=pixels->pixels.pixels;
        const auto revision=candidate.authorship.revision();
        observed::failed=false; observed::failAfter=n;
        bool accepted=false;
        try { accepted=e.continueStrokeInput(point(75,20,2,1)); }
        catch (const std::bad_alloc &) {}
        observed::failAfter=-1;
        if (!accepted) {
            ++rejected;
            check(observed::failed && pixels->pixels.pixels==before &&
                  candidate.authorship.revision()==revision && e.strokeActive(),
                  "allocation failure changed prior preview");
        } else ++succeeded;
        e.cancelStroke();
        check(!e.strokeActive() && pixels->pixels.pixels[20*pixels->pixels.width+20]==0,
              "allocation case remains cancellable");
    }
    check(rejected>=10 && succeeded>0,"allocation injection did not cover failure and success");
}
void unsupportedBindings() {
    auto growing=makeDocument(true);
    growing.canvasMode=CanvasMode::Infinite;
    ChunkedBitmapEditor c(growing,"paint");
    check(c.setStrokeCanvasGrowth(true) && !c.setExternalStrokeHistory(true),
          "growth must reject external stroke history");
    check(!c.setDeferredStrokeHistory(true),"growth must reject deferred stroke history");
    check(c.setStrokeCanvasGrowth(false) && c.setExternalStrokeHistory(true) &&
          !c.setStrokeCanvasGrowth(true),"external history must reject enabling growth");
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    check(temporary.isValid(),"file binding test directory");
    DocumentFile file;
    check(file.create((temporary.path()/"external-stroke.iisc").string(),makeDocument(false)).ok(),
          "create file binding");
    BitmapEditor bound(file,"paint");
    check(!bound.setExternalStrokeHistory(true) && !bound.setDeferredStrokeHistory(true),
          "durable file cannot opt into deferred or external history");
}
}
int main() try {
    exercise<BitmapEditor>(false);
    exercise<ChunkedBitmapEditor>(true);
    exercise<BitmapEditor>(false,true);
    exercise<ChunkedBitmapEditor>(true,true);
    equivalence<BitmapEditor>(false,false,false);
    equivalence<BitmapEditor>(false,true,false);
    equivalence<BitmapEditor>(false,false,true);
    equivalence<ChunkedBitmapEditor>(true,false,false);
    equivalence<ChunkedBitmapEditor>(true,true,false);
    deferredBudget<BitmapEditor>(false);
    deferredBudget<ChunkedBitmapEditor>(true);
    conflictAndAllocations();
    unsupportedBindings();
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
