#include <Document/DocumentHistory.h>
#include <Document/DocumentEditor.h>
#include <Bitmap/BitmapEditor.h>
#include <Bitmap/ChunkedBitmapEditor.h>
#include <File/DocumentFile.h>
#include <iiFileProviderNative.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, std::string_view what) {
    if (!value) { ++failures; std::cerr << what << '\n'; }
}
Document finite(int side = 32) {
    Document d; d.extent = {side, side};
    d.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(side, side)});
    d.assets.emplace_back(RasterAsset{"mask", makeRasterLayer(side, side)});
    d.assets.emplace_back(RasterAsset{"backdrop", makeRasterLayer(side, side)});
    d.layers.emplace_back(StaticBitmapLayer{{"paint-layer", "Paint"}, StaticSource{"paint"}});
    layerProperties(d.layers.front()).appearance.rasterMask = LayerMask{.assetId = "mask"};
    d.layers.emplace_back(StaticBitmapLayer{{"back-layer", "Backdrop"}, StaticSource{"backdrop"}});
    return d;
}
std::uint32_t pixel(const Document &d, std::string_view asset, int x, int y) {
    const auto *r = findRasterAsset(d, std::string(asset));
    return r->pixels.pixels[std::size_t(y) * r->pixels.width + x];
}
const LayerProperties &properties(const Document &d, std::string_view id) {
    return layerProperties(*findLayer(d, std::string(id)));
}
void orderedHistory() {
    auto d = finite(); DocumentHistory h(d);
    const std::array<std::uint32_t,1> a{0xff123456}, b{0x00123456};
    const std::array p{BitmapPatch{{{1,1},1,1},a}}, m{BitmapPatch{{{2,2},1,1},b}};
    expect(h.isBound() && h.replaceBitmapPatches("paint",p).changed, "ordered: paint");
    expect(h.setLayerName("paint-layer","Ink").changed, "ordered: rename");
    expect(h.replaceBitmapPatches("mask",m).changed, "ordered: mask");
    expect(h.setLayerOpacity("paint-layer",0.5).changed, "ordered: opacity");
    expect(h.moveLayer("back-layer",0).changed, "ordered: reorder");
    expect(h.statistics().undoEntries==5 && h.statistics().redoEntries==0, "ordered: five entries");
    expect(h.undo().changed && layerIndex(d,"back-layer")==1, "ordered: reorder undo");
    expect(h.undo().changed && properties(d,"paint-layer").opacity==1, "ordered: opacity undo");
    expect(h.undo().changed && pixel(d,"mask",2,2)==0, "ordered: mask undo");
    expect(h.undo().changed && properties(d,"paint-layer").name=="Paint", "ordered: rename undo");
    expect(h.undo().changed && pixel(d,"paint",1,1)==0 && !h.canUndo(), "ordered: paint undo");
    expect(h.statistics().redoEntries==5, "ordered: redo count");
    for (int i=0;i<5;++i) expect(h.redo().changed, "ordered: redo command");
    expect(pixel(d,"paint",1,1)==a[0] && pixel(d,"mask",2,2)==b[0] &&
           properties(d,"paint-layer").name=="Ink" && properties(d,"paint-layer").opacity==0.5 &&
           layerIndex(d,"back-layer")==0, "ordered: redo restores all domains");
    expect(h.undo().changed && h.undo().changed, "branch: make redo branch");
    const auto before = h.statistics();
    const auto noop = h.setLayerName("paint-layer","Ink");
    expect(noop.ok() && !noop.changed && h.statistics().redoEntries==before.redoEntries,
           "branch: no-op keeps redo");
    const auto branch = h.setLayerVisible("paint-layer",false);
    expect(branch.changed && branch.discardedRedoEntries==2 && !h.canRedo(),
           "branch: successful change discards redo");
    AffineTransform t; t.translationX=7; t.translationY=-3;
    expect(h.setLayerTransform("paint-layer",t).changed &&
           h.setLayerBlendMode("paint-layer",RasterBlendMode::Multiply).changed,
           "ordered: transform and blend");
    expect(h.undo().changed && h.undo().changed &&
           properties(d,"paint-layer").transform.translationX==0 &&
           properties(d,"paint-layer").blendMode==RasterBlendMode::SourceOver,
           "ordered: transform and blend undo");
}
void budgetsAndConflicts() {
    auto d=finite(); DocumentHistoryLimits limits; limits.maximumEntries=2;
    DocumentHistory reject(d,limits);
    expect(reject.setLayerName("paint-layer","A").changed &&
           reject.setLayerVisible("paint-layer",false).changed, "budget: fill count");
    const auto revision=d.authorship.revision(); const auto stats=reject.statistics();
    const auto denied=reject.setLayerOpacity("paint-layer",0.25);
    expect(denied.code==DocumentHistoryCode::BudgetExceeded &&
           d.authorship.revision()==revision && reject.statistics().retainedBytes==stats.retainedBytes,
           "budget: reject before mutation");
    expect(reject.setLayerVisible("paint-layer",false).ok() &&
           d.authorship.revision()==revision, "budget: unchanged is allowed at capacity");
    expect(reject.undo().changed, "budget: undo");
    expect(reject.setLayerName("paint-layer","A").ok() && reject.canRedo(),
           "budget: no-op preserves redo at capacity");
    expect(reject.setLayerOpacity("paint-layer",0.25).discardedRedoEntries==1 && !reject.canRedo(),
           "budget: branch reuses redo budget");
    auto e=finite(); limits.overflow=DocumentHistoryOverflow::EvictOldest;
    DocumentHistory evict(e,limits);
    expect(evict.setLayerName("paint-layer","A").changed &&
           evict.setLayerVisible("paint-layer",false).changed, "eviction: first commands");
    const auto evicted=evict.setLayerOpacity("paint-layer",0.25);
    expect(evicted.changed && evicted.evictedEntries==1 && evict.statistics().undoEntries==2,
           "eviction: explicit oldest removal");
    expect(evict.undo().changed && evict.undo().changed && !evict.canUndo() &&
           properties(e,"paint-layer").name=="A" && properties(e,"paint-layer").visible &&
           properties(e,"paint-layer").opacity==1, "eviction: retained commands undo in order");
    auto z=finite(); DocumentHistoryLimits tiny; tiny.maximumBytes=1; tiny.maximumEntries=0;
    DocumentHistory limited(z,tiny); const std::array<std::uint32_t,1> zero{0}, one{0xff00ff00};
    const std::array nochange{BitmapPatch{{{1,1},1,1},zero}}, change{BitmapPatch{{{1,1},1,1},one}};
    expect(limited.replaceBitmapPatches("paint",nochange).ok(), "zero budget: no-op bitmap");
    expect(limited.setLayerName("paint-layer","Paint").ok(), "zero budget: no-op metadata");
    expect(limited.replaceBitmapPatches("paint",change).code==DocumentHistoryCode::BudgetExceeded &&
           limited.setLayerName("paint-layer","New").code==DocumentHistoryCode::BudgetExceeded &&
           pixel(z,"paint",1,1)==0 && z.authorship.revision()==0, "zero budget: changed operations reject");
    auto c=finite(); DocumentHistory conflict(c); expect(conflict.setLayerName("paint-layer","Ink").changed, "conflict: setup");
    DocumentEditor other(c); expect(other.setLayerVisible("back-layer",false).changed, "conflict: external editor");
    expect(conflict.undo().code==DocumentHistoryCode::Conflict && conflict.statistics().undoEntries==1,
           "conflict: tracked outside edit preserves cursor");
    expect(conflict.reset().ok() && !conflict.canUndo(), "conflict: explicit reset rebases");
    expect(conflict.replaceBitmapPatches("paint",change).changed, "conflict: pixel setup");
    findRasterAsset(c,"paint")->pixels.pixels[33]=0xffabcdef;
    expect(conflict.undo().code==DocumentHistoryCode::Conflict && pixel(c,"paint",1,1)==0xffabcdef &&
           conflict.statistics().undoEntries==1, "conflict: direct pixel mutation does not get overwritten");

    auto invalid=finite(); DocumentHistory guarded(invalid);
    expect(guarded.setLayerName("paint-layer","Ink").changed && guarded.undo().changed,
           "invalid: establish redo");
    const auto saved=guarded.statistics(); const auto rev=invalid.authorship.revision();
    AffineTransform nanTransform; nanTransform.m11=std::numeric_limits<double>::quiet_NaN();
    const std::array badOverlap{BitmapPatch{{{1,1},1,1},one},BitmapPatch{{{1,1},1,1},one}};
    expect(guarded.setLayerName("paint-layer",std::string("\xff",1)).code==DocumentHistoryCode::InvalidArgument &&
           guarded.setLayerOpacity("paint-layer",-0.01).code==DocumentHistoryCode::InvalidArgument &&
           guarded.setLayerTransform("paint-layer",nanTransform).code==DocumentHistoryCode::InvalidArgument &&
           guarded.setLayerBlendMode("paint-layer",RasterBlendMode::DestinationOut).code==DocumentHistoryCode::InvalidArgument &&
           guarded.moveLayer("paint-layer",2).code==DocumentHistoryCode::InvalidArgument &&
           guarded.replaceBitmapPatches("paint",badOverlap).code==DocumentHistoryCode::InvalidArgument &&
           guarded.statistics().redoEntries==saved.redoEntries && invalid.authorship.revision()==rev,
           "invalid: malformed edits reject without mutating state or redo");
}
void chunks() {
    Document d; d.extent={128,128}; d.canvasMode=CanvasMode::Infinite;
    d.infiniteCanvas.origin={-64,-64}; d.infiniteCanvas.chunkSize=32;
    d.assets.emplace_back(ChunkedRasterAsset{"paint",{{-1,-1,makeRasterLayer(32,32)}}});
    d.layers.emplace_back(StaticBitmapLayer{{"paint-layer","Paint"},StaticSource{"paint"}});
    DocumentHistory h(d); const std::array<std::uint32_t,4> v{0x00123456,0xff223344,0xff334455,0xff445566};
    const std::array p{BitmapPatch{{{-33,-33},2,2},v}};
    expect(h.replaceBitmapPatches("paint",p).changed &&
           findChunkedRasterAsset(d,"paint")->chunks.size()==4, "chunks: crossing allocates");
    expect(h.setLayerName("paint-layer","Ink").changed && h.undo().changed && h.undo().changed,
           "chunks: mixed commands undo");
    expect(findChunkedRasterAsset(d,"paint")->chunks.size()==1 &&
           properties(d,"paint-layer").name=="Paint", "chunks: original empty chunk restored");
    expect(h.redo().changed && h.redo().changed &&
           findChunkedRasterAsset(d,"paint")->chunks.size()==4, "chunks: mixed commands redo");
}
void fileHistory() {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    expect(temporary.isValid(), "file: temp directory");
    const auto path=(temporary.path()/"history.iisc").string();
    auto d=finite(128); DocumentFile file;
    expect(file.create(path,d).ok(), "file: create"); d={};
    DocumentHistory h(file); const std::array<std::uint32_t,1> v{0xff123456};
    const std::array p{BitmapPatch{{{5,6},1,1},v}};
    expect(h.replaceBitmapPatches("paint",p).changed && h.setLayerName("paint-layer","Ink").changed &&
           file.revision()==2, "file: mixed commands commit separately");
    expect(h.undo().changed && h.undo().changed && file.revision()==4, "file: ordered durable undo");
    expect(h.redo().changed && h.redo().changed && file.revision()==6, "file: ordered durable redo");
    Document snapshot; expect(DocumentFile::readSnapshot(path,snapshot).ok() &&
           pixel(snapshot,"paint",5,6)==v[0] && properties(snapshot,"paint-layer").name=="Ink",
           "file: reopened state");
    DocumentEditor other(file); expect(other.setLayerVisible("back-layer",false).changed, "file: outside edit");
    expect(h.undo().code==DocumentHistoryCode::Conflict && h.statistics().undoEntries==2,
           "file: outside revision preserves cursor");
    expect(h.reset().ok() && !h.canUndo(), "file: explicit rebase");
    DocumentFile stale;
    expect(stale.open(path).ok(), "file: open second binding");
    DocumentHistory staleHistory(stale);
    expect(other.setLayerOpacity("paint-layer",0.75).changed, "file: change through first binding");
    expect(staleHistory.setLayerName("paint-layer","Ink").code==DocumentHistoryCode::Conflict &&
           staleHistory.statistics().undoEntries==0,
           "file: stale no-op checks external write and preserves cursor");
    file.close(); expect(!h.isBound() && h.undo().code==DocumentHistoryCode::NotBound,
                        "file: close invalidates history");
}
void large() {
    constexpr int side=8192; auto d=finite(side);
    // A referenced mask is currently limited by Document validation to 16M
    // pixels. Keep three full 8K assets while exercising a valid 4K mask.
    findRasterAsset(d,"mask")->pixels=makeRasterLayer(4096,4096);
    d.assets.emplace_back(RasterAsset{"spare",makeRasterLayer(side,side)});
    std::uint32_t random=1;
    for (auto &asset:d.assets) for (auto &p:std::get<RasterAsset>(asset).pixels.pixels) {
        random^=random<<13; random^=random>>17; random^=random<<5; p=random;
    }
    DocumentHistoryLimits limits; limits.maximumBytes=1024*1024; limits.maximumEntries=128;
    DocumentHistory h(d,limits); expect(h.isBound(), "8K: valid mixed document");
    std::vector<std::uint32_t> values(64*16,0xffabcdef);
    const auto started=std::chrono::steady_clock::now(); int commands=0;
    for(int i=0;i<48;++i) {
        const std::array p{BitmapPatch{{{2048+(i%8)*64,2048+(i/8)*16},64,16},values}};
        expect(h.replaceBitmapPatches(i%2?"mask":"paint",p).changed, "8K: pixel command"); ++commands;
        if(i%12==0) { expect(h.setLayerName("paint-layer","Pass "+std::to_string(i)).changed,
                             "8K: metadata command"); ++commands; }
    }
    const auto retained=h.statistics().retainedBytes;
    expect(retained<limits.maximumBytes && h.statistics().undoEntries==std::size_t(commands),
           "8K: global history within budget");
    for(int i=0;i<commands;++i) expect(h.undo().changed, "8K: global undo");
    expect(properties(d,"paint-layer").name=="Paint", "8K: metadata restored");
    random=1; bool restored=true; std::uint64_t checked=0;
    for (const auto &asset:d.assets) for (auto p:std::get<RasterAsset>(asset).pixels.pixels) {
        random^=random<<13; random^=random>>17; random^=random<<5; restored=(p==random)&&restored; ++checked;
    }
    expect(restored, "8K: all source pixels restored");
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-started).count();
    std::cout << "GLOBAL_HISTORY_8K source_bytes=" << 3ULL*side*side*4+4096ULL*4096*4 << " commands=" << commands
              << " retained_bytes=" << retained << " checked_pixels=" << checked
              << " restored=" << restored << " total_ms=" << elapsed << '\n';
}
}
int main(int argc,char **argv) {
    if (argc==2 && std::string_view(argv[1])=="--large") large();
    else { orderedHistory(); budgetsAndConflicts(); chunks(); fileHistory(); }
    return failures ? 1 : 0;
}
