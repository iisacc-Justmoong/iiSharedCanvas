#include <iiSharedCanvas>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#if defined(__APPLE__)
#include <dlfcn.h>
#include <execinfo.h>
#endif

// Fail one selected allocation at a time so rollback can still complete.
namespace fault { thread_local std::ptrdiff_t remaining = -1; thread_local bool fired = false; }
// The installed iiFileProvider authorship parser constructs libc++ regexes;
// injected failures there can corrupt the external regex implementation.
// Exercise this module's allocation sites without injecting into that parser.
bool externalAuthorshipParse() noexcept
{
#if defined(__APPLE__)
    void *frames[64];
    const int count = backtrace(frames, 64);
    for (int i = 0; i < count; ++i) {
        Dl_info info{};
        if (dladdr(frames[i], &info) && info.dli_sname &&
            std::strstr(info.dli_sname, "Authorship8fromDump")) return true;
    }
#endif
    return false;
}
void *operator new(std::size_t size)
{
    if (fault::remaining == 0 && !externalAuthorshipParse()) {
        fault::remaining = -1; fault::fired = true; throw std::bad_alloc();
    }
    if (fault::remaining > 0) --fault::remaining;
    if (void *value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool condition, const char *message)
{
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
Document sample(std::uint32_t pixel)
{
    Document d; d.extent = {2, 2};
    d.assets.emplace_back(RasterAsset{"pixels", makeRasterLayer(2, 2, pixel)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Layer"}, StaticSource{"pixels"}});
    return d;
}
void insertion()
{
    const auto asset = LinkedAssetResolver::copy("linked", sample(0xff112233), 0).asset;
    std::size_t rejected = 0, accepted = 0;
    for (std::ptrdiff_t n = 0; n < 192; ++n) {
        auto document = sample(0); DocumentEditor editor(document);
        const auto before = encodeIisc(document).bytes;
        const auto revision = document.authorship.revision();
        fault::fired = false; fault::remaining = n;
        bool success = false;
        try { success = editor.insertLinkedCanvasAsset(asset).changed; }
        catch (const std::bad_alloc &) {}
        fault::remaining = -1;
        if (!success) {
            ++rejected;
            expect(fault::fired && encodeIisc(document).bytes == before &&
                   document.authorship.revision() == revision && !findAsset(document, "linked"),
                   "failed linked insertion must preserve the whole document");
        } else {
            ++accepted;
            expect(findAsset(document, "linked") && document.authorship.revision() == revision + 1,
                   "successful linked insertion publishes once");
        }
    }
    expect(rejected > 0 && accepted > 0, "insertion must exercise both failures and success");
}
void replacement()
{
    const auto first = LinkedAssetResolver::copy("linked", sample(0xff112233), 0).asset;
    const auto next = LinkedAssetResolver::copy("linked", sample(0xff445566), 0).asset;
    std::size_t rejected = 0, accepted = 0;
    for (std::ptrdiff_t n = 0; n < 256; ++n) {
        auto document = sample(0); document.assets.emplace_back(first);
        DocumentHistory history(document);
        expect(history.setLayerName("layer", "Changed").changed && history.undo().changed,
               "prepare a redo branch");
        const auto before = encodeIisc(document).bytes;
        const auto revision = document.authorship.revision();
        const auto stats = history.statistics();
        fault::fired = false; fault::remaining = n;
        DocumentHistoryResult result;
        try { result = history.replaceLinkedCanvasAsset(next); }
        catch (const std::bad_alloc &) { result.code = DocumentHistoryCode::LimitExceeded; }
        fault::remaining = -1;
        if (!result.ok()) {
            ++rejected;
            const auto after = history.statistics();
            expect(fault::fired && encodeIisc(document).bytes == before &&
                   document.authorship.revision() == revision && history.canRedo() &&
                   after.undoEntries == stats.undoEntries && after.redoEntries == stats.redoEntries &&
                   after.retainedBytes == stats.retainedBytes,
                   "failed linked replacement must preserve document and redo");
        } else {
            ++accepted;
            expect(result.changed && history.undo().changed && encodeIisc(document).ok(),
                   "successful linked replacement publishes and undoes once");
        }
    }
    expect(rejected > 0 && accepted > 0, "replacement must exercise both failures and success");
}
void proceduralReplacement()
{
    // Invalid-content reporting itself can allocate after candidate validation;
    // every failure point must leave the original asset and authorship intact.
    std::size_t injected=0;
    for (std::ptrdiff_t n=0;n<192;++n) {
        auto document=sample(0); FillLayerContent fill; fill.id="fill"; fill.extent={2,2};
        document.assets.emplace_back(fill);
        document.layers.emplace_back(StaticBitmapLayer{{"fill-layer","Fill"},StaticSource{"fill"}});
        DocumentEditor editor(document);
        auto invalid=fill; invalid.extent.width=0;
        const auto before=encodeIisc(document).bytes; const auto revision=document.authorship.revision();
        fault::fired=false; fault::remaining=n;
        try { (void)editor.replaceProceduralContent(invalid); }
        catch (const std::bad_alloc &) {}
        fault::remaining=-1;
        if (fault::fired) ++injected;
        expect(encodeIisc(document).bytes==before && document.authorship.revision()==revision,
               "invalid procedural replacement must roll back at every allocation failure");
    }
    expect(injected>0,"procedural replacement exercised allocation failures");
}
void appearanceReplacement()
{
    std::size_t injected=0;
    for (std::ptrdiff_t n=0;n<192;++n) {
        auto document=sample(0); DocumentEditor editor(document);
        auto invalid=layerProperties(document.layers[0]).appearance;
        invalid.fillOpacity=2;
        const auto before=encodeIisc(document).bytes; const auto revision=document.authorship.revision();
        fault::fired=false; fault::remaining=n;
        try { (void)editor.setLayerAppearance("layer",invalid); }
        catch (const std::bad_alloc &) {}
        fault::remaining=-1;
        if (fault::fired) ++injected;
        expect(encodeIisc(document).bytes==before && document.authorship.revision()==revision,
               "invalid appearance replacement must roll back at every allocation failure");
    }
    expect(injected>0,"appearance replacement exercised allocation failures");
}
void patternRename()
{
    std::size_t failed=0,successes=0;
    const std::string next="renamed-pattern-with-enough-characters-to-require-allocation";
    for (std::ptrdiff_t n=0;n<192;++n) {
        auto document=sample(0xff123456); FillLayerContent fill; fill.id="fill"; fill.extent={2,2};
        fill.kind=FillLayerKind::Pattern; fill.patternAssetId="pixels";
        document.assets.emplace_back(fill);
        document.layers.emplace_back(StaticBitmapLayer{{"fill-layer","Fill"},StaticSource{"fill"}});
        DocumentEditor editor(document); const auto before=encodeIisc(document).bytes;
        const auto revision=document.authorship.revision();
        fault::fired=false; fault::remaining=n; bool changed=false;
        try { changed=editor.renameAsset("pixels",next).changed; }
        catch (const std::bad_alloc &) {}
        fault::remaining=-1;
        if (!changed) {
            ++failed;
            expect(fault::fired && encodeIisc(document).bytes==before && document.authorship.revision()==revision,
                   "failed pattern rename must restore asset and all references");
        } else {
            ++successes;
            const auto *content=std::get_if<FillLayerContent>(findAsset(document,"fill"));
            expect(content && content->patternAssetId==next && findRasterAsset(document,next),
                   "successful pattern rename retargets all references");
        }
    }
    expect(failed && successes,"pattern rename exercised both failure and success");
}
// Inject failures through snapshot preparation, multi-asset movement,
// validation and authorship publication; an existing redo branch must survive.
void flattenTransaction()
{
    std::size_t rejected=0,accepted=0;
    for (std::ptrdiff_t n=0;n<512;++n) {
        auto document=sample(0xff102030);
        document.assets.emplace_back(RasterAsset{"second",makeRasterLayer(2,2,0x80ff0000)});
        document.layers.emplace_back(StaticBitmapLayer{{"second-layer","Second"},StaticSource{"second"}});
        DocumentHistory history(document);
        expect(history.setLayerName("layer","Changed").changed && history.undo().changed,"prepare flatten redo branch");
        const std::vector<FrameRenderResult> frames{renderFrame(document,0)};
        auto plan=DocumentFlattenTransaction{}(document,frames,"flat","flat-asset");
        expect(plan.ok(),"prepare flatten allocation plan");
        const auto before=encodeIisc(document).bytes; const auto revision=document.authorship.revision();
        const auto stats=history.statistics();
        fault::fired=false; fault::remaining=n; DocumentHistoryResult result;
        try { result=history.applyLayerTransaction(std::move(plan.plan)); }
        catch (const std::bad_alloc &) { result.code=DocumentHistoryCode::LimitExceeded; }
        fault::remaining=-1;
        if (!result.ok()) {
            ++rejected; const auto after=history.statistics();
            expect(fault::fired && encodeIisc(document).bytes==before && document.authorship.revision()==revision &&
                   history.canRedo() && after.undoEntries==stats.undoEntries && after.redoEntries==stats.redoEntries &&
                   after.retainedBytes==stats.retainedBytes,
                   "failed flatten transaction restores all data and the redo branch");
        } else {
            ++accepted;
            expect(result.changed && document.layers.size()==1 && history.undo().changed &&
                   document.layers.size()==2 && findAsset(document,"pixels") && findAsset(document,"second"),
                   "successful flatten publishes once and restores all source assets");
        }
    }
    expect(rejected && accepted,"flatten allocation test exercises both failure and success");
}
} // namespace

int main() { insertion(); replacement(); proceduralReplacement(); appearanceReplacement(); patternRename(); flattenTransaction(); return failures ? 1 : 0; }
