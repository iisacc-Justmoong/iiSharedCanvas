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
#include <string>
#include <string_view>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ASSET_LAYER_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(ASSET_LAYER_ASAN)
#define ASSET_LAYER_ASAN 1
#endif
#ifndef ASSET_LAYER_ASAN
#define ASSET_LAYER_ASAN 0
#endif
namespace allocations {
thread_local bool active=false;
thread_local std::size_t maximum=0,total=0;
}
#if !ASSET_LAYER_ASAN
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

namespace {
using namespace iiSharedCanvas;
int failures=0;
void expect(bool value,std::string_view message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
Document finite(int side=32,bool dense=false) {
    Document d; d.extent={side,side}; d.timeline={{24,1},4};
    auto pixels=makeRasterLayer(side,side);
    if (dense) {
        std::uint32_t v=37;
        for (auto &p:pixels.pixels) { v^=v<<13; v^=v>>17; v^=v<<5; p=v; }
    } else pixels.pixels[0]=0xff123456;
    d.assets.emplace_back(RasterAsset{"base-asset",std::move(pixels)});
    d.layers.emplace_back(StaticBitmapLayer{{"base","Base"},StaticSource{"base-asset"}});
    return d;
}
std::uint64_t checksum(const RasterLayer &r) {
    std::uint64_t h=1469598103934665603ULL;
    for (auto p:r.pixels) { h^=p; h*=1099511628211ULL; }
    return h;
}
Layer bitmap(std::string asset="new-asset",std::string id="new-layer") {
    return StaticBitmapLayer{{std::move(id),"New"},StaticSource{std::move(asset)}};
}
RasterAsset raster(int side=32) {
    auto p=makeRasterLayer(side,side,0); p.pixels[0]=0xffaabbcc; p.pixels.back()=0xff345678;
    return {"new-asset",std::move(p)};
}
void roundtrip(DocumentHistory &history,const Document &doc) {
    const auto *base=findRasterAsset(doc,"base-asset");
    if (!base) { expect(false,"roundtrip: base asset"); return; }
    const auto *baseBuffer=base->pixels.pixels.data();
    const auto revision=doc.authorship.revision();
    auto value=raster(); const auto expected=checksum(value.pixels);
    const auto insert=history.insertLayerWithAsset(std::move(value),bitmap(),1,0,"Create layer");
    expect(insert.changed && insert.entryBytes>0 && history.undoLabel()=="Create layer" &&
           doc.authorship.revision()==revision+1 && assetIndex(doc,"new-asset")==0 &&
           layerIndex(doc,"new-layer")==1 && findRasterAsset(doc,"new-asset") &&
           checksum(findRasterAsset(doc,"new-asset")->pixels)==expected,
           "roundtrip: one command creates asset and layer at their requested positions");
    const std::array<std::uint32_t,1> paint{0xff00aabb};
    const std::array patch{BitmapPatch{{{1,1},1,1},paint}};
    expect(history.setLayerName("new-layer","Ink").changed &&
           history.replaceBitmapPatches("new-asset",patch,"Paint").changed && history.undo().changed &&
           findRasterAsset(doc,"new-asset")->pixels.pixels[33]==0 && history.undo().changed,
           "roundtrip: later pixel and name edits undo before the compound creation");
    expect(history.undo().changed && !findAsset(doc,"new-asset") && !findLayer(doc,"new-layer") &&
           history.canRedo() && findRasterAsset(doc,"base-asset")->pixels.pixels.data()==baseBuffer,
           "roundtrip: undo removes both new values without replacing existing pixels");
    expect(history.redo().changed && findRasterAsset(doc,"new-asset") &&
           checksum(findRasterAsset(doc,"new-asset")->pixels)==expected &&
           layerIndex(doc,"new-layer")==1 && assetIndex(doc,"new-asset")==0 &&
           history.redo().changed && layerProperties(*findLayer(doc,"new-layer")).name=="Ink" &&
           history.redo().changed && findRasterAsset(doc,"new-asset")->pixels.pixels[33]==paint[0],
           "roundtrip: redo recreates exact contents and resumes the shared pixel and name order");
}
void memory() {
    auto d=finite(); DocumentHistory h(d); roundtrip(h,d);
    {
        auto v=finite(); DocumentHistory history(v);
        VectorPath path; path.commands={MoveTo{{0,1}},LineTo{{2,3}},QuadraticTo{{4,5},{6,7}},
                                        CubicTo{{8,9},{10,11},{12,13}},ClosePath{}};
        path.stroke=StrokeStyle{{0xff55aacc},2};
        VectorAsset asset{"vector-asset",{32,32},{path}};
        Layer layer=StaticVectorLayer{{"vector-layer","Vector"},StaticSource{"vector-asset"}};
        expect(history.insertLayerWithAsset(std::move(asset),std::move(layer)).changed &&
               findVectorAsset(v,"vector-asset") && history.undo().changed &&
               !findAsset(v,"vector-asset") && history.redo().changed &&
               findVectorAsset(v,"vector-asset") &&
               findVectorAsset(v,"vector-asset")->paths[0].commands.size()==5,
               "vector: asset, geometry and layer survive undo and redo");
        auto *vector=findVectorAsset(v,"vector-asset");
        if (!vector) { expect(false,"vector: restored asset exists"); return; }
        std::get<MoveTo>(vector->paths[0].commands[0]).point.x=-0.0;
        expect(history.undo().code==DocumentHistoryCode::Conflict && history.canUndo(),
               "vector: external signed-zero bit changes are not discarded");
    }
    {
        auto d=finite(); DocumentHistory history(d);
        auto pixels=makeRasterLayer(32,32);
        for (std::size_t i=0;i<pixels.pixels.size();++i)
            pixels.pixels[i]=std::uint32_t(i*2654435761U);
        const auto expected=pixels.pixels;
        expect(history.insertLayerWithAsset(RasterAsset{"new-asset",std::move(pixels)},bitmap()).changed &&
               history.undo().changed && history.redo().changed &&
               findRasterAsset(d,"new-asset")->pixels.pixels==expected,
               "raw: incompressible pixels restore exactly within the budget");
    }
    {
        auto d=finite(); d.formatVersion.minor=17; DocumentHistory history(d);
        auto layer=bitmap(); layerProperties(layer).appearance.rasterMask=LayerMask{.assetId="base-asset"};
        expect(history.isBound() && history.insertLayerWithAsset(raster(),std::move(layer)).changed &&
               d.formatVersion.minor==CurrentFormatMinor && history.undo().changed && history.redo().changed &&
               layerProperties(*findLayer(d,"new-layer")).appearance.rasterMask.has_value(),
               "legacy: a real edit upgrades before validating modern layer metadata");
    }
    {
        Document d; d.extent={128,128}; d.canvasMode=CanvasMode::Infinite;
        d.infiniteCanvas.chunkSize=32;
        d.assets.emplace_back(ChunkedRasterAsset{"base-asset",{}});
        d.layers.emplace_back(StaticBitmapLayer{{"base","Base"},StaticSource{"base-asset"}});
        DocumentHistory history(d);
        auto p=makeRasterLayer(32,32,0); p.pixels[9]=0xff123456;
        ChunkedRasterAsset asset{"new-asset",{{-1,2,std::move(p)}}};
        expect(history.insertLayerWithAsset(std::move(asset),bitmap()).changed &&
               history.undo().changed && !findAsset(d,"new-asset") && history.redo().changed,
               "chunked: creation is a single reversible command");
        const auto *restored=findChunkedRasterAsset(d,"new-asset");
        expect(restored && restored->chunks.size()==1 && restored->chunks[0].column==-1 &&
               restored->chunks[0].row==2 && restored->chunks[0].pixels.pixels[9]==0xff123456,
               "chunked: redo restores coordinates and pixel contents");
    }
}
void rejected() {
    {
        auto d=finite(); DocumentHistoryLimits limits; limits.maximumBytes=1024;
        DocumentHistory h(d,limits);
        expect(h.setLayerVisible("base",false).changed && h.undo().changed,"budget: existing redo");
        auto p=makeRasterLayer(32,32); for (std::size_t i=0;i<p.pixels.size();++i) p.pixels[i]=std::uint32_t(i);
        const auto before=encodeIisc(d).bytes; const auto revision=d.authorship.revision();
        auto result=h.insertLayerWithAsset(RasterAsset{"new-asset",std::move(p)},bitmap());
        expect(result.code==DocumentHistoryCode::BudgetExceeded && encodeIisc(d).bytes==before &&
               d.authorship.revision()==revision && h.canRedo(),
               "budget: incompressible asset is rejected before mutation and preserves redo");
    }
    {
        auto d=finite(); DocumentHistory h(d); const auto before=encodeIisc(d).bytes;
        expect(!h.insertLayerWithAsset(raster(),bitmap("missing")).ok() && encodeIisc(d).bytes==before &&
               !h.canUndo(),"invalid: layer must reference the inserted asset");
        expect(!h.insertLayerWithAsset(RasterAsset{"base-asset",makeRasterLayer(32,32)},bitmap()).ok() &&
               encodeIisc(d).bytes==before,"invalid: existing asset ID is not overwritten");
        VideoAsset video{"video",{24,1},{makeRasterLayer(32,32)}};
        Layer videoLayer=VideoLayer{{"video-layer","Video"},StaticSource{"video"},{}};
        expect(h.insertLayerWithAsset(std::move(video),std::move(videoLayer)).code==DocumentHistoryCode::InvalidArgument &&
               encodeIisc(d).bytes==before && !h.canUndo(),
               "unsupported: video content is rejected without an orphan asset");
    }
    {
        auto d=finite(); DocumentHistoryLimits limits; limits.maximumBitmapInputBytes=8;
        DocumentHistory h(d,limits); const auto before=encodeIisc(d).bytes;
        expect(h.insertLayerWithAsset(raster(),bitmap()).code==DocumentHistoryCode::LimitExceeded &&
               encodeIisc(d).bytes==before && !h.canUndo(),
               "input limit: oversized raster input is rejected before mutation");
    }
    {
        auto d=finite(); DocumentHistory h(d);
        expect(h.beginPixelPreview("base-asset").ok(),"preview: begin");
        const auto before=encodeIisc(d).bytes;
        expect(h.insertLayerWithAsset(raster(),bitmap()).code==DocumentHistoryCode::PreviewActive &&
               encodeIisc(d).bytes==before && h.pixelPreviewActive() && h.cancelPixelPreview().ok(),
               "preview: compound insertion is blocked without altering the session");
    }
    {
        auto d=finite(); DocumentHistoryLimits limits; limits.maximumEntries=1;
        limits.overflow=DocumentHistoryOverflow::EvictOldest;
        DocumentHistory h(d,limits);
        expect(h.setLayerVisible("base",false).changed,"eviction: create prior command");
        const auto result=h.insertLayerWithAsset(raster(),bitmap());
        expect(result.changed && result.evictedEntries==1 && h.undo().changed && !h.canUndo() &&
               !findAsset(d,"new-asset") && !layerProperties(*findLayer(d,"base")).visible,
               "eviction: one published compound command evicts and reports the older entry");
    }
    {
        auto d=finite(); DocumentHistory h(d);
        expect(h.insertLayerWithAsset(raster(),bitmap()).changed,"conflict: setup");
        findRasterAsset(d,"new-asset")->pixels.pixels[7]=0xff123456;
        const auto before=encodeIisc(d).bytes;
        expect(h.undo().code==DocumentHistoryCode::Conflict && encodeIisc(d).bytes==before && h.canUndo(),
               "conflict: untracked asset pixels are not discarded");
    }
    {
        auto d=finite(); DocumentHistory h(d);
        expect(h.insertLayerWithAsset(raster(),bitmap()).changed,"reference conflict: setup");
        staticLayerSource(*findLayer(d,"base"))->assetId="new-asset";
        const auto before=encodeIisc(d).bytes;
        expect(h.undo().code==DocumentHistoryCode::Conflict && encodeIisc(d).bytes==before && h.canUndo(),
               "conflict: an externally shared asset is not removed");
    }
    {
        auto d=finite(); DocumentHistory h(d);
        expect(h.insertLayerWithAsset(raster(),bitmap()).changed,"mask conflict: setup");
        layerProperties(*findLayer(d,"base")).appearance.rasterMask=LayerMask{.assetId="new-asset"};
        const auto before=encodeIisc(d).bytes;
        expect(h.undo().code==DocumentHistoryCode::Conflict && encodeIisc(d).bytes==before && h.canUndo(),
               "conflict: an externally added mask reference prevents asset removal");
    }
}
void file() {
    namespace sql=iiFileProvider::native::sql;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"file: temporary directory"); return; }
    const auto path=(temporary.path()/"asset-layer.iisc").string();
    auto source=finite(); DocumentFile owner;
    if (!owner.create(path,source).ok()) { expect(false,"file: create"); return; }
    DocumentHistory h(owner); roundtrip(h,*owner.document());
    Document snapshot;
    expect(DocumentFile::readSnapshot(path,snapshot).ok() && findRasterAsset(snapshot,"new-asset") &&
           findLayer(snapshot,"new-layer") && layerProperties(*findLayer(snapshot,"new-layer")).name=="Ink",
           "file: durable state matches the redone document");
    expect(h.undo().changed && h.undo().changed &&
           layerProperties(*findLayer(*owner.document(),"new-layer")).name=="New",
           "file: remove later pixel and name edits before testing compound undo");
    auto lockDb=sql::Database::open(std::filesystem::u8path(path));
    if (!lockDb) { expect(false,"file: lock connection"); return; }
    const auto prior=encodeIisc(*owner.document()).bytes; const auto rev=owner.revision();
    auto lock=sql::Transaction::begin(*lockDb,true);
    if (!lock) { expect(false,"file: lock"); return; }
    expect(h.undo().code==DocumentHistoryCode::Conflict && owner.revision()==rev &&
           encodeIisc(*owner.document()).bytes==prior && h.canUndo(),
           "file: failed locked undo retains document and cursor");
    expect(lock->rollback().has_value(),"file: release lock");
    expect(h.undo().changed && !findAsset(*owner.document(),"new-asset") &&
           !findLayer(*owner.document(),"new-layer") &&
           DocumentFile::readSnapshot(path,snapshot).ok() && !findAsset(snapshot,"new-asset"),
           "file: retry atomically removes both values from the durable document");
    const auto undone=encodeIisc(*owner.document()).bytes; const auto undoneRevision=owner.revision();
    auto redoLock=sql::Transaction::begin(*lockDb,true);
    if (!redoLock) { expect(false,"file: redo lock"); return; }
    expect(h.redo().code==DocumentHistoryCode::Conflict && owner.revision()==undoneRevision &&
           encodeIisc(*owner.document()).bytes==undone && h.canRedo(),
           "file: failed locked redo retains document and cursor");
    expect(redoLock->rollback().has_value(),"file: release redo lock");
    expect(h.redo().changed && DocumentFile::readSnapshot(path,snapshot).ok() &&
           findAsset(snapshot,"new-asset") && findLayer(snapshot,"new-layer"),
           "file: retry atomically restores both durable values");
    DocumentFile stale;
    if (!stale.open(path).ok()) { expect(false,"file: open second binding"); return; }
    DocumentHistory old(stale);
    expect(h.setLayerVisible("base",false).changed,"file: first writer commits");
    expect(old.insertLayerWithAsset(RasterAsset{"stale",makeRasterLayer(32,32)},bitmap("stale","stale-layer")).code==
               DocumentHistoryCode::Conflict && old.statistics().undoEntries==0 &&
           DocumentFile::readSnapshot(path,snapshot).ok() && !findAsset(snapshot,"stale") &&
           !layerProperties(*findLayer(snapshot,"base")).visible,
           "file: stale compound edit does not overwrite the winner");
    VectorPath vectorPath; vectorPath.commands={MoveTo{{1,2}},LineTo{{3,4}}};
    vectorPath.stroke=StrokeStyle{{0xff445566},3};
    expect(h.insertLayerWithAsset(VectorAsset{"vector",{32,32},{vectorPath}},
               StaticVectorLayer{{"vector-layer","Vector"},StaticSource{"vector"}}).changed &&
           h.undo().changed && h.redo().changed && DocumentFile::readSnapshot(path,snapshot).ok() &&
           findVectorAsset(snapshot,"vector") && findVectorAsset(snapshot,"vector")->paths.size()==1,
           "file: vector asset and layer survive compound undo, redo and reopen");
    {
        Document infinite; infinite.extent={128,128}; infinite.canvasMode=CanvasMode::Infinite;
        infinite.infiniteCanvas.chunkSize=32;
        infinite.assets.emplace_back(ChunkedRasterAsset{"base-asset",{}});
        infinite.layers.emplace_back(StaticBitmapLayer{{"base","Base"},StaticSource{"base-asset"}});
        DocumentFile chunks;
        const auto chunkPath=(temporary.path()/"chunked.iisc").string();
        if (!chunks.create(chunkPath,infinite).ok()) { expect(false,"file: create chunked document"); return; }
        DocumentHistory chunkHistory(chunks);
        auto pixels=makeRasterLayer(32,32); pixels.pixels[17]=0xff123456;
        expect(chunkHistory.insertLayerWithAsset(ChunkedRasterAsset{"new-asset",{{-1,2,std::move(pixels)}}},bitmap()).changed &&
               chunkHistory.undo().changed && chunkHistory.redo().changed &&
               DocumentFile::readSnapshot(chunkPath,snapshot).ok(),
               "file: chunked asset and layer survive compound undo, redo and reopen");
        const auto *saved=findChunkedRasterAsset(snapshot,"new-asset");
        expect(saved && saved->chunks.size()==1 && saved->chunks[0].column==-1 && saved->chunks[0].row==2 &&
               saved->chunks[0].pixels.pixels[17]==0xff123456,
               "file: chunk coordinates and nonzero pixels remain durable");
    }
}
void large() {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"large: temporary directory"); return; }
    const auto path=(temporary.path()/"large.iisc").string();
    auto source=finite(8192,true); const auto original=checksum(findRasterAsset(source,"base-asset")->pixels);
    AudioAsset sound; sound.id="sound"; sound.samples.resize(131072);
    for (std::size_t i=0;i<sound.samples.size();++i) sound.samples[i]=std::int16_t((i*17)%32768);
    const auto audio=sound.samples; source.audioAssets.push_back(std::move(sound));
    DocumentFile file;
    if (!file.create(path,source).ok()) { expect(false,"large: create"); return; }
    source={}; DocumentHistory h(file);
    const auto bytesBefore=std::filesystem::file_size(path);
    const auto *base=findRasterAsset(*file.document(),"base-asset")->pixels.pixels.data();
    const auto *audioBuffer=file.document()->audioAssets.front().samples.data();
    RasterAsset blank{"new-asset",makeRasterLayer(8192,8192,0)};
    const auto blankChecksum=checksum(blank.pixels);
    allocations::maximum=allocations::total=0; allocations::active=true;
    const auto insertStart=std::chrono::steady_clock::now();
    const auto result=h.insertLayerWithAsset(std::move(blank),bitmap());
    const auto insertUs=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-insertStart).count();
    allocations::active=false;
    const auto bytesAfter=std::filesystem::file_size(path);
    expect(result.changed && result.entryBytes<1024*1024 &&
           findRasterAsset(*file.document(),"base-asset")->pixels.pixels.data()==base &&
           file.document()->audioAssets.front().samples.data()==audioBuffer,
           "large: blank 8K layer is retained compactly without copying raster or audio assets");
    expect(bytesAfter<=bytesBefore+1024*1024,
           "large: a blank 8K asset does not write a full zero-filled raster to the working file");
#if !ASSET_LAYER_ASAN
    expect(allocations::maximum<4*1024*1024 && allocations::total<32*1024*1024,
           "large: insertion does not clone existing 8K pixels in C++ allocations");
#endif
    const auto undoStart=std::chrono::steady_clock::now();
    const auto undone=h.undo();
    const auto undoUs=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-undoStart).count();
    const bool absent=!findAsset(*file.document(),"new-asset");
    const auto redoStart=std::chrono::steady_clock::now();
    const auto redone=h.redo();
    const auto redoUs=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-redoStart).count();
    expect(undone.changed && absent && redone.changed && findRasterAsset(*file.document(),"new-asset") &&
           checksum(findRasterAsset(*file.document(),"base-asset")->pixels)==original &&
           checksum(findRasterAsset(*file.document(),"new-asset")->pixels)==blankChecksum &&
           file.document()->audioAssets.front().samples.data()==audioBuffer,
           "large: undo and redo preserve both 8K asset contents");
    file.close(); Document snapshot;
    expect(DocumentFile::readSnapshot(path,snapshot).ok() && findRasterAsset(snapshot,"new-asset") &&
           checksum(findRasterAsset(snapshot,"base-asset")->pixels)==original &&
           checksum(findRasterAsset(snapshot,"new-asset")->pixels)==blankChecksum &&
           snapshot.audioAssets.size()==1 && snapshot.audioAssets.front().samples==audio,
           "large: both assets survive a fresh snapshot read");
    std::cout << "ASSET_LAYER_8K retained=" << result.entryBytes << " max_cpp=" << allocations::maximum
              << " total_cpp=" << allocations::total << " checked_pixels=" << 2ULL*8192*8192
              << " file_before=" << bytesBefore << " file_after=" << bytesAfter
              << " insert_us=" << insertUs << " undo_us=" << undoUs << " redo_us=" << redoUs << '\n';
}
void legacy() {
    namespace sql=iiFileProvider::native::sql;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"legacy file: temporary directory"); return; }
    const auto path=temporary.path()/"legacy.iisc";
    {
        auto database=sql::Database::open(path,{.mode=sql::OpenMode::create});
        if (!database) { expect(false,"legacy file: open fixture"); return; }
        for (const auto *statement:{
            "PRAGMA application_id=1229542211", "PRAGMA user_version=1",
            "CREATE TABLE canvas_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), revision INTEGER NOT NULL CHECK(revision>=0))",
            "CREATE TABLE canvas_records (kind INTEGER NOT NULL, id TEXT NOT NULL, position INTEGER NOT NULL CHECK(position>=0), data BLOB NOT NULL, digest BLOB NOT NULL, UNIQUE(kind,id))",
            "INSERT INTO canvas_state VALUES(1,0)"})
            if (!database->execute(statement)) { expect(false,"legacy file: schema"); return; }
        const auto unhex=[](std::string_view text) {
            const auto digit=[](char c) { return c<='9'?c-'0':(c<='F'?c-'A'+10:c-'a'+10); };
            sql::Blob bytes;
            for (std::size_t i=0;i<text.size();i+=2) bytes.push_back(std::uint8_t((digit(text[i])<<4)|digit(text[i+1])));
            return bytes;
        };
        for (const auto &record:loadLegacyWorkingRecords(
                 std::filesystem::u8path(IISHAREDCANVAS_TEST_RESOURCE_DIR)/"fixtures/legacy-working-v1.tsv")) {
            auto insert=database->prepare("INSERT INTO canvas_records VALUES(?,?,?,?,?)");
            if (!insert || !insert->bind(1,std::int64_t(record.kind)) || !insert->bind(2,record.id) ||
                !insert->bind(3,std::int64_t(record.position)) || !insert->bind(4,unhex(record.data)) ||
                !insert->bind(5,unhex(record.digest)) || !insert->step()) {
                expect(false,"legacy file: insert fixture"); return;
            }
        }
    }
    DocumentFile file;
    if (!file.open(path.string()).ok()) { expect(false,"legacy file: open"); return; }
    DocumentHistory h(file);
    expect(file.storageVersion()==1 && h.insertLayerWithAsset(
               RasterAsset{"new-asset",makeRasterLayer(1,1,0xff445566)},bitmap()).changed &&
           file.storageVersion()==2 && file.revision()==1 && h.undo().changed && h.redo().changed,
           "legacy file: compound edit migrates, undoes and redoes atomically");
    Document snapshot;
    expect(DocumentFile::readSnapshot(path.string(),snapshot).ok() && findRasterAsset(snapshot,"new-asset") &&
           findRasterAsset(snapshot,"new-asset")->pixels.pixels[0]==0xff445566 && findLayer(snapshot,"new-layer"),
           "legacy file: migrated compound result survives reopen");
}
}
int main(int argc,char **argv) {
    if (argc>1 && std::string_view(argv[1])=="--large") large();
    else if (argc>1 && std::string_view(argv[1])=="--memory") { memory(); rejected(); }
    else { memory(); rejected(); file(); legacy(); }
    return failures ? 1 : 0;
}
