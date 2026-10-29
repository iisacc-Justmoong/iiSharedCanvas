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
#include <limits>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STRUCTURE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(STRUCTURE_ASAN)
#define STRUCTURE_ASAN 1
#endif
#ifndef STRUCTURE_ASAN
#define STRUCTURE_ASAN 0
#endif

namespace allocation {
thread_local bool active = false;
thread_local bool fired = false;
thread_local std::size_t largest = 0, total = 0, calls = 0;
thread_local std::size_t failAt = std::numeric_limits<std::size_t>::max();
}
#if !STRUCTURE_ASAN
void *operator new(std::size_t size) {
    if (allocation::active) {
        allocation::largest=std::max(allocation::largest,size);
        allocation::total+=size;
        if (allocation::calls++==allocation::failAt) {
            allocation::active=false;
            allocation::fired=true;
            throw std::bad_alloc();
        }
    }
    if (auto *p=std::malloc(size ? size : 1)) return p;
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
std::uint64_t checksum(const RasterLayer &layer) {
    std::uint64_t hash=1469598103934665603ULL;
    for (auto pixel:layer.pixels) { hash^=pixel; hash*=1099511628211ULL; }
    return hash;
}
struct Fixture {
    Document document;
    std::array<std::uint64_t,2> checksums;
};
Fixture fixture(int side,bool entropy) {
    Fixture result; auto &d=result.document;
    d.extent={side,side}; d.timeline={{24,1},8};
    std::uint32_t random=1;
    for (const auto *id:{"back","front"}) {
        auto pixels=makeRasterLayer(side,side);
        if (entropy) for (auto &p:pixels.pixels) {
            random^=random<<13; random^=random>>17; random^=random<<5; p=random;
        }
        else { pixels.pixels[0]=0xff123456; pixels.pixels.back()=0x7fabcdef; }
        result.checksums[d.assets.size()]=checksum(pixels);
        d.assets.emplace_back(RasterAsset{id,std::move(pixels)});
    }
    d.layers.emplace_back(StaticBitmapLayer{{"back-layer","Back"},StaticSource{"back"}});
    d.layers.emplace_back(StaticBitmapLayer{{"front-layer","Front"},StaticSource{"front"}});
    layerProperties(d.layers.back()).appearance.rasterMask=LayerMask{.assetId="back"};
    LayerFilter filter; filter.name="Invert"; filter.operation=RasterEffect{RasterEffectKind::Invert};
    layerProperties(d.layers.back()).appearance.filters.push_back(filter);
    AudioAsset audio; audio.id="audio"; audio.samples.resize(131072);
    for (std::size_t i=0;i<audio.samples.size();++i) audio.samples[i]=std::int16_t((i*17)%32768);
    d.audioAssets.push_back(std::move(audio));
    return result;
}
std::array<const void *,3> pixels(const Document &d) {
    return {findRasterAsset(d,"back")->pixels.pixels.data(),findRasterAsset(d,"front")->pixels.pixels.data(),
            d.audioAssets.front().samples.data()};
}
bool audioIntact(const Document &d) {
    if (d.audioAssets.size()!=1 || d.audioAssets.front().samples.size()!=131072) return false;
    const auto &samples=d.audioAssets.front().samples;
    for (std::size_t i=0;i<samples.size();++i)
        if (samples[i]!=std::int16_t((i*17)%32768)) return false;
    return true;
}
template <class Function>
void measured(DocumentFile &file,std::string_view name,Function function,bool enforce) {
    const auto before=pixels(*file.document());
    const auto revision=file.revision();
    allocation::largest=allocation::total=allocation::calls=0;
    allocation::active=true;
    const auto start=std::chrono::steady_clock::now();
    bool changed=false;
    try { changed=function(); }
    catch (...) { allocation::active=false; throw; }
    const auto duration=std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-start).count();
    allocation::active=false;
    const bool same=pixels(*file.document())==before;
    std::cout << "STRUCTURE_EDIT name=" << name << " max_cpp=" << allocation::largest
              << " total_cpp=" << allocation::total << " elapsed_us=" << duration
              << " pixels_preserved=" << same << " payload_written="
              << file.lastWriteStatistics().payloadBytesWritten << '\n';
    expect(changed && file.revision()==revision+1,"structure: operation commits exactly one revision");
    if (enforce) {
        expect(same,"structure: successful change retains both original pixel buffers");
        expect(file.lastWriteStatistics().payloadBytesWritten<1024*1024,
               "structure: no raster payload is rewritten");
#if !STRUCTURE_ASAN
        expect(allocation::largest<4*1024*1024 && allocation::total<16*1024*1024,
               "structure: edit does not allocate a raster-sized document draft");
#endif
    }
}
void exercise(int side,bool entropy,bool large) {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"structure: temporary directory"); return; }
    const auto path=(temporary.path()/"structure.iisc").string();
    auto source=fixture(side,entropy); const auto original=source.checksums;
    DocumentFile file;
    if (!file.create(path,source.document).ok()) { expect(false,"structure: create working file"); return; }
    source.document={};
    const auto fileBytes=std::filesystem::file_size(path);
    if (large) expect(fileBytes>400ULL*1024*1024,"8K: both dense rasters are present in the working file");
    DocumentEditor editor(file);
    if (!editor.isBound()) { expect(false,"structure: bind editor"); return; }

    measured(file,"insert",[&] { return editor.insertLayer(
        StaticBitmapLayer{{"temporary","Temporary"},StaticSource{"back"}},1).changed; },true);
    measured(file,"insert-keyframed",[&] { return editor.insertKeyframedLayer(
        DynamicBitmapLayer{{"animated","Animated"},KeyframedSource{}},
        {{0,"back"},{3,"front"}},0).changed; },true);
    const auto *initialKey=findKeyframe(*file.document(),"animated",0);
    const auto *laterKey=findKeyframe(*file.document(),"animated",3);
    expect(initialKey && laterKey && laterKey->assetId=="front",
           "structure: inserted animated layer retains frame keys");
    if (!large && side==32) {
        Document intermediate;
        expect(DocumentFile::readSnapshot(path,intermediate).ok() &&
               findLayer(intermediate,"animated") && findKeyframe(intermediate,"animated",0) &&
               findKeyframe(intermediate,"animated",3) &&
               findKeyframe(intermediate,"animated",3)->assetId=="front",
               "structure: inserted animated keys are durable before removal");
    }
    measured(file,"reorder",[&] { return editor.moveLayer("front-layer",0).changed; },true);
    expect(layerIndex(*file.document(),"front-layer")==0,"structure: reordered layer index");
    measured(file,"remove-static",[&] { return editor.removeLayer("temporary").changed; },true);
    measured(file,"remove-keyframed",[&] { return editor.removeLayer("animated").changed; },true);
    expect(!findLayer(*file.document(),"animated") && !findKeyframe(*file.document(),"animated",0),
           "structure: deleting animation removes its keys");
    DocumentHistory history(file);
    measured(file,"history-reorder",[&] { return history.moveLayer("front-layer",1).changed; },true);
    measured(file,"history-undo",[&] { return history.undo().changed; },true);
    measured(file,"history-redo",[&] { return history.redo().changed; },true);
    expect(history.statistics().undoEntries==1 && layerIndex(*file.document(),"front-layer")==1,
           "structure: global undo and redo preserve one command");

    const auto revision=file.revision(); const auto stats=history.statistics();
    const auto stablePointers=pixels(*file.document());
    expect(editor.moveLayer("front-layer",1).ok() && file.revision()==revision,
           "structure: no-op keeps revision");
    expect(!editor.moveLayer("missing",0).ok() && !editor.moveLayer("front-layer",99).ok() &&
           !editor.insertLayer(StaticBitmapLayer{{"bad","Bad"},StaticSource{"missing"}},0).ok() &&
           file.revision()==revision && pixels(*file.document())==stablePointers &&
           history.statistics().undoEntries==stats.undoEntries,
           "structure: rejected edits preserve document and history");

    for (std::size_t i=0;i<original.size();++i) {
        const auto *asset=findRasterAsset(*file.document(),i ? "front" : "back");
        expect(checksum(asset->pixels)==original[i],"structure: every original asset pixel is unchanged");
    }
    file.close();
    DocumentFile reopened;
    if (!reopened.open(path).ok()) { expect(false,"structure: reopen working file"); return; }
    expect(reopened.revision()==revision && layerIndex(*reopened.document(),"front-layer")==1 &&
           !findLayer(*reopened.document(),"temporary") && !findLayer(*reopened.document(),"animated"),
           "structure: reopened layer order and removals");
    if (const auto *front=findLayer(*reopened.document(),"front-layer")) {
        const auto &appearance=layerProperties(*front).appearance;
        expect(appearance.rasterMask && appearance.rasterMask->assetId=="back" &&
               appearance.filters.size()==1 && appearance.filters[0].name=="Invert" &&
               audioIntact(*reopened.document()),"structure: masks, filters and audio survive reopening");
    } else expect(false,"structure: front layer survives reopening");
    for (std::size_t i=0;i<original.size();++i) {
        const auto *asset=findRasterAsset(*reopened.document(),i ? "front" : "back");
        expect(asset && checksum(asset->pixels)==original[i],"structure: reopened assets preserve all pixels");
    }
    std::cout << "STRUCTURE_FILE side=" << side << " dense=" << entropy << " file_bytes=" << fileBytes
              << " checked_pixels=" << 2ULL*side*side << " reopened=" << reopened.isOpen() << '\n';
}
void conflict() {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"conflict: temporary directory"); return; }
    const auto path=(temporary.path()/"conflict.iisc").string();
    auto source=fixture(32,false); DocumentFile first,stale;
    if (!first.create(path,source.document).ok() || !stale.open(path).ok()) {
        expect(false,"conflict: open two owners"); return;
    }
    DocumentHistory old(stale); DocumentEditor editor(first);
    expect(editor.moveLayer("front-layer",0).changed,"conflict: first owner changes order");
    const auto attempted=old.moveLayer("front-layer",1);
    expect(attempted.code==DocumentHistoryCode::Conflict && !attempted.changed &&
           old.statistics().undoEntries==0,"conflict: stale no-op checks external writer");
    Document snapshot;
    expect(DocumentFile::readSnapshot(path,snapshot).ok() && layerIndex(snapshot,"front-layer")==0,
           "conflict: durable writer's order is preserved");
}
void lockedFile() {
    namespace sql=iiFileProvider::native::sql;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"lock: temporary directory"); return; }
    const auto path=(temporary.path()/"locked.iisc").string();
    auto source=fixture(32,false); DocumentFile file;
    if (!file.create(path,source.document).ok()) { expect(false,"lock: create"); return; }
    DocumentHistory history(file);
    expect(history.setLayerVisible("back-layer",false).changed && history.undo().changed,
           "lock: create redo branch");
    const auto revision=file.revision(); const auto stable=encodeIisc(*file.document()).bytes;
    const auto stats=history.statistics(); const auto stablePointers=pixels(*file.document());
    auto other=sql::Database::open(std::filesystem::u8path(path));
    if (!other) { expect(false,"lock: open another database connection"); return; }
    auto lock=sql::Transaction::begin(*other,true);
    if (!lock) { expect(false,"lock: hold another writer transaction"); return; }
    const auto result=history.moveLayer("front-layer",0);
    expect(result.code==DocumentHistoryCode::Conflict && !result.changed && file.isOpen() && file.revision()==revision &&
           pixels(*file.document())==stablePointers && history.canRedo() &&
           history.statistics().redoEntries==stats.redoEntries,
           "lock: blocked write preserves owner and redo cursor");
    expect(lock->rollback().has_value(),"lock: release writer");
    Document snapshot;
    expect(DocumentFile::readSnapshot(path,snapshot).ok() && encodeIisc(snapshot).bytes==stable,
           "lock: blocked write preserves durable document");
}
void limits() {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"limits: temporary directory"); return; }
    const auto path=(temporary.path()/"limits.iisc").string();
    auto source=fixture(32,false); SerializationLimits limits; limits.maximumLayers=2;
    DocumentFile file;
    if (!file.create(path,source.document,limits).ok()) { expect(false,"limits: create"); return; }
    DocumentHistory history(file); DocumentEditor editor(file);
    expect(history.setLayerVisible("back-layer",false).changed && history.undo().changed,
           "limits: establish redo branch");
    const auto revision=file.revision(); const auto bytes=encodeIisc(*file.document()).bytes;
    const auto pointers=pixels(*file.document()); const auto stats=history.statistics();
    const auto inserted=editor.insertLayer(StaticBitmapLayer{{"third","Third"},StaticSource{"back"}},1);
    expect(!inserted.ok() && file.lastResult().code==DocumentFileCode::LimitExceeded &&
           file.revision()==revision && pixels(*file.document())==pointers && history.canRedo() &&
           history.statistics().redoEntries==stats.redoEntries && encodeIisc(*file.document()).bytes==bytes,
           "limits: over-limit insertion preserves document and redo");
    Document snapshot;
    expect(DocumentFile::readSnapshot(path,snapshot).ok() && encodeIisc(snapshot).bytes==bytes,
           "limits: over-limit insertion preserves durable document");
}

enum class Operation { Insert, InsertKeys, Move, Remove };
bool applyOperation(Operation op,DocumentEditor &editor,DocumentHistory &history) {
    switch (op) {
    case Operation::Insert:
        return editor.insertLayer(StaticBitmapLayer{{"fault-static","Static"},StaticSource{"back"}},1).changed;
    case Operation::InsertKeys:
        return editor.insertKeyframedLayer(DynamicBitmapLayer{{"fault-keys","Dynamic"},KeyframedSource{}},
                                           {{0,"back"},{3,"front"}},1).changed;
    case Operation::Move: return history.moveLayer("front-layer",0).changed;
    case Operation::Remove: return editor.removeLayer("front-layer").changed;
    }
    return false;
}
bool reverseOperation(Operation op,DocumentEditor &editor,DocumentHistory &history,const Layer &front) {
    switch (op) {
    case Operation::Insert: return editor.removeLayer("fault-static").changed;
    case Operation::InsertKeys: return editor.removeLayer("fault-keys").changed;
    case Operation::Move: return history.undo().changed;
    case Operation::Remove: return editor.insertLayer(front,1).changed;
    }
    return false;
}
void allocationFailures() {
#if !STRUCTURE_ASAN
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"allocation: temporary directory"); return; }
    for (const auto op:{Operation::Insert,Operation::InsertKeys,Operation::Move,Operation::Remove}) {
        const auto path=(temporary.path()/("fault-"+std::to_string(static_cast<int>(op))+".iisc")).string();
        auto source=fixture(16,false); DocumentFile file;
        if (!file.create(path,source.document).ok()) { expect(false,"allocation: create"); return; }
        DocumentEditor editor(file); DocumentHistory history(file); const Layer front=file.document()->layers[1];
        allocation::calls=allocation::largest=allocation::total=0; allocation::active=true;
        const bool probed=applyOperation(op,editor,history);
        allocation::active=false; const auto calls=allocation::calls;
        if (!probed || !reverseOperation(op,editor,history,front)) { expect(false,"allocation: probe and restore"); return; }
        const std::set<std::size_t> offsets{0,1,4,calls/4,calls/2,calls-1,calls,calls+64};
        std::size_t rejected=0,accepted=0;
        for (const auto offset:offsets) {
            std::cout << "STRUCTURE_INJECT operation=" << static_cast<int>(op)
                      << " offset=" << offset << " observed=" << calls << '\n';
            expect(history.reset().ok() && history.setLayerVisible("back-layer",false).changed &&
                   history.undo().changed,"allocation: create redo branch");
            const auto revision=file.revision(); const auto stable=encodeIisc(*file.document()).bytes;
            const auto stats=history.statistics(); const auto stablePointers=pixels(*file.document());
            allocation::calls=0; allocation::failAt=offset; allocation::fired=false; allocation::active=true;
            bool changed=false;
            try { changed=applyOperation(op,editor,history); } catch (const std::bad_alloc &) {}
            allocation::active=false; allocation::failAt=std::numeric_limits<std::size_t>::max();
            if (!file.isOpen()) { expect(false,"allocation: file binding survives known failure"); return; }
            Document snapshot;
            expect(DocumentFile::readSnapshot(path,snapshot).ok(),"allocation: read durable state");
            if (!changed) {
                ++rejected;
                expect(allocation::fired && file.revision()==revision &&
                       encodeIisc(*file.document()).bytes==stable && encodeIisc(snapshot).bytes==stable &&
                       pixels(*file.document())==stablePointers && history.canRedo() &&
                       history.statistics().undoEntries==stats.undoEntries &&
                       history.statistics().redoEntries==stats.redoEntries &&
                       history.statistics().retainedBytes==stats.retainedBytes,
                       "allocation: rejected transaction preserves memory, durable state and redo");
            } else {
                ++accepted;
                expect(file.revision()==revision+1 && encodeIisc(snapshot).bytes==encodeIisc(*file.document()).bytes,
                       "allocation: successful transaction publishes matching durable state");
                expect(reverseOperation(op,editor,history,front),"allocation: reverse successful probe");
            }
        }
        expect(rejected>0 && accepted>0,"allocation: both failure and success paths exercised");
        std::cout << "STRUCTURE_FAULTS operation=" << static_cast<int>(op) << " allocations=" << calls
                  << " samples=" << offsets.size() << " rejected=" << rejected << " accepted=" << accepted << '\n';
    }
#endif
}

void legacy() {
    namespace sql=iiFileProvider::native::sql;
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"legacy: temporary directory"); return; }
    const auto path=temporary.path()/"legacy.iisc";
    {
        auto database=sql::Database::open(path,{.mode=sql::OpenMode::create});
        if (!database) { expect(false,"legacy: create fixture"); return; }
        for (const auto *statement:{
            "PRAGMA application_id=1229542211", "PRAGMA user_version=1",
            "CREATE TABLE canvas_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), revision INTEGER NOT NULL CHECK(revision>=0))",
            "CREATE TABLE canvas_records (kind INTEGER NOT NULL, id TEXT NOT NULL, position INTEGER NOT NULL CHECK(position>=0), data BLOB NOT NULL, digest BLOB NOT NULL, UNIQUE(kind,id))",
            "INSERT INTO canvas_state VALUES(1,0)"})
            if (!database->execute(statement)) { expect(false,"legacy: create schema"); return; }
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
                expect(false,"legacy: populate fixture"); return;
            }
        }
    }
    DocumentFile file;
    if (!file.open(path.string()).ok()) { expect(false,"legacy: open fixture"); return; }
    DocumentEditor editor(file);
    expect(file.storageVersion()==1 && editor.moveLayer("layer",0).ok() &&
           file.revision()==0 && file.storageVersion()==1,"legacy: no-op does not migrate storage");
    expect(editor.insertLayer(StaticBitmapLayer{{"second","Second"},StaticSource{"paint"}},0).changed &&
           file.storageVersion()==2 && file.revision()==1,"legacy: real insertion migrates atomically");
    file.close();
    expect(file.open(path.string()).ok() && layerIndex(*file.document(),"second")==0 &&
           findRasterAsset(*file.document(),"paint")->pixels.pixels[0]==0xff123456,
           "legacy: migrated layer and pixel reopen");
}
}
int main(int argc,char **argv) {
    std::cout << std::unitbuf;
    const std::string_view mode=argc>1?argv[1]:"--contract";
    if (mode=="--large") exercise(8192,true,true);
    else if (mode=="--cost") exercise(2048,false,false);
    else if (mode=="--preflight") { conflict(); lockedFile(); limits(); legacy(); }
    else { exercise(32,false,false); conflict(); lockedFile(); limits(); allocationFailures(); legacy(); }
    return failures?1:0;
}
