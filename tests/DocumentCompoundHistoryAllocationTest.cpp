#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderNative.h>
#include "AllocationTestSelection_p.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <string>
#include <string_view>

namespace fault {
thread_local std::ptrdiff_t remaining=-1;
thread_local bool fired=false;
}
void *operator new(std::size_t size) {
    if (fault::remaining==0) { fault::remaining=-1; fault::fired=true; throw std::bad_alloc(); }
    if (fault::remaining>0) --fault::remaining;
    if (auto *p=std::malloc(size?size:1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }
void operator delete[](void *p,std::size_t) noexcept { std::free(p); }

namespace {
using namespace iiSharedCanvas;
enum class Operation { Timing, TimingUndo, TimingRedo, Remove, RemoveUndo, RemoveRedo };
int failures=0;
void expect(bool value,std::string_view message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
Document fixture() {
    Document d; d.extent={32,32}; d.timeline={{24,1},12};
    d.assets.emplace_back(RasterAsset{"back",makeRasterLayer(32,32,0xff012345)});
    auto pixels=makeRasterLayer(32,32);
    for (std::size_t i=0;i<pixels.pixels.size();i+=7) pixels.pixels[i]=0xffabcdef;
    d.assets.emplace_back(RasterAsset{"removed",std::move(pixels)});
    d.layers.emplace_back(StaticBitmapLayer{{"back-layer","Back"},StaticSource{"back"}});
    d.layers.emplace_back(StaticBitmapLayer{{"removed-layer","Ink"},StaticSource{"removed"}});
    return d;
}
bool setup(DocumentHistory &h,Operation op) {
    if (op==Operation::Timing || op==Operation::Remove)
        return h.setLayerVisible("back-layer",false).changed && h.undo().changed;
    const bool timing=op==Operation::TimingUndo || op==Operation::TimingRedo;
    const auto result=timing ? h.setTimeline({{30000,1001},36},"Timing")
                             : h.removeLayerWithAsset("removed-layer","Delete");
    if (!result.changed) return false;
    return op==Operation::TimingUndo || op==Operation::RemoveUndo || h.undo().changed;
}
DocumentHistoryResult perform(DocumentHistory &h,Operation op) {
    switch (op) {
    case Operation::Timing: return h.setTimeline({{30000,1001},36},"Timing");
    case Operation::Remove: return h.removeLayerWithAsset("removed-layer","Delete");
    case Operation::TimingUndo: case Operation::RemoveUndo: return h.undo();
    case Operation::TimingRedo: case Operation::RemoveRedo: return h.redo();
    }
    return {DocumentHistoryCode::InvalidArgument};
}
bool reverse(DocumentHistory &h,Operation op) {
    return (op==Operation::TimingUndo || op==Operation::RemoveUndo) ? h.redo().changed : h.undo().changed;
}
void memory(Operation op) {
    std::size_t rejected=0,accepted=0;
    for (std::ptrdiff_t at=0;at<384;++at) {
        auto d=fixture(); DocumentHistory h(d);
        if (!setup(h,op)) { expect(false,"memory allocation: setup"); return; }
        const auto before=encodeIisc(d).bytes; const auto revision=d.authorship.revision();
        const auto stats=h.statistics(); const bool canUndo=h.canUndo(),canRedo=h.canRedo();
        const auto *baseAsset=findAsset(d,"back");
        const auto *basePixels=findRasterAsset(d,"back")->pixels.pixels.data();
        fault::fired=false; fault::remaining=at;
        DocumentHistoryResult result;
        try { result=perform(h,op); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        if (!result.ok()) {
            ++rejected; const auto after=h.statistics();
            expect(fault::fired && encodeIisc(d).bytes==before && d.authorship.revision()==revision &&
                   h.canUndo()==canUndo && h.canRedo()==canRedo && after.undoEntries==stats.undoEntries &&
                   after.redoEntries==stats.redoEntries && after.retainedBytes==stats.retainedBytes &&
                   findAsset(d,"back")==baseAsset && findRasterAsset(d,"back")->pixels.pixels.data()==basePixels,
                   "memory allocation: failed command preserves document, pointers and history");
        } else {
            ++accepted;
            expect(result.changed && reverse(h,op),"memory allocation: success publishes one reversible command");
        }
    }
    expect(rejected>0 && accepted>0,"memory allocation: failures and successes covered");
    std::cout << "COMPOUND_MEMORY_ALLOCATION operation=" << static_cast<int>(op)
              << " rejected=" << rejected << " accepted=" << accepted << '\n';
}
void file(Operation op) {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"file allocation: temporary directory"); return; }
    constexpr std::array<std::ptrdiff_t,8> positions{0,1,4,16,64,128,512,2048};
    std::size_t rejected=0,accepted=0;
    for (std::size_t i=0;i<positions.size();++i) {
        const auto path=(temporary.path()/(std::to_string(i)+".iisc")).string();
        auto d=fixture(); DocumentFile owner;
        if (!owner.create(path,d).ok()) { expect(false,"file allocation: create"); return; }
        DocumentHistory h(owner);
        if (!setup(h,op)) { expect(false,"file allocation: setup"); return; }
        const auto before=encodeIisc(*owner.document()).bytes; const auto revision=owner.revision();
        const auto stats=h.statistics(); const bool canUndo=h.canUndo(),canRedo=h.canRedo();
        const auto *basePixels=findRasterAsset(*owner.document(),"back")->pixels.pixels.data();
        fault::fired=false; fault::remaining=positions[i];
        DocumentHistoryResult result;
        try { result=perform(h,op); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        Document snapshot;
        expect(DocumentFile::readSnapshot(path,snapshot).ok(),"file allocation: snapshot");
        if (!result.ok()) {
            ++rejected; const auto after=h.statistics();
            expect(fault::fired && owner.isOpen() && owner.revision()==revision &&
                   encodeIisc(*owner.document()).bytes==before && encodeIisc(snapshot).bytes==before &&
                   h.canUndo()==canUndo && h.canRedo()==canRedo && after.undoEntries==stats.undoEntries &&
                   after.redoEntries==stats.redoEntries && after.retainedBytes==stats.retainedBytes &&
                   findRasterAsset(*owner.document(),"back")->pixels.pixels.data()==basePixels,
                   "file allocation: failure preserves memory, durable bytes and cursor");
        } else {
            ++accepted;
            expect(result.changed && owner.revision()==revision+1 &&
                   encodeIisc(snapshot).bytes==encodeIisc(*owner.document()).bytes && reverse(h,op),
                   "file allocation: success publishes one durable reversible command");
        }
    }
    expect(rejected>0 && accepted>0,"file allocation: failures and successes covered");
    std::cout << "COMPOUND_FILE_ALLOCATION operation=" << static_cast<int>(op)
              << " rejected=" << rejected << " accepted=" << accepted << '\n';
}
}
int main(int argc,char **argv) {
    std::cout << std::unitbuf;
    constexpr std::array<std::string_view,6> names{"timing","timingundo","timingredo","remove","removeundo","removeredo"};
    constexpr std::array operations{Operation::Timing,Operation::TimingUndo,Operation::TimingRedo,
                                    Operation::Remove,Operation::RemoveUndo,Operation::RemoveRedo};
    const auto selection=allocation_test::parse(argc,argv,names);
    if (!selection) { std::cerr << "invalid allocation test arguments\n"; return 2; }
    for (std::size_t i=0;i<operations.size();++i) {
        if (selection->operation && *selection->operation!=i) continue;
        if (selection->memory) memory(operations[i]);
        if (selection->file) file(operations[i]);
    }
    return failures?1:0;
}
