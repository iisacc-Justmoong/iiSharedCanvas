#include <Document/DocumentEditor.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderNative.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <tuple>
#include <vector>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, std::string_view message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
Document fixture(bool chunked, int side=128) {
    Document d; d.extent={side,side};
    if (chunked) {
        d.canvasMode=CanvasMode::Infinite; d.infiniteCanvas.origin={-64,-64};
        d.infiniteCanvas.chunkSize=32;
        d.assets.emplace_back(ChunkedRasterAsset{"paint",{{-1,-1,makeRasterLayer(32,32)}}});
    } else d.assets.emplace_back(RasterAsset{"paint",makeRasterLayer(side,side)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer","Paint"},StaticSource{"paint"}});
    return d;
}
std::uint32_t at(const Document &d,int x,int y) {
    if (const auto *r=findRasterAsset(d,"paint"))
        return r->pixels.pixels[std::size_t(y)*r->pixels.width+x];
    for (const auto &c:findChunkedRasterAsset(d,"paint")->chunks) {
        const int left=c.column*32, top=c.row*32;
        if (x>=left && x<left+32 && y>=top && y<top+32)
            return c.pixels.pixels[std::size_t(y-top)*32+x-left];
    }
    return 0;
}
std::size_t chunks(const Document &d) {
    const auto *r=findChunkedRasterAsset(d,"paint"); return r?r->chunks.size():0;
}
void memory(bool chunked) {
    auto d=fixture(chunked); DocumentHistory h(d);
    expect(h.setLayerName("layer","Ink").changed && h.undo().changed,
           "preview: establish redo before session");
    const auto before=h.statistics(); const auto initial=d.authorship.revision();
    expect(h.beginPixelPreview("paint","Whole gesture").ok() && h.pixelPreviewActive() &&
           h.previewStorageBytes()==0 && h.previewPendingHistoryBytes()==0 &&
           d.authorship.revision()==initial, "preview: begin owns cursor without changing document");
    expect(!h.canUndo() && !h.canRedo() &&
           h.beginPixelPreview("paint").code==DocumentHistoryCode::PreviewActive &&
           h.setLayerName("layer","Other").code==DocumentHistoryCode::PreviewActive &&
           h.replaceBitmapPatches("paint",{}).code==DocumentHistoryCode::PreviewActive &&
           h.undo().code==DocumentHistoryCode::PreviewActive &&
           h.redo().code==DocumentHistoryCode::PreviewActive &&
           h.reset().code==DocumentHistoryCode::PreviewActive,
           "preview: unrelated mutations cannot interleave");
    const int p=chunked?-1:1;
    const std::array<std::uint32_t,4> colors{0x00123456,0x80335577,0xff445566,0xff778899};
    const std::array<std::uint32_t,1> final{0xff123456};
    const std::array first{BitmapPatch{{{p,p},2,2},colors}},
                     second{BitmapPatch{{{3,3},1,1},final}};
    auto change=h.replacePixelPreview(first);
    expect(change.changed && at(d,p,p)==colors[0] && h.previewStorageBytes()>0 &&
           h.previewPendingHistoryBytes()>0 && h.pixelPreviewSourceAt(p,p)==0 &&
           h.statistics().redoEntries==before.redoEntries,
           "preview: visible pixels change but previous redo is retained");
    auto dirty=h.previewDirtyBounds();
    expect(dirty.width>0 && dirty.height>0,"preview: dirty region is exposed");
    h.clearPreviewDirtyBounds();
    expect(h.previewDirtyBounds().width==0,"preview: dirty region can be consumed");
    const auto revision=d.authorship.revision();
    const auto repeated=h.replacePixelPreview(first);
    expect(repeated.ok() && !repeated.changed && repeated.entryBytes>0 && d.authorship.revision()==revision,
           "preview: identical result is a document no-op");
    expect(h.replacePixelPreview(second).changed && at(d,p,p)==0 && at(d,3,3)==final[0] &&
           h.pixelPreviewSourceAt(3,3)==0, "preview: replacement restores omitted pixels");
    const auto stable=encodeIisc(d).bytes; const auto saved=h.previewPendingHistoryBytes();
    expect(h.replacePixelPreview(std::array{second[0],second[0]}).code==DocumentHistoryCode::InvalidArgument &&
           encodeIisc(d).bytes==stable && h.previewPendingHistoryBytes()==saved && h.pixelPreviewActive(),
           "preview: invalid replacement preserves visible result and pending command");
    const auto committed=h.commitPixelPreview();
    expect(committed.changed && committed.discardedRedoEntries==1 && !h.pixelPreviewActive() &&
           h.statistics().undoEntries==1 && !h.canRedo(),"preview: final result publishes one branch command");
    expect(h.setLayerOpacity("layer",0.5).changed && h.undo().changed && h.undo().changed &&
           at(d,3,3)==0 && (chunked?chunks(d)==1:true),"preview: mixed undo restores source and chunks");
    expect(h.redo().changed && h.redo().changed && at(d,3,3)==final[0],
           "preview: mixed redo restores the final pixels");
    expect(h.undo().changed && h.undo().changed && h.beginPixelPreview("paint").ok() &&
           h.replacePixelPreview(first).changed, "preview: begin cancellation fixture");
    h.clearPreviewDirtyBounds();
    expect(h.cancelPixelPreview().changed && h.canRedo() && h.previewDirtyBounds().width>0 &&
           at(d,p,p)==0 && (chunked?chunks(d)==1:true),
           "preview: cancel restores source, reports dirty region, and preserves redo");
    expect(h.beginPixelPreview("paint").ok() && h.replacePixelPreview(first).changed &&
           h.replacePixelPreview({}).changed && h.previewPendingHistoryBytes()==0 &&
           !h.commitPixelPreview().changed && h.canRedo(),
           "preview: empty final result keeps redo");
    expect(h.commitPixelPreview().code==DocumentHistoryCode::NoPreview &&
           !h.pixelPreviewSourceAt(3,3),"preview: absent session is explicit");
}
void limitsAndConflicts() {
    auto d=fixture(false); DocumentHistoryLimits limits; limits.maximumBytes=1; limits.maximumEntries=0;
    DocumentHistory tiny(d,limits); const std::array<std::uint32_t,1> value{0xff123456};
    const std::array patch{BitmapPatch{{{1,1},1,1},value}};
    expect(tiny.beginPixelPreview("paint").ok() &&
           tiny.replacePixelPreview(patch).code==DocumentHistoryCode::BudgetExceeded &&
           tiny.pixelPreviewActive() && at(d,1,1)==0 && tiny.cancelPixelPreview().ok(),
           "budget: visible preview is rejected before mutation and can be cancelled");

    auto e=fixture(false); DocumentHistoryLimits eviction; eviction.maximumEntries=2;
    eviction.overflow=DocumentHistoryOverflow::EvictOldest; DocumentHistory evict(e,eviction);
    expect(evict.setLayerName("layer","A").changed && evict.setLayerVisible("layer",false).changed &&
           evict.beginPixelPreview("paint").ok() && evict.replacePixelPreview(patch).changed &&
           evict.statistics().undoEntries==2,"eviction: preview does not remove prior entries");
    const auto published=evict.commitPixelPreview();
    expect(published.changed && published.evictedEntries==1 && evict.statistics().undoEntries==2 &&
           evict.undo().changed && evict.undo().changed &&
           layerProperties(*findLayer(e,"layer")).name=="A" &&
           layerProperties(*findLayer(e,"layer")).visible,
           "eviction: commit reports and removes the oldest entry exactly once");

    auto c=fixture(false); DocumentHistory h(c);
    expect(h.setLayerName("layer","Ink").changed && h.beginPixelPreview("paint").ok() &&
           h.replacePixelPreview(patch).changed,"conflict: fixture");
    findRasterAsset(c,"paint")->pixels.pixels[129]=0xffabcdef;
    expect(h.commitPixelPreview().code==DocumentHistoryCode::Conflict &&
           h.cancelPixelPreview().code==DocumentHistoryCode::Conflict && h.pixelPreviewActive() &&
           at(c,1,1)==0xffabcdef && h.statistics().undoEntries==1,
           "conflict: changed target is never overwritten and remains resolvable");
    findRasterAsset(c,"paint")->pixels.pixels[129]=value[0];
    expect(h.cancelPixelPreview().changed && h.canUndo(),"conflict: restored target can be cancelled");
    expect(h.beginPixelPreview("paint").ok() && h.replacePixelPreview(patch).changed,
           "conflict: start new preview");
    DocumentEditor other(c); expect(other.setLayerVisible("layer",false).changed,"conflict: external metadata edit");
    expect(h.cancelPixelPreview().changed && at(c,1,1)==0 &&
           h.undo().code==DocumentHistoryCode::Conflict && h.statistics().undoEntries==1 &&
           h.reset().ok() && !h.canUndo(),
           "conflict: safe cancel keeps external change and history requires explicit rebase");
    expect(h.setLayerName("layer","Again").changed && h.beginPixelPreview("paint").ok() &&
           h.replacePixelPreview(patch).changed,"abandon: fixture");
    findRasterAsset(c,"paint")->pixels.pixels[129]=0xffabcdef;
    expect(h.cancelPixelPreview().code==DocumentHistoryCode::Conflict &&
           h.abandonPixelPreview().ok() && !h.pixelPreviewActive() && !h.canUndo() &&
           at(c,1,1)==0xffabcdef && h.statistics().retainedBytes==0,
           "abandon: explicit recovery keeps current pixels and discards the whole history");
}
void file(bool chunked) {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory temporary(parent);
    expect(temporary.isValid(),"file: temporary directory");
    const auto path=(temporary.path()/"preview.iisc").string();
    auto d=fixture(chunked); DocumentFile owner;
    expect(owner.create(path,d).ok(),"file: create");
    DocumentHistory h(owner); const int p=chunked?-1:1;
    const std::array<std::uint32_t,1> value{0x80112233};
    const std::array patch{BitmapPatch{{{p,p},1,1},value}};
    expect(h.setLayerName("layer","Ink").changed && h.beginPixelPreview("paint").ok(),"file: begin");
    const auto rev=owner.revision();
    expect(h.replacePixelPreview(patch).changed && owner.revision()==rev+1,"file: preview is durable");
    Document snapshot; expect(DocumentFile::readSnapshot(path,snapshot).ok() && at(snapshot,p,p)==value[0],
                              "file: another snapshot sees visible preview");
    const auto beforeCommit=owner.revision();
    expect(h.commitPixelPreview().changed && owner.revision()==beforeCommit &&
           h.undo().changed && at(*owner.document(),p,p)==0 &&
           h.undo().changed && layerProperties(*findLayer(*owner.document(),"layer")).name=="Paint" &&
           h.redo().changed && h.redo().changed && at(*owner.document(),p,p)==value[0],
           "file: commit adds one command and mixed undo/redo persist");
    expect(h.beginPixelPreview("paint").ok(),"file: begin cancel");
    const std::array<std::uint32_t,1> replacement{0xffabcdef};
    const std::array changed{BitmapPatch{{{p,p},1,1},replacement}};
    expect(h.replacePixelPreview(changed).changed && h.cancelPixelPreview().changed &&
           DocumentFile::readSnapshot(path,snapshot).ok() && at(snapshot,p,p)==value[0],
           "file: cancellation is durable");
    expect(h.beginPixelPreview("paint").ok() && h.replacePixelPreview(changed).changed,
           "file: conflict fixture");
    DocumentFile second; expect(second.open(path).ok(),"file: second handle");
    DocumentEditor external(second); expect(external.setLayerVisible("layer",false).changed,
                                            "file: external writer");
    expect(h.cancelPixelPreview().code==DocumentHistoryCode::Conflict && h.pixelPreviewActive(),
           "file: conflicting cancel preserves pending session");
    expect(h.abandonPixelPreview().ok() && !h.pixelPreviewActive() && !h.isBound() &&
           h.statistics().retainedBytes==0,"file: explicit abandon releases invalid binding");
}
void large() {
    constexpr int side=8192; auto d=fixture(false,side);
    d.assets.emplace_back(RasterAsset{"second",makeRasterLayer(side,side)});
    d.assets.emplace_back(RasterAsset{"third",makeRasterLayer(side,side)});
    d.assets.emplace_back(RasterAsset{"mask",makeRasterLayer(4096,4096)});
    layerProperties(d.layers.front()).appearance.rasterMask=LayerMask{.assetId="mask"};
    std::uint32_t random=1;
    for (auto &asset:d.assets) for (auto &p:std::get<RasterAsset>(asset).pixels.pixels) {
        random^=random<<13; random^=random>>17; random^=random<<5; p=random;
    }
    DocumentHistoryLimits limits; limits.maximumBytes=1024*1024;
    limits.maximumBitmapInputBytes=8*1024*1024; DocumentHistory h(d,limits);
    expect(h.isBound() && h.setLayerName("layer","Ink").changed && h.beginPixelPreview("paint").ok(),
           "8K preview: begin");
    std::vector<std::uint32_t> values(64*16,0xff123456);
    for (int i=0;i<12;++i) {
        const std::array p{BitmapPatch{{{2048+i*64,2048},64,16},values}};
        expect(h.replacePixelPreview(p).changed,"8K preview: replace final tile");
    }
    const auto pending=h.previewPendingHistoryBytes(), cached=h.previewStorageBytes();
    expect(pending<limits.maximumBytes && cached>0 && h.commitPixelPreview().changed &&
           h.undo().changed && h.undo().changed,"8K preview: commit and mixed undo");
    random=1; bool restored=true; std::uint64_t checked=0;
    for (const auto &asset:d.assets) for (auto p:std::get<RasterAsset>(asset).pixels.pixels) {
        random^=random<<13; random^=random>>17; random^=random<<5;
        restored=(p==random)&&restored; ++checked;
    }
    expect(restored,"8K preview: all source pixels restored");
    std::cout << "DOCUMENT_PREVIEW_8K source_bytes=" << 3ULL*side*side*4+4096ULL*4096*4
              << " replacements=12 pending_bytes=" << pending << " cache_bytes=" << cached
              << " checked_pixels=" << checked << " restored=" << restored << '\n';
}
}
int main(int argc,char **argv) {
    if (argc==2 && std::string_view(argv[1])=="--large") large();
    else { memory(false); memory(true); limitsAndConflicts(); file(false); file(true); }
    return failures?1:0;
}
