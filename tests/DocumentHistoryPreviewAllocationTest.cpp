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
enum class Operation { Begin, Replace, Cancel };
void memory(Operation operation) {
    std::size_t failed=0,passed=0;
    for (std::ptrdiff_t position=0; position<128; ++position) {
        auto d=fixture(); DocumentHistory h(d);
        expect(h.setLayerName("layer","Ink").changed && h.undo().changed,"allocation: redo setup");
        const std::array<std::uint32_t,1> a{0x80112233},b{0xffabcdef};
        const std::array first{BitmapPatch{{{1,1},1,1},a}}, second{BitmapPatch{{{33,33},1,1},b}};
        const std::string label(128,'L');
        if (operation!=Operation::Begin)
            expect(h.beginPixelPreview("paint",label).ok() && h.replacePixelPreview(first).changed,
                   "allocation: preview setup");
        const auto stable=encodeIisc(d).bytes; const auto revision=d.authorship.revision();
        const auto stats=h.statistics(); const auto cache=h.previewStorageBytes();
        const auto pending=h.previewPendingHistoryBytes();
        fault::remaining=position; fault::fired=false;
        DocumentHistoryResult result;
        try {
            if (operation==Operation::Begin) result=h.beginPixelPreview("paint",label);
            else if (operation==Operation::Replace) result=h.replacePixelPreview(second);
            else result=h.cancelPixelPreview();
        } catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        if (!result.ok()) {
            ++failed;
            const auto now=h.statistics();
            expect(fault::fired && encodeIisc(d).bytes==stable && d.authorship.revision()==revision &&
                   now.undoEntries==stats.undoEntries && now.redoEntries==stats.redoEntries &&
                   now.retainedBytes==stats.retainedBytes && h.previewStorageBytes()==cache &&
                   h.previewPendingHistoryBytes()==pending &&
                   h.pixelPreviewActive()==(operation!=Operation::Begin),
                   "allocation: failure preserves document, pending preview and global cursor");
        } else {
            ++passed;
            expect(operation==Operation::Begin ? h.pixelPreviewActive() : result.changed,
                   "allocation: successful operation publishes its expected state");
        }
        if (h.pixelPreviewActive()) expect(h.cancelPixelPreview().ok(),"allocation: session remains resolvable");
        expect(h.canRedo() && h.statistics().redoEntries==1,"allocation: redo survives cancellation");
    }
    expect(failed>=5 && passed>0,"allocation: failures and successes covered");
    std::cout << "DOCUMENT_PREVIEW_ALLOCATION operation=" << static_cast<int>(operation)
              << " rejected=" << failed << " successful=" << passed << '\n';
}
void file() {
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory tmp(parent);
    expect(tmp.isValid(),"file allocation: temporary directory");
    constexpr std::array<std::ptrdiff_t,8> positions{0,1,4,10,30,70,150,256};
    std::size_t failed=0,passed=0;
    for (std::size_t i=0;i<positions.size();++i) {
        const auto path=(tmp.path()/(std::to_string(i)+".iisc")).string();
        auto d=fixture(); DocumentFile owner; expect(owner.create(path,d).ok(),"file allocation: create");
        DocumentHistory h(owner); const std::array<std::uint32_t,1> a{0x80112233},b{0xffabcdef};
        const std::array first{BitmapPatch{{{1,1},1,1},a}}, second{BitmapPatch{{{33,33},1,1},b}};
        expect(h.setLayerName("layer","Ink").changed && h.undo().changed &&
               h.beginPixelPreview("paint").ok() && h.replacePixelPreview(first).changed,
               "file allocation: preview and redo setup");
        const auto stable=encodeIisc(*owner.document()).bytes; const auto stats=h.statistics();
        const auto cache=h.previewStorageBytes(), pending=h.previewPendingHistoryBytes();
        fault::remaining=positions[i]; fault::fired=false;
        DocumentHistoryResult result;
        try { result=h.replacePixelPreview(second); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        Document snapshot; expect(DocumentFile::readSnapshot(path,snapshot).ok(),"file allocation: read durable document");
        if (!result.ok()) {
            ++failed; const auto now=h.statistics();
            expect(fault::fired && encodeIisc(snapshot).bytes==stable && h.pixelPreviewActive() &&
                   now.undoEntries==stats.undoEntries && now.redoEntries==stats.redoEntries &&
                   now.retainedBytes==stats.retainedBytes && h.previewStorageBytes()==cache &&
                   h.previewPendingHistoryBytes()==pending,
                   "file allocation: failed replacement preserves durable preview and cursor");
        } else {
            ++passed;
            const auto *r=findRasterAsset(snapshot,"paint");
            expect(result.changed && r->pixels.pixels[1*64+1]==0 && r->pixels.pixels[33*64+33]==b[0] &&
                   h.statistics().redoEntries==1,"file allocation: success publishes complete replacement");
        }
        if (owner.isOpen()) expect(h.cancelPixelPreview().ok() && h.canRedo(),"file allocation: retry cancel");
        else expect(h.abandonPixelPreview().ok(),"file allocation: release detached session");
    }
    expect(failed>0 && passed>0,"file allocation: rejected and accepted paths covered");
    std::cout << "DOCUMENT_PREVIEW_FILE_ALLOCATION rejected=" << failed << " successful=" << passed << '\n';
}
}
int main() {
    memory(Operation::Begin); memory(Operation::Replace); memory(Operation::Cancel); file();
    return failures?1:0;
}
