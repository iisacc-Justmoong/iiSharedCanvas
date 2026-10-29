#include <Bitmap/BitmapEditor.h>
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
void *operator new(std::size_t n) {
    if (fault::remaining==0) { fault::remaining=-1; fault::fired=true; throw std::bad_alloc(); }
    if (fault::remaining>0) --fault::remaining;
    if (auto *p=std::malloc(n?n:1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }
void operator delete[](void *p,std::size_t) noexcept { std::free(p); }

namespace {
using namespace iiSharedCanvas;
int failures=0;
void expect(bool value,const char *message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
Document fixture() {
    Document d; d.extent={64,64};
    d.assets.emplace_back(RasterAsset{"paint",makeRasterLayer(64,64)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer","Paint"},StaticSource{"paint"}});
    return d;
}
BitmapBrush brush() { BitmapBrush b; b.size=5; b.argb=0xff336699; return b; }
StrokePoint sample(double x,double y,double time) {
    StrokePoint p; p.position={x,y}; p.time=time; return p;
}
enum class Operation { Begin, Continue, End, Cancel };
DocumentHistoryResult invoke(DocumentHistory &h,Operation operation) {
    switch (operation) {
    case Operation::Begin: return h.beginStroke("paint",brush(),sample(8,8,1),"Allocation test");
    case Operation::Continue: return h.continueStroke(sample(22,22,2));
    case Operation::End: return h.endStroke(sample(22,22,2));
    case Operation::Cancel: return h.cancelStroke();
    }
    return {DocumentHistoryCode::InvalidArgument};
}
bool sameBounds(DevicePixelRect a,DevicePixelRect b) {
    return a.origin.x==b.origin.x && a.origin.y==b.origin.y &&
           a.width==b.width && a.height==b.height;
}
void prepare(DocumentHistory &h,Operation operation) {
    expect(h.setLayerName("layer","Ink").changed && h.undo().changed,"allocation: establish redo");
    if (operation!=Operation::Begin)
        expect(h.beginStroke("paint",brush(),sample(8,8,1),"Allocation test").changed,
               "allocation: establish active stroke");
}
void memory(Operation operation) {
    std::size_t rejected=0,accepted=0;
    for (std::ptrdiff_t position=0;position<512;++position) {
        auto d=fixture(); DocumentHistory h(d); prepare(h,operation);
        const auto stable=encodeIisc(d).bytes; const auto revision=d.authorship.revision();
        const auto stats=h.statistics();
        const auto cache=h.strokeStorageBytes(), pending=h.strokePendingHistoryBytes();
        const auto dirty=h.strokeDirtyBounds();
        fault::remaining=position; fault::fired=false;
        DocumentHistoryResult result;
        try { result=invoke(h,operation); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        if (!result.ok()) {
            ++rejected; const auto now=h.statistics();
            expect(fault::fired && encodeIisc(d).bytes==stable && d.authorship.revision()==revision &&
                   now.undoEntries==stats.undoEntries && now.redoEntries==stats.redoEntries &&
                   now.retainedBytes==stats.retainedBytes && h.strokeStorageBytes()==cache &&
                   h.strokePendingHistoryBytes()==pending && sameBounds(h.strokeDirtyBounds(),dirty) &&
                   h.strokeActive()==(operation!=Operation::Begin),
                   "allocation: failed operation preserves document, pending stroke and cursor");
        } else {
            ++accepted;
            expect(result.changed && (operation==Operation::End ? h.canUndo() : true),
                   "allocation: successful operation reaches its expected state");
        }
        if (h.strokeActive()) expect(h.cancelStroke().ok(),"allocation: pending stroke can be cancelled");
        if (operation!=Operation::End || !result.ok())
            expect(h.canRedo() && h.statistics().redoEntries==1,"allocation: redo survives rejection or cancel");
        else
            expect(h.undo().changed && h.redo().changed,"allocation: completed stroke remains reversible");
    }
    expect(rejected>=5 && accepted>0,"allocation: both rejected and accepted positions exercised");
    std::cout << "DOCUMENT_STROKE_MEMORY_ALLOCATION operation=" << static_cast<int>(operation)
              << " rejected=" << rejected << " accepted=" << accepted << '\n';
}
void file(Operation operation) {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory tmp(parent);
    if (!tmp.isValid()) { expect(false,"file allocation: temporary directory"); return; }
    constexpr std::array<std::ptrdiff_t,8> positions{0,1,4,10,30,70,150,384};
    std::size_t rejected=0,accepted=0;
    for (std::size_t i=0;i<positions.size();++i) {
        const auto path=(tmp.path()/(std::to_string(i)+".iisc")).string();
        auto d=fixture(); DocumentFile owner;
        if (!owner.create(path,d).ok()) { expect(false,"file allocation: create"); return; }
        DocumentHistory h(owner); prepare(h,operation);
        const auto stable=encodeIisc(*owner.document()).bytes; const auto stats=h.statistics();
        const auto cache=h.strokeStorageBytes(), pending=h.strokePendingHistoryBytes();
        const auto dirty=h.strokeDirtyBounds(); const auto revision=owner.revision();
        fault::remaining=positions[i]; fault::fired=false;
        DocumentHistoryResult result;
        try { result=invoke(h,operation); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        Document snapshot;
        expect(DocumentFile::readSnapshot(path,snapshot).ok(),"file allocation: inspect durable document");
        if (!result.ok()) {
            ++rejected; const auto now=h.statistics();
            expect(fault::fired && encodeIisc(snapshot).bytes==stable &&
                   now.undoEntries==stats.undoEntries && now.redoEntries==stats.redoEntries &&
                   now.retainedBytes==stats.retainedBytes && h.strokeStorageBytes()==cache &&
                   h.strokePendingHistoryBytes()==pending && sameBounds(h.strokeDirtyBounds(),dirty) &&
                   (!owner.isOpen() || (owner.revision()==revision && encodeIisc(*owner.document()).bytes==stable)) &&
                   h.strokeActive()==(operation!=Operation::Begin),
                   "file allocation: failure preserves durable pixels, pending stroke and cursor");
        } else {
            ++accepted;
            expect(result.changed,"file allocation: successful operation changes expected state");
        }
        if (h.strokeActive()) {
            if (owner.isOpen()) expect(h.cancelStroke().ok(),"file allocation: retry cancellation");
            else expect(h.abandonStroke().ok(),"file allocation: release closed binding");
        }
        if (owner.isOpen() && (operation!=Operation::End || !result.ok()))
            expect(h.canRedo(),"file allocation: redo remains after rejection or cancel");
        else if (owner.isOpen())
            expect(h.undo().changed && h.redo().changed,"file allocation: completed stroke remains reversible");
    }
    expect(rejected>0 && accepted>0,"file allocation: rejected and accepted samples exercised");
    std::cout << "DOCUMENT_STROKE_FILE_ALLOCATION operation=" << static_cast<int>(operation)
              << " rejected=" << rejected << " accepted=" << accepted << '\n';
}
}
int main(int argc,char **argv) {
    constexpr std::array<std::string_view,4> names{"begin","continue","end","cancel"};
    constexpr std::array operations{Operation::Begin,Operation::Continue,Operation::End,Operation::Cancel};
    const auto selection=allocation_test::parse(argc,argv,names);
    if (!selection) { std::cerr << "invalid allocation test arguments\n"; return 2; }
    for (std::size_t i=0;i<operations.size();++i) {
        if (selection->operation && *selection->operation!=i) continue;
        if (selection->memory) memory(operations[i]);
        if (selection->file) file(operations[i]);
    }
    return failures?1:0;
}
