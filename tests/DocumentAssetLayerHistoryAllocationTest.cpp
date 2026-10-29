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
enum class Operation { Insert, Undo, Redo };
int failures=0;
void expect(bool value,std::string_view message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
Document fixture() {
    Document d; d.extent={32,32};
    d.assets.emplace_back(RasterAsset{"base-asset",makeRasterLayer(32,32,0xff123456)});
    d.layers.emplace_back(StaticBitmapLayer{{"base","Base"},StaticSource{"base-asset"}});
    return d;
}
DocumentHistoryResult insert(DocumentHistory &h) {
    auto pixels=makeRasterLayer(32,32,0);
    for (std::size_t i=0;i<pixels.pixels.size();i+=7) pixels.pixels[i]=0xff00ff00;
    return h.insertLayerWithAsset(RasterAsset{"created",std::move(pixels)},
        StaticBitmapLayer{{"added","Added"},StaticSource{"created"}},0,0,"Create");
}
bool setup(DocumentHistory &history,Operation operation) {
    if (operation==Operation::Insert)
        return history.setLayerVisible("base",false).changed && history.undo().changed;
    if (!insert(history).changed) return false;
    return operation==Operation::Undo || history.undo().changed;
}
DocumentHistoryResult perform(DocumentHistory &history,Operation operation) {
    if (operation==Operation::Insert) return insert(history);
    return operation==Operation::Undo ? history.undo() : history.redo();
}
void memory(Operation operation) {
    std::size_t rejected=0,accepted=0;
    for (std::ptrdiff_t at=0;at<384;++at) {
        auto d=fixture(); DocumentHistory h(d);
        if (!setup(h,operation)) { expect(false,"memory allocation: setup"); return; }
        const auto before=encodeIisc(d).bytes; const auto revision=d.authorship.revision();
        const auto stats=h.statistics(); const bool canUndo=h.canUndo(),canRedo=h.canRedo();
        const auto *baseAsset=findAsset(d,"base-asset");
        const auto *basePixels=findRasterAsset(d,"base-asset")->pixels.pixels.data();
        fault::fired=false; fault::remaining=at;
        DocumentHistoryResult result;
        try { result=perform(h,operation); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        if (!result.ok()) {
            ++rejected; const auto after=h.statistics();
            expect(fault::fired && encodeIisc(d).bytes==before && d.authorship.revision()==revision &&
                   h.canUndo()==canUndo && h.canRedo()==canRedo && after.undoEntries==stats.undoEntries &&
                   after.redoEntries==stats.redoEntries && after.retainedBytes==stats.retainedBytes &&
                   findAsset(d,"base-asset")==baseAsset &&
                   findRasterAsset(d,"base-asset")->pixels.pixels.data()==basePixels,
                   "memory allocation: rejected command restores document, pointers and cursor");
        } else {
            ++accepted;
            const bool reversed=operation==Operation::Undo ? h.redo().changed : h.undo().changed;
            expect(result.changed && reversed,"memory allocation: success publishes one reversible command");
        }
    }
    expect(rejected>0 && accepted>0,"memory allocation: failures and successes covered");
    std::cout << "ASSET_LAYER_MEMORY_ALLOCATION operation=" << static_cast<int>(operation)
              << " rejected=" << rejected << " accepted=" << accepted << '\n';
}
void file(Operation operation) {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent);
    iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false,"file allocation: temporary directory"); return; }
    constexpr std::array<std::ptrdiff_t,8> positions{0,1,4,16,64,128,512,2048};
    std::size_t rejected=0,accepted=0;
    for (std::size_t i=0;i<positions.size();++i) {
        const auto path=(temporary.path()/(std::to_string(i)+".iisc")).string();
        auto d=fixture(); DocumentFile owner;
        if (!owner.create(path,d).ok()) { expect(false,"file allocation: create"); return; }
        DocumentHistory h(owner);
        if (!setup(h,operation)) { expect(false,"file allocation: setup"); return; }
        const auto before=encodeIisc(*owner.document()).bytes; const auto revision=owner.revision();
        const auto stats=h.statistics(); const bool canUndo=h.canUndo(),canRedo=h.canRedo();
        const auto *basePixels=findRasterAsset(*owner.document(),"base-asset")->pixels.pixels.data();
        fault::fired=false; fault::remaining=positions[i];
        DocumentHistoryResult result;
        try { result=perform(h,operation); }
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
                   findRasterAsset(*owner.document(),"base-asset")->pixels.pixels.data()==basePixels,
                   "file allocation: failed command preserves memory, durable bytes and cursor");
        } else {
            ++accepted;
            expect(result.changed && owner.revision()==revision+1 &&
                   encodeIisc(snapshot).bytes==encodeIisc(*owner.document()).bytes &&
                   (operation==Operation::Undo ? h.redo().changed : h.undo().changed),
                   "file allocation: success publishes one durable reversible command");
        }
    }
    expect(rejected>0 && accepted>0,"file allocation: failures and successes covered");
    std::cout << "ASSET_LAYER_FILE_ALLOCATION operation=" << static_cast<int>(operation)
              << " rejected=" << rejected << " accepted=" << accepted << '\n';
}
}
int main(int argc,char **argv) {
    std::cout << std::unitbuf;
    const bool memoryOnly=argc>1 && std::string_view(argv[1])=="--memory-only";
    for (auto operation:{Operation::Insert,Operation::Undo,Operation::Redo}) {
        memory(operation);
        if (!memoryOnly) file(operation);
    }
    return failures ? 1 : 0;
}
