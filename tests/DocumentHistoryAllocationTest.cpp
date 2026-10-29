#include <Bitmap/BitmapEditor.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>
#include <iiFileProviderNative.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <string>

namespace fault {
thread_local std::ptrdiff_t remaining = -1;
thread_local bool fired = false;
}
// Fault injection belongs to this test executable alone. Only the selected
// allocation fails, so the implementation can report and roll back normally.
void *operator new(std::size_t size) {
    if (fault::remaining == 0) {
        fault::remaining = -1;
        fault::fired = true;
        throw std::bad_alloc();
    }
    if (fault::remaining > 0) --fault::remaining;
    if (auto *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
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
Document makeDocument(bool chunked=false) {
    Document d; d.extent={32,32};
    if (chunked) {
        d.canvasMode=CanvasMode::Infinite; d.infiniteCanvas.chunkSize=32;
        d.assets.emplace_back(ChunkedRasterAsset{"paint",{}});
    } else d.assets.emplace_back(RasterAsset{"paint",makeRasterLayer(32,32)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer","Original"},StaticSource{"paint"}});
    d.layers.emplace_back(StaticBitmapLayer{{"second","Second"},StaticSource{"paint"}});
    return d;
}
enum class Operation { Name, Order, Bitmap, Chunked, Insert, InsertKeys, Remove, RemoveKeys };
bool structural(Operation op) { return op==Operation::Insert || op==Operation::InsertKeys || op==Operation::Remove || op==Operation::RemoveKeys; }
void dynamicLayer(Document &d) {
    d.timeline.frameCount=4;
    d.layers.emplace_back(DynamicBitmapLayer{{"dynamic","Dynamic"},KeyframedSource{{0,3}}});
    d.frames={{0,{{"dynamic","paint"}}},{3,{{"dynamic","paint"}}}};
}
DocumentHistoryResult structure(DocumentHistory &history,Operation op) {
    switch (op) {
    case Operation::Insert: return history.insertLayer(StaticBitmapLayer{{"new","New"},StaticSource{"paint"}},1);
    case Operation::InsertKeys: return history.insertKeyframedLayer(
        DynamicBitmapLayer{{"new","New"},KeyframedSource{}},{{0,"paint"},{3,"paint"}},1);
    case Operation::Remove: return history.removeLayer("second");
    case Operation::RemoveKeys: return history.removeLayer("dynamic");
    default: return {DocumentHistoryCode::InvalidArgument};
    }
}
void memory(Operation operation) {
    std::size_t rejected=0, accepted=0;
    for (std::ptrdiff_t allocation=0; allocation<(structural(operation)?256:128); ++allocation) {
        auto d=makeDocument(operation==Operation::Chunked);
        if (operation==Operation::InsertKeys || operation==Operation::RemoveKeys) dynamicLayer(d);
        DocumentHistory history(d);
        expect(history.setLayerVisible("layer",false).changed && history.undo().changed,
               "setup: a redo branch exists");
        const auto before=encodeIisc(d).bytes;
        const auto revision=d.authorship.revision();
        const auto stats=history.statistics();
        const std::array<std::uint32_t,1> value{0xff123456};
        const std::array patch{BitmapPatch{{{1,1},1,1},value}};
        const std::string name(140,'N');
        fault::fired=false; fault::remaining=allocation;
        DocumentHistoryResult result;
        try {
            if (operation==Operation::Name) result=history.setLayerName("layer",name);
            else if (operation==Operation::Order) result=history.moveLayer("second",0);
            else if (structural(operation)) result=structure(history,operation);
            else result=history.replaceBitmapPatches("paint",patch,"Atomic pixel command");
        } catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        if (!result.ok()) {
            ++rejected;
            const auto after=history.statistics();
            expect(fault::fired && encodeIisc(d).bytes==before && d.authorship.revision()==revision &&
                   after.undoEntries==stats.undoEntries && after.redoEntries==stats.redoEntries &&
                   after.retainedBytes==stats.retainedBytes && history.canRedo(),
                   "allocation: rejected edit preserves full document and redo cursor");
        } else {
            ++accepted;
            expect(result.changed && result.discardedRedoEntries==1 && !history.canRedo() &&
                   history.statistics().undoEntries==1 && history.undo().changed,
                   "allocation: successful edit publishes and undoes exactly once");
        }
    }
    expect(rejected>=5 && accepted>0, "allocation: failures and successful publication covered");
    std::cout << "HISTORY_ALLOCATION operation=" << static_cast<int>(operation)
              << " rejected=" << rejected << " successful=" << accepted << '\n';
}
void replay(Operation operation,bool redo) {
    std::size_t rejected=0,accepted=0;
    for (std::ptrdiff_t allocation=0; allocation<256; ++allocation) {
        auto d=makeDocument();
        if (operation==Operation::InsertKeys || operation==Operation::RemoveKeys) dynamicLayer(d);
        DocumentHistory history(d);
        if (!structure(history,operation).changed || (redo && !history.undo().changed)) {
            expect(false,"structure replay allocation: setup");
            return;
        }
        const auto before=encodeIisc(d).bytes;
        const auto revision=d.authorship.revision();
        const auto stats=history.statistics();
        const bool couldUndo=history.canUndo(),couldRedo=history.canRedo();
        fault::fired=false; fault::remaining=allocation;
        DocumentHistoryResult result;
        try { result=redo ? history.redo() : history.undo(); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        if (!result.ok()) {
            ++rejected;
            const auto after=history.statistics();
            expect(fault::fired && encodeIisc(d).bytes==before && d.authorship.revision()==revision &&
                   after.undoEntries==stats.undoEntries && after.redoEntries==stats.redoEntries &&
                   after.retainedBytes==stats.retainedBytes && history.canUndo()==couldUndo &&
                   history.canRedo()==couldRedo,
                   "structure replay allocation: failure preserves document and history cursor");
        } else {
            ++accepted;
            expect(result.changed && (redo ? history.canUndo() : history.canRedo()) &&
                   (redo ? history.undo().changed : history.redo().changed),
                   "structure replay allocation: success moves one entry and can be reversed");
        }
    }
    expect(rejected>0 && accepted>0,"structure replay allocation: failure and success covered");
    std::cout << "HISTORY_STRUCTURE_REPLAY_ALLOCATION operation=" << static_cast<int>(operation)
              << " direction=" << (redo ? "redo" : "undo") << " rejected=" << rejected
              << " successful=" << accepted << '\n';
}
void fileStructure(Operation operation) {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    expect(temporary.isValid(),"file structure allocation: temporary directory");
    constexpr std::array<std::ptrdiff_t,6> positions{0,3,16,64,256,1024};
    std::size_t rejected=0,accepted=0;
    for (std::size_t i=0;i<positions.size();++i) {
        const auto path=(temporary.path()/("structure-"+std::to_string(i)+".iisc")).string();
        auto d=makeDocument();
        if (operation==Operation::InsertKeys || operation==Operation::RemoveKeys) dynamicLayer(d);
        DocumentFile owner;
        expect(owner.create(path,d).ok(),"file structure allocation: create");
        DocumentHistory history(owner);
        expect(history.setLayerVisible("layer",false).changed && history.undo().changed,
               "file structure allocation: create redo");
        const auto revision=owner.revision(); const auto stats=history.statistics();
        const auto stable=encodeIisc(*owner.document()).bytes;
        fault::remaining=positions[i]; fault::fired=false;
        DocumentHistoryResult result;
        try { result=structure(history,operation); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        Document snapshot;
        expect(DocumentFile::readSnapshot(path,snapshot).ok(),"file structure allocation: read durable state");
        if (!result.ok()) {
            ++rejected; const auto after=history.statistics();
            expect(fault::fired && owner.isOpen() && owner.revision()==revision &&
                   encodeIisc(*owner.document()).bytes==stable && encodeIisc(snapshot).bytes==stable &&
                   after.undoEntries==stats.undoEntries && after.redoEntries==stats.redoEntries &&
                   after.retainedBytes==stats.retainedBytes && history.canRedo(),
                   "file structure allocation: failure preserves durable state and redo");
        } else {
            ++accepted;
            expect(result.changed && result.discardedRedoEntries==1 && owner.revision()==revision+1 &&
                   encodeIisc(snapshot).bytes==encodeIisc(*owner.document()).bytes && history.undo().changed,
                   "file structure allocation: success publishes and undoes one command");
        }
    }
    expect(rejected>0 && accepted>0,"file structure allocation: rejected and accepted paths covered");
    std::cout << "HISTORY_STRUCTURE_FILE_ALLOCATION operation=" << static_cast<int>(operation)
              << " rejected=" << rejected << " successful=" << accepted << '\n';
}
void file() {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    expect(temporary.isValid(),"file allocation: temporary directory");
    constexpr std::array<std::ptrdiff_t,10> positions{0,1,3,8,16,32,64,96,128,256};
    std::size_t rejected=0,accepted=0;
    for (std::size_t i=0;i<positions.size();++i) {
        const auto path=(temporary.path()/("allocation-"+std::to_string(i)+".iisc")).string();
        auto d=makeDocument(); DocumentFile owner;
        expect(owner.create(path,d).ok(), "file allocation: create");
        DocumentHistory history(owner);
        expect(history.setLayerVisible("layer",false).changed && history.undo().changed,
               "file allocation: create redo");
        const auto revision=owner.revision(); const auto stats=history.statistics();
        const auto stable=encodeIisc(*owner.document()).bytes;
        const std::string name(140,'F');
        fault::remaining=positions[i]; fault::fired=false;
        DocumentHistoryResult result;
        try { result=history.setLayerName("layer",name); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        Document snapshot;
        expect(DocumentFile::readSnapshot(path,snapshot).ok(), "file allocation: read durable state");
        if (!result.ok()) {
            ++rejected;
            const auto after=history.statistics();
            expect(fault::fired && owner.revision()==revision &&
                   encodeIisc(snapshot).bytes==stable && after.undoEntries==stats.undoEntries &&
                   after.redoEntries==stats.redoEntries && after.retainedBytes==stats.retainedBytes,
                   "file allocation: failure preserves durable document and redo cursor");
        } else {
            ++accepted;
            expect(result.changed && result.discardedRedoEntries==1 &&
                   layerProperties(*findLayer(snapshot,"layer")).name==name,
                   "file allocation: success publishes durable edit and cursor together");
        }
    }
    expect(rejected>0 && accepted>0, "file allocation: rejected and accepted paths covered");
    std::cout << "HISTORY_FILE_ALLOCATION rejected=" << rejected << " successful=" << accepted << '\n';
}
}
int main(int argc,char **argv) {
    std::cout << std::unitbuf;
    if (argc>1 && std::string_view(argv[1])=="--structure-replay") {
        for (auto operation:{Operation::Insert,Operation::InsertKeys,Operation::Remove,Operation::RemoveKeys}) {
            replay(operation,false); replay(operation,true);
        }
        return failures ? 1 : 0;
    }
    memory(Operation::Name); memory(Operation::Order); memory(Operation::Bitmap); memory(Operation::Chunked);
    for (auto operation:{Operation::Insert,Operation::InsertKeys,Operation::Remove,Operation::RemoveKeys}) {
        memory(operation); fileStructure(operation);
        replay(operation,false); replay(operation,true);
    }
    file();
    return failures ? 1 : 0;
}
