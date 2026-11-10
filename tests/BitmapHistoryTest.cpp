#include <Bitmap/BitmapEdit.h>
#include <Bitmap/BitmapEditor.h>
#include <Bitmap/ChunkedBitmapEditor.h>
#include <Document/DocumentEditor.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderNative.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace
{
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool value, const char *message)
{
    if (!value)
    {
        ++failures;
        std::cerr << message << '\n';
    }
}
Document finite(int width = 256, int height = 132)
{
    Document d;
    d.extent = {width, height};
    d.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(width, height)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return d;
}
Document infinite()
{
    Document d;
    d.extent = {260, 132};
    d.canvasMode = CanvasMode::Infinite;
    d.infiniteCanvas.chunkSize = 128;
    d.infiniteCanvas.origin = {-130, -1};
    d.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return d;
}
auto bytes(const Document &d) { return encodeIisc(d).bytes; }
void finitePatches()
{
    auto d = finite();
    BitmapEditor e(d, "paint");
    const std::vector<std::uint32_t> a(12, 0xff112233), b(6, 0x80445566);
    const std::array patches{BitmapPatch{{{62, 1}, 4, 3}, a}, BitmapPatch{{{127, 65}, 3, 2}, b}};
    const auto before = bytes(d);
    const auto author = d.authorship.revision();
    expect(e.replacePatches(patches), "finite: one atomic patch batch");
    expect(e.revision() == 1 && d.authorship.revision() == author + 1,
           "finite: one edit/authorship revision per batch");
    expect(e.pixelAt(62, 1) == a[0] && e.pixelAt(129, 66) == b[0],
           "finite: both patch regions published");
    expect(e.historyStatistics().undoEntries == 1 && e.historyStatistics().retainedBytes < 16384,
           "finite: region history rather than asset snapshots");
    expect(e.undo() && e.pixelAt(62, 1) == 0 && e.pixelAt(129, 66) == 0,
           "finite: one undo restores all regions");
    const auto revision = e.revision();
    const auto retained = e.historyStatistics().retainedBytes;
    std::vector<std::uint32_t> zero(12);
    expect(e.replacePatches(std::array{BitmapPatch{{{62, 1}, 4, 3}, zero}}),
           "finite: no-op succeeds");
    expect(e.revision() == revision && e.canRedo() &&
               e.historyStatistics().retainedBytes == retained,
           "finite: no-op preserves redo and revision");
    expect(e.redo() && e.pixelAt(129, 66) == b[0], "finite: redo restores batch");
    const auto stable = bytes(d);
    const auto cursor = e.historyStatistics();
    expect(!e.replacePatches(std::array{patches[0], BitmapPatch{{{255, 131}, 3, 2}, b}}),
           "finite: invalid second patch rejects entire batch");
    expect(bytes(d) == stable && e.historyStatistics().undoEntries == cursor.undoEntries,
           "finite: rejected batch preserves document/history");
    expect(!e.replacePatches(std::array{patches[0], patches[0]}) &&
               e.lastHistoryResult().code == RasterHistoryCode::InvalidArgument,
           "finite: overlaps rejected");
    auto copy = e;
    expect(e.undo(), "finite: original cursor can undo");
    expect(!copy.undo() && copy.lastHistoryResult().code == RasterHistoryCode::Conflict,
           "finite: shared source conflict preserves copied cursor");
    expect(copy.historyStatistics().undoEntries == 1, "finite: COW cursor remains independent");
    (void)before;
}
void budgets()
{
    auto d = finite();
    RasterHistoryLimits limits;
    limits.maximumBytes = 4096;
    limits.maximumEntries = 1;
    BitmapEditor e(d, "paint", limits);
    expect(e.setPixel(1, 1, 0xff000001), "budget: initial edit");
    const auto stable = bytes(d);
    const auto revision = e.revision();
    expect(!e.setPixel(2, 1, 0xff000002) &&
               e.lastHistoryResult().code == RasterHistoryCode::BudgetExceeded,
           "budget: reject before second edit");
    expect(bytes(d) == stable && e.revision() == revision && e.historyStatistics().undoEntries == 1,
           "budget: no silent eviction or mutation");
    expect(e.setPixel(1, 1, 0xff000001) && e.revision() == revision,
           "budget: no-op accepted at full history");
    auto other = finite();
    limits.overflow = RasterHistoryOverflow::EvictOldest;
    BitmapEditor evict(other, "paint", limits);
    expect(evict.setPixel(1, 1, 0xff000001) && evict.setPixel(2, 1, 0xff000002),
           "budget: explicit eviction succeeds");
    expect(evict.lastHistoryResult().evictedEntries == 1, "budget: eviction is reported");
    expect(evict.undo() && evict.pixelAt(1, 1) == 0xff000001 && evict.pixelAt(2, 1) == 0,
           "budget: only retained command undone");
    const auto original = bytes(other);
    std::vector<std::uint32_t> huge(256 * 132, 0xffabcdef);
    expect(!evict.replacePatch({{0, 0}, 256, 132}, huge), "budget: oversized command rejected");
    expect(bytes(other) == original && evict.canRedo(),
           "budget: oversized command preserves prior redo");
}
void resize()
{
    auto d = finite(16, 12);
    auto *p = findRasterAsset(d, "paint");
    for (std::size_t i = 0; i < p->pixels.pixels.size(); ++i)
        p->pixels.pixels[i] = 0xff000000 + unsigned(i);
    const auto original = p->pixels;
    BitmapEditor e(d, "paint");
    auto next = makeRasterLayer(8, 20, 0x80112233);
    expect(e.replacePixels(next) && e.width() == 8 && e.height() == 20,
           "resize: extent and pixels apply atomically");
    expect(e.undo() && e.width() == 16 && e.height() == 12 && e.pixels()->pixels == original.pixels,
           "resize: undo restores removed rows/columns");
    expect(e.redo() && e.pixels()->pixels == next.pixels, "resize: redo exact");
}
void chunks()
{
    auto d = infinite();
    ChunkedBitmapEditor e(d, "paint");
    const std::vector<std::uint32_t> a(12, 0xff112233), b(1, 0x80445566);
    const std::array patches{BitmapPatch{{{-129, -1}, 4, 3}, a},
                             BitmapPatch{{{128, 128}, 1, 1}, b}};
    expect(e.replacePatches(patches), "chunks: negative origin and chunk boundaries");
    auto *asset = findChunkedRasterAsset(d, "paint");
    expect(asset->chunks.size() == 5 && e.revision() == 1,
           "chunks: exact sparse allocation and one revision");
    const auto painted = bytes(d);
    expect(e.undo() && asset->chunks.empty(), "chunks: undo removes chunks absent before command");
    expect(e.redo() && e.pixelAt(-129, -1) == a[0] && e.pixelAt(128, 128) == b[0],
           "chunks: redo creates exact pixels");
    expect(e.clear() && asset->chunks.empty(), "chunks: clear removes allocation");
    expect(e.undo() && e.pixelAt(128, 128) == b[0] && asset->chunks.size() == 5,
           "chunks: clear undo restores allocation");
    asset->chunks.push_back({0, 0, makeRasterLayer(128, 128)});
    std::ranges::sort(asset->chunks, [](const auto &x, const auto &y)
                      { return std::pair{x.row, x.column} < std::pair{y.row, y.column}; });
    expect(e.bind(d, "paint") && e.clear() && e.undo() && asset->chunks.size() == 6,
           "chunks: all-zero allocated chunk survives clear undo");
    auto replacement = makeRasterLayer(260, 132, 0);
    replacement.pixels[0] = 0xff778899;
    expect(e.replaceRegion({-130, -1}, replacement),
           "chunks: whole allocated-region replacement uses journal");
    expect(e.undo() && asset->chunks.size() == 6,
           "chunks: replacement undo restores exact chunk presence");
    (void)painted;
}
void chunkAssetCoordinates()
{
    auto document = infinite();
    const auto origin = document.infiniteCanvas.origin;
    const auto extent = document.extent;
    ChunkedBitmapEditor editor(document, "paint");
    const std::array<std::uint32_t, 2> values{0xff123456U, 0xffabcdefU};
    const BitmapPatch outside{{{300, -200}, 2, 1}, values};
    expect(!editor.replacePatches(std::array{outside}), "asset coordinates: canvas-clipped API rejects outside region");
    expect(editor.replaceAssetPatches(std::array{outside}), "asset coordinates: explicit local patch succeeds");
    expect(!editor.pixelAt(300, -200) && editor.assetPixelAt(300, -200) == values[0] && editor.assetPixelAt(301, -200) == values[1],
           "asset coordinates: local sampling is distinct from canvas bounds");
    expect(document.infiniteCanvas.origin == origin && document.extent == extent,
           "asset coordinates: local edits do not grow the canvas");
    expect(editor.undo() && editor.assetPixelAt(300, -200) == 0 && editor.canRedo(), "asset coordinates: undo removes allocation");
    const auto revision = editor.revision();
    const BitmapPatch invalid{{{-1000000001, 0}, 1, 1}, std::span(values).first(1)};
    expect(!editor.replaceAssetPatches(std::array{invalid}) && editor.revision() == revision && editor.canRedo(),
           "asset coordinates: invalid bounds preserve redo");
    expect(editor.redo() && editor.assetPixelAt(301, -200) == values[1], "asset coordinates: redo restores pixels");
    const auto unchanged = bytes(document); const auto unchangedRevision = editor.revision();
    expect(!editor.replaceAssetPatches(std::array{outside, outside}) && editor.revision() == unchangedRevision && bytes(document) == unchanged,
           "asset coordinates: overlapping patches fail atomically");
    const std::array<std::uint32_t, 2> zeros{};
    expect(editor.undo() && editor.canRedo() && editor.replaceAssetPatches(std::array{BitmapPatch{{{300,-200},2,1},zeros}}) && editor.canRedo(),
           "asset coordinates: unchanged transparent patch preserves redo");
    expect(editor.redo(), "asset coordinates: redo after unchanged patch");
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false, "asset coordinates: temporary directory"); return; }
    const auto path = temporary.path()/"asset-coordinates.iisc";
    DocumentFile file;
    expect(file.create(path.string(), infinite()).ok(), "asset coordinates: create file");
    ChunkedBitmapEditor durable(file, "paint");
    expect(durable.replaceAssetPatches(std::array{outside}) && durable.undo() && durable.redo(),
           "asset coordinates: durable local edit history");
    file.close();
    DocumentFile reopened;
    expect(reopened.open(path.string()).ok(), "asset coordinates: reopen file");
    ChunkedBitmapEditor read(reopened, "paint");
    expect(read.assetPixelAt(300,-200) == values[0] && !read.pixelAt(300,-200) && reopened.document()->infiniteCanvas.origin == origin && reopened.document()->extent == extent,
           "asset coordinates: durable pixels and canvas extent");
}
void chunkAssetStroke()
{
    auto document = infinite();const auto origin=document.infiniteCanvas.origin;const auto extent=document.extent;
    ChunkedBitmapEditor editor(document,"paint");BitmapBrush brush;brush.size=4;brush.argb=0xff123456U;brush.pressureToOpacityEnabled=false;
    StrokePoint first;first.position={304,-196};first.time=0;StrokePoint last=first;last.position={308,-196};last.time=.1;
    const CanvasRegion region{{300,-200},{16,16}};
    expect(editor.setBrush(brush) && editor.beginAssetStrokeInput(first,region) && editor.endStrokeInput(last),"asset stroke: paint local region outside canvas");
    expect(editor.assetPixelAt(304,-196).value_or(0)!=0 && !editor.pixelAt(304,-196) && document.infiniteCanvas.origin==origin && document.extent==extent,"asset stroke: local pixels without canvas growth");
    expect(editor.undo() && editor.assetPixelAt(304,-196)==0 && editor.canRedo(),"asset stroke: undo restores sparse chunks");
    const CanvasRegion invalid{{999999999,0},{2,1}};const auto revision=editor.revision();
    expect(!editor.beginAssetStrokeInput(first,invalid) && editor.revision()==revision && editor.canRedo(),"asset stroke: invalid region preserves redo");
    expect(editor.beginAssetStrokeInput(first,region),"asset stroke: begin cancel");editor.cancelStroke();
    expect(editor.assetPixelAt(304,-196)==0 && editor.canRedo() && editor.redo(),"asset stroke: cancel preserves old redo");
    auto clippedDoc=infinite();ChunkedBitmapEditor clipped(clippedDoc,"paint");auto wide=brush;wide.size=12;
    StrokePoint edge=first;edge.position={300,-196};
    expect(clipped.setBrush(wide)&&clipped.beginAssetStrokeInput(edge,region)&&clipped.endStrokeInput(edge),"asset stroke: edge dab");
    expect(clipped.assetPixelAt(299,-196)==0 && clipped.assetPixelAt(300,-196).value_or(0)!=0,"asset stroke: local clip bounds enforced");
    RasterHistoryLimits tiny;tiny.maximumBytes=1;auto boundedDoc=infinite();ChunkedBitmapEditor bounded(boundedDoc,"paint",tiny);const auto boundedBefore=bytes(boundedDoc);
    expect(bounded.setBrush(brush)&&!bounded.beginAssetStrokeInput(first,region)&&bytes(boundedDoc)==boundedBefore,"asset stroke: history budget rejects without mutation");
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);iiFileProvider::native::TemporaryDirectory temporary(parent);
    if(!temporary.isValid()){expect(false,"asset stroke: temporary directory");return;}
    const auto path=temporary.path()/"asset-stroke.iisc";DocumentFile file;expect(file.create(path.string(),infinite()).ok(),"asset stroke: create file");
    ChunkedBitmapEditor durable(file,"paint");expect(durable.setBrush(brush)&&durable.beginAssetStrokeInput(first,region)&&durable.endStrokeInput(last)&&durable.undo()&&durable.redo(),"asset stroke: durable edit history");
    file.close();DocumentFile reopened;expect(reopened.open(path.string()).ok(),"asset stroke: reopen file");ChunkedBitmapEditor read(reopened,"paint");
    expect(read.assetPixelAt(304,-196).value_or(0)!=0 && !read.pixelAt(304,-196) && reopened.document()->infiniteCanvas.origin==origin && reopened.document()->extent==extent,"asset stroke: persistence and canvas extent");
}
void sparseDirtyBounds()
{
    auto d=infinite();d.infiniteCanvas.origin={-4096,-4096};d.extent={9000,9000};
    ChunkedBitmapEditor editor(d,"paint");BitmapBrush brush;brush.size=4;brush.argb=0xff123456U;brush.pressureToOpacityEnabled=false;
    const auto bounded=[](DevicePixelRect r,int x,int y){return r.width>0 && r.height>0 && r.width<=128 && r.height<=128 && r.origin.x<=x && r.origin.y<=y && std::int64_t(r.origin.x)+r.width>x && std::int64_t(r.origin.y)+r.height>y;};
    expect(editor.setBrush(brush)&&editor.beginStroke({-250,10})&&editor.endStroke({-250,10}),"sparse dirty: stroke");
    expect(bounded(editor.dirtyBounds(),-250,10),"sparse dirty: created chunk must not dirty the entire canvas");
    editor.clearDirtyBounds();expect(editor.undo()&&bounded(editor.dirtyBounds(),-250,10),"sparse dirty: removed chunk bounds on undo");
    editor.clearDirtyBounds();expect(editor.redo()&&bounded(editor.dirtyBounds(),-250,10),"sparse dirty: recreated chunk bounds on redo");
    auto local=infinite();ChunkedBitmapEditor asset(local,"paint");StrokePoint point;point.position={304,-196};point.time=0;
    expect(asset.setBrush(brush)&&asset.beginAssetStrokeInput(point,{{300,-200},{16,16}})&&asset.endStrokeInput(point),"sparse dirty: asset-local stroke");
    expect(bounded(asset.dirtyBounds(),304,-196),"sparse dirty: asset coordinates outside canvas");
    asset.clearDirtyBounds();expect(asset.undo()&&bounded(asset.dirtyBounds(),304,-196),"sparse dirty: asset-local undo");
    auto metadata=infinite();findChunkedRasterAsset(metadata,"paint")->chunks.push_back({0,0,makeRasterLayer(128,128,0)});
    ChunkedBitmapEditor structure(metadata,"paint");expect(structure.clear()&&structure.revision()==1&&structure.dirtyBounds().width==0 && structure.dirtyBounds().height==0&&structure.canUndo(),"sparse dirty: empty chunk removal updates revision without pixel dirt");
    expect(structure.undo()&&structure.revision()==2&&structure.dirtyBounds().width==0 && structure.dirtyBounds().height==0&&structure.canRedo(),"sparse dirty: empty chunk restoration updates revision without pixel dirt");
}
void chunkStrokeGrowth()
{
    auto d=infinite();const auto origin=d.infiniteCanvas.origin;const auto extent=d.extent;ChunkedBitmapEditor editor(d,"paint");BitmapBrush brush;brush.size=4;brush.argb=0xff123456U;brush.pressureToOpacityEnabled=false;
    expect(editor.setBrush(brush)&&editor.setStrokeCanvasGrowth(true),"stroke growth: configure");StrokePoint first;first.position={120,20};first.time=1;StrokePoint last=first;last.position={200,20};last.time=2;
    expect(editor.beginStrokeInput(first)&&editor.endStrokeInput(last),"stroke growth: paint beyond canvas");const auto grownOrigin=d.infiniteCanvas.origin;const auto grownExtent=d.extent;const auto painted=editor.assetPixelAt(200,20).value_or(0);expect((grownOrigin!=origin||grownExtent!=extent)&&painted!=0,"stroke growth: pixels and geometry expand");
    expect(editor.undo()&&d.infiniteCanvas.origin==origin&&d.extent==extent&&editor.assetPixelAt(200,20)==0&&editor.canRedo(),"stroke growth: undo pixels and geometry");
    auto transparent=brush;transparent.opacity=0;StrokePoint outside=first;outside.position={500,20};expect(editor.setBrush(transparent)&&editor.beginStrokeInput(outside)&&editor.endStrokeInput(outside)&&d.extent==extent&&d.infiniteCanvas.origin==origin&&editor.canRedo(),"stroke growth: no-op preserves geometry and redo");
    expect(editor.setBrush(brush)&&editor.beginStrokeInput(outside),"stroke growth: start cancel");expect(!editor.setStrokeCanvasGrowth(false),"stroke growth: mode cannot change during a stroke");editor.cancelStroke();expect(d.extent==extent&&d.infiniteCanvas.origin==origin&&editor.canRedo(),"stroke growth: cancel preserves geometry and redo");
    expect(editor.redo()&&d.infiniteCanvas.origin==grownOrigin&&d.extent==grownExtent&&editor.assetPixelAt(200,20)==painted,"stroke growth: redo pixels and geometry");
    auto clippedDoc=infinite();ChunkedBitmapEditor clipped(clippedDoc,"paint");expect(clipped.setBrush(brush)&&clipped.setStrokeCanvasGrowth(false)&&clipped.beginStrokeInput(outside)&&clipped.endStrokeInput(outside)&&clippedDoc.extent==extent&&clippedDoc.infiniteCanvas.origin==origin,"stroke growth: clip mode stays within canvas");
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);iiFileProvider::native::TemporaryDirectory tmp(parent);if(!tmp.isValid()){expect(false,"stroke growth: temporary directory");return;}
    const auto path=tmp.path()/"growing.iisc";DocumentFile file;expect(file.create(path.string(),infinite()).ok(),"stroke growth: create file");ChunkedBitmapEditor durable(file,"paint");expect(durable.setBrush(brush)&&durable.setStrokeCanvasGrowth(true)&&durable.beginStrokeInput(first)&&durable.endStrokeInput(last)&&durable.undo()&&durable.redo(),"stroke growth: durable history");file.close();DocumentFile reopened;expect(reopened.open(path.string()).ok(),"stroke growth: reopen");ChunkedBitmapEditor read(reopened,"paint");expect(reopened.document()->extent==grownExtent&&reopened.document()->infiniteCanvas.origin==grownOrigin&&read.assetPixelAt(200,20)==painted,"stroke growth: durable pixels and geometry");
}
void preview()
{
    auto d = finite();
    RasterHistoryLimits limits;
    limits.maximumBytes = 4096;
    BitmapEditor e(d, "paint", limits);
    BitmapBrush b;
    b.size = 1;
    b.hardness = 1;
    b.pressureToOpacityEnabled = false;
    expect(e.setPixel(2, 2, 0xff123456) && e.undo(), "preview: establish redo before stroke");
    expect(e.setBrush(b) && e.beginStroke({4., 4.}), "preview: small stroke starts");
    const auto previewPixels = e.pixels()->pixels;
    const auto revision = e.revision();
    expect(!e.continueStroke({200., 4.}) &&
               e.lastHistoryResult().code == RasterHistoryCode::BudgetExceeded,
           "preview: growing command rejected before publish");
    expect(e.pixels()->pixels == previewPixels && e.revision() == revision && e.strokeActive(),
           "preview: rejection preserves previous preview and active session");
    expect(e.previewStorageBytes() > 0 && e.previewStorageBytes() < 1024 * 1024,
           "preview: sparse original/pending/final pixel storage");
    e.cancelStroke();
    expect(!e.strokeActive() && e.pixelAt(4, 4) == 0 && e.canRedo(),
           "preview: cancellation restores source and old redo");
    expect(e.redo() && e.pixelAt(2, 2) == 0xff123456, "preview: original redo retained");
    auto c = infinite();
    ChunkedBitmapEditor ce(c, "paint");
    expect(ce.setBrush(b) && ce.beginStroke({-128., 0.}) && ce.continueStroke({-120., 0.}),
           "preview: sparse chunk stroke");
    ce.cancelStroke();
    expect(findChunkedRasterAsset(c, "paint")->chunks.empty(),
           "preview: chunk cancellation removes new allocation");
    expect(ce.beginStroke({-128., 0.}) && ce.endStroke({-120., 0.}) &&
               ce.historyStatistics().undoEntries == 1,
           "preview: one stroke one command");
    expect(ce.undo() && findChunkedRasterAsset(c, "paint")->chunks.empty() && ce.redo(),
           "preview: chunk stroke reversible without full snapshot");
}
void file()
{
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false, "file: reserve test directory"); return; }
    const auto path = temporary.path() / "bitmap-history.iisc";
    auto d = finite();
    DocumentFile f;
    expect(f.create(path.string(), d).ok(), "file: create working document");
    BitmapEditor e(f, "paint");
    const std::array<std::uint32_t, 1> a{0xffaabbcc}, b{0x80112233};
    const auto revision = f.revision();
    expect(e.replacePatches(
               std::array{BitmapPatch{{{1, 1}, 1, 1}, a}, BitmapPatch{{{65, 65}, 1, 1}, b}}),
           "file: atomic multi-region commit");
    expect(f.revision() == revision + 1 && e.historyStatistics().undoEntries == 1,
           "file: one durable revision and command");
    const auto stable = encodeIisc(*f.document()).bytes;
    expect(!e.replacePatches(
               std::array{BitmapPatch{{{1, 1}, 1, 1}, b}, BitmapPatch{{{255, 131}, 2, 1}, a}}),
           "file: rejected batch rolls back");
    expect(f.revision() == revision + 1 && encodeIisc(*f.document()).bytes == stable,
           "file: failed batch preserves durable bytes/revision");
    expect(e.undo() && e.redo(), "file: durable undo and redo");
    f.close();
    expect(!e.setPixel(0, 0, 1), "file: closed binding rejected");
    DocumentFile reopened;
    expect(reopened.open(path.string()).ok(), "file: reopen");
    BitmapEditor read(reopened, "paint");
    expect(read.pixelAt(1, 1) == a[0] && read.pixelAt(65, 65) == b[0],
           "file: both patches survive reopen");
}
void removalConflicts()
{
    auto d = finite(4, 4);
    BitmapEditor e(d, "paint");
    expect(e.replacePixels(makeRasterLayer(8, 8)), "conflict: transparent extent expansion");
    findRasterAsset(d, "paint")->pixels.pixels[7 * 8 + 7] = 0xff123456;
    const auto stable = bytes(d);
    expect(!e.undo() && e.lastHistoryResult().code == RasterHistoryCode::Conflict,
           "conflict: undo cannot discard external pixels in removed extent");
    expect(bytes(d) == stable && e.historyStatistics().undoEntries == 1,
           "conflict: resize source/cursor preserved");
    auto c = infinite();
    ChunkedBitmapEditor ce(c, "paint");
    const std::array<std::uint32_t, 1> pixel{0xffaabbcc};
    expect(ce.replacePatches(std::array{BitmapPatch{{{-128, 0}, 1, 1}, pixel}}),
           "conflict: create chunk");
    auto *chunk = findRasterChunk(*findChunkedRasterAsset(c, "paint"), -1, 0);
    chunk->pixels.pixels[20 * 128 + 20] = 0xff445566;
    const auto chunkStable = bytes(c);
    expect(!ce.undo() && ce.lastHistoryResult().code == RasterHistoryCode::Conflict,
           "conflict: undo cannot remove externally changed chunk");
    expect(bytes(c) == chunkStable && ce.historyStatistics().undoEntries == 1,
           "conflict: chunk source/cursor preserved");
}

void aliasAndEmptyCancel()
{
    auto d = finite(2, 1);
    findRasterAsset(d, "paint")->pixels.pixels = {0xff112233, 0xff445566};
    BitmapEditor e(d, "paint");
    const auto &source = e.pixels()->pixels;
    const std::array patches{BitmapPatch{{{1, 0}, 1, 1}, std::span(source).first(1)},
                             BitmapPatch{{{0, 0}, 1, 1}, std::span(source).subspan(1, 1)}};
    expect(e.replacePatches(patches) && e.pixelAt(0, 0) == 0xff445566 &&
               e.pixelAt(1, 0) == 0xff112233,
           "alias: all borrowed inputs captured before the first write");
    expect(e.undo() && e.pixelAt(0, 0) == 0xff112233, "alias: swap undo");
    auto blank = finite();
    RasterHistoryLimits limits;
    limits.maximumBytes = 1;
    BitmapEditor empty(blank, "paint", limits);
    BitmapBrush brush;
    brush.size = 1;
    expect(empty.setBrush(brush) && empty.beginStroke({-5., -5.}), "empty cancel: no-op begin");
    expect(!empty.continueStroke({4., 4.}), "empty cancel: first visible preview denied");
    empty.cancelStroke();
    expect(!empty.strokeActive() && empty.historyStatistics().undoEntries == 0,
           "empty cancel: rejected no-op stroke still cancels");
    auto chunks = infinite();
    ChunkedBitmapEditor chunkEditor(chunks, "paint", limits);
    expect(chunkEditor.setBrush(brush) && chunkEditor.beginStroke({-140., -20.}),
           "empty cancel: chunk no-op begin");
    expect(!chunkEditor.continueStroke({-128., 0.}), "empty cancel: chunk preview denied");
    chunkEditor.cancelStroke();
    expect(!chunkEditor.strokeActive() && findChunkedRasterAsset(chunks, "paint")->chunks.empty(),
           "empty cancel: chunk stroke cancels without publication");
}

void fileFailure()
{
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false, "file failure: reserve test directory"); return; }
    const auto root = temporary.path() / "moved-binding";
    std::filesystem::create_directories(root / "original");
    DocumentFile f;
    expect(f.create((root / "original/work.iisc").string(), finite()).ok(), "file failure: create");
    BitmapEditor e(f, "paint");
    expect(e.setPixel(1, 1, 0xff123456), "file failure: initial command");
    const auto stable = encodeIisc(*f.document()).bytes;
    const auto revision = f.revision();
    const auto retained = e.historyStatistics().retainedBytes;
    std::filesystem::rename(root / "original", root / "moved");
    expect(!e.setPixel(2, 2, 0xffabcdef), "file failure: publication into moved database rejected");
    expect(f.revision() == revision && f.document() && encodeIisc(*f.document()).bytes == stable,
           "file failure: source and durable revision preserved");
    expect(e.historyStatistics().retainedBytes == retained &&
               e.historyStatistics().undoEntries == 1,
           "file failure: prepared cursor not installed");
    f.close();
    DocumentFile moved;
    expect(moved.open((root / "moved/work.iisc").string()).ok(),
           "file failure: moved database reopen");
    BitmapEditor read(moved, "paint");
    expect(read.pixelAt(1, 1) == 0xff123456 && read.pixelAt(2, 2) == 0,
           "file failure: rejected pixels not persisted");
}
void large()
{
    auto d = finite(8192, 8192);
    d.assets.emplace_back(RasterAsset{"backdrop", makeRasterLayer(8192, 8192)});
    d.assets.emplace_back(RasterAsset{"mask", makeRasterLayer(8192, 8192)});
    d.layers.emplace_back(
        StaticBitmapLayer{{"backdrop-layer", "Backdrop"}, StaticSource{"backdrop"}});
    layerProperties(d.layers.front()).appearance.rasterMask = LayerMask{.assetId = "mask"};
    std::uint32_t random = 1;
    for (auto &asset : d.assets)
        for (auto &p : std::get<RasterAsset>(asset).pixels.pixels)
        {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            p = random;
        }
    auto *a = findRasterAsset(d, "paint");
    const auto original = a->pixels.pixels[std::size_t(4096) * 8192 + 4096];
    RasterHistoryLimits limits;
    limits.maximumBytes = 1024 * 1024;
    limits.maximumEntries = 128;
    BitmapEditor e(d, "paint", limits);
    std::vector<std::uint32_t> pixels(64 * 16, 0xffabcdef);
    const auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 48; ++i)
        expect(e.replacePatch({{4096 + (i % 8) * 64, 4096 + (i / 8) * 16}, 64, 16}, pixels),
               "8K: non-overlapping region command");
    expect(e.historyStatistics().undoEntries == 48 &&
               e.historyStatistics().retainedBytes < 1024 * 1024,
           "8K: history bounded by changed regions");
    for (int i = 0; i < 48; ++i)
        expect(e.undo(), "8K: all commands undo");
    expect(e.pixelAt(4096, 4096) == original, "8K: original high-entropy pixel restored");
    BitmapBrush brush;
    brush.size = 12;
    expect(e.setBrush(brush) && e.beginStroke({4096., 4096.}), "8K: actual Native brush preview");
    const auto previewBytes = e.previewStorageBytes();
    e.cancelStroke();
    expect(e.pixelAt(4096, 4096) == original && e.historyStatistics().redoEntries == 48,
           "8K: preview cancel preserves original pixels and redo");
    expect(previewBytes < 1024 * 1024, "8K: preview does not clone the full raster");
    BitmapEditor maskEditor(d, "mask", limits);
    expect(maskEditor.replacePatch({{0, 0}, 64, 16}, pixels) && maskEditor.undo(),
           "8K: referenced raster mask region edit and undo");
    const auto operationTime = std::chrono::steady_clock::now() - begin;
    // Regenerate the independent source fixture without retaining another 768 MiB copy.
    random = 1;
    bool restored = true;
    std::uint64_t checkedPixels = 0;
    for (const auto &asset : d.assets)
        for (const auto p : std::get<RasterAsset>(asset).pixels.pixels)
        {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            restored = (p == random) && restored;
            ++checkedPixels;
        }
    expect(restored, "8K: every paint, backdrop and referenced mask pixel restored");
    std::cout << "8K source_bytes=" << 3ULL * 8192 * 8192 * 4
              << " commands=48 retained_bytes=" << e.historyStatistics().retainedBytes
              << " preview_bytes=" << previewBytes << " checked_pixels=" << checkedPixels
              << " source_restored=" << (restored ? "true" : "false") << " operations_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(operationTime).count()
              << '\n';
}
} // namespace
int main(int argc, char **)
{
    if (argc > 1)
        large();
    else
    {
        finitePatches();
        budgets();
        resize();
        chunks();
        chunkAssetCoordinates();
        chunkAssetStroke();
        sparseDirtyBounds();
        chunkStrokeGrowth();
        preview();
        file();
        removalConflicts();
        fileFailure();
        aliasAndEmptyCancel();
    }
    return failures ? 1 : 0;
}
