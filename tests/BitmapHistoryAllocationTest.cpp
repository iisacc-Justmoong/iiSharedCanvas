#include <Bitmap/BitmapEditor.h>
#include <Bitmap/ChunkedBitmapEditor.h>
#include <Serialization/IiscCodec.h>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>

namespace fault
{
thread_local std::ptrdiff_t remaining = -1;
thread_local bool fired = false;
thread_local bool observe = false;
thread_local std::size_t allocatedBytes = 0;
} // namespace fault
// Test process only: fail one C++ allocation, then immediately restore normal
// allocation so errors and verification can be reported without another fault.
void *operator new(std::size_t size)
{
    if (fault::observe)
        fault::allocatedBytes += size;
    if (fault::remaining == 0)
    {
        fault::remaining = -1;
        fault::fired = true;
        throw std::bad_alloc();
    }
    if (fault::remaining > 0)
        --fault::remaining;
    if (auto *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace
{
using namespace iiSharedCanvas;
int failures = 0;
void check(bool value, const char *message)
{
    if (!value)
    {
        ++failures;
        std::cerr << message << '\n';
    }
}
Document document(bool chunked)
{
    Document d;
    d.extent = {128, 128};
    if (chunked)
    {
        d.canvasMode = CanvasMode::Infinite;
        d.infiniteCanvas.chunkSize = 128;
        d.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    }
    else
        d.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(128, 128)});
    d.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    return d;
}
template <class Editor> void run(bool chunked)
{
    std::size_t rejected = 0, successful = 0;
    for (std::ptrdiff_t allocation = 0; allocation < 128; ++allocation)
    {
        auto d = document(chunked);
        Editor e(d, "paint");
        const auto stable = encodeIisc(d).bytes;
        const std::array<std::uint32_t, 1> a{0xff112233}, b{0x80445566};
        const std::array patches{BitmapPatch{{{1, 1}, 1, 1}, a}, BitmapPatch{{{65, 65}, 1, 1}, b}};
        fault::fired = false;
        fault::remaining = allocation;
        bool applied = false;
        try
        {
            applied = e.replacePatches(patches);
        }
        catch (const std::bad_alloc &)
        {
        }
        fault::remaining = -1;
        if (!applied)
        {
            ++rejected;
            check(fault::fired, "allocation: rejection corresponds to the injected failure");
            check(encodeIisc(d).bytes == stable && e.revision() == 0 &&
                      e.historyStatistics().undoEntries == 0,
                  "allocation: all source metadata, pixels, chunk allocation and cursor remain "
                  "unchanged");
        }
        else
        {
            ++successful;
            check(applied && e.revision() == 1 && e.historyStatistics().undoEntries == 1,
                  "allocation: fully prepared batch commits once");
            check(e.pixelAt(1, 1) == a[0] && e.pixelAt(65, 65) == b[0],
                  "allocation: successful batch publishes every region");
        }
    }
    check(rejected >= 20 && successful > 0,
          "allocation: injection spans planning, journal and writer preparation");
    std::cout << (chunked ? "chunked" : "finite") << " failed_allocation_positions=" << rejected
              << " successful_positions=" << successful << '\n';
}
template <class Editor> void runPreview(bool chunked)
{
    std::size_t rejected = 0, successful = 0;
    for (std::ptrdiff_t allocation = 0; allocation < 128; ++allocation)
    {
        auto d = document(chunked);
        Editor e(d, "paint");
        const std::array<std::uint32_t, 1> a{0x80445566}, b{0xff778899};
        const std::array first{BitmapPatch{{{1, 1}, 1, 1}, a}},
            second{BitmapPatch{{{65, 65}, 1, 1}, b}};
        check(e.beginPixelPreview("Allocation preview") && e.replacePixelPreview(first),
              "preview allocation: establish visible prior result");
        const auto stable = encodeIisc(d).bytes;
        const auto revision = e.revision();
        const auto retained = e.historyStatistics().retainedBytes;
        const auto cached = e.previewStorageBytes();
        fault::remaining = allocation;
        fault::fired = false;
        bool applied = false;
        try
        {
            applied = e.replacePixelPreview(second);
        }
        catch (const std::bad_alloc &)
        {
        }
        fault::remaining = -1;
        if (!applied)
        {
            ++rejected;
            check(fault::fired && encodeIisc(d).bytes == stable && e.revision() == revision &&
                      e.previewStorageBytes() == cached &&
                      e.historyStatistics().retainedBytes == retained && e.pixelPreviewActive(),
                  "preview allocation: old visible pixels, metadata, cache and cursor survive");
        }
        else
        {
            ++successful;
            check(e.pixelAt(1, 1) == 0 && e.pixelAt(65, 65) == b[0] &&
                      e.historyStatistics().undoEntries == 1,
                  "preview allocation: complete replacement commits together");
        }
        check(e.cancelPixelPreview(), "preview allocation: session remains resolvable");
    }
    check(rejected >= 30 && successful > 0,
          "preview allocation: planning, tile clones and atomic writer failures exercised");
    std::cout << (chunked ? "chunked" : "finite")
              << " preview_failed_allocation_positions=" << rejected
              << " successful_positions=" << successful << '\n';
}
template <class Editor> void runPreviewCacheBudget(bool chunked)
{
    auto d = document(chunked);
    RasterHistoryLimits limits;
    limits.maximumInputBytes = 1024;
    Editor e(d, "paint", limits);
    check(e.beginPixelPreview("Cache budget"), "preview budget allocation: begin");
    const std::array<std::uint32_t, 1> value{0xff123456};
    const std::array patch{BitmapPatch{{{1, 1}, 1, 1}, value}};
    fault::allocatedBytes = 0;
    fault::observe = true;
    const auto applied = e.replacePixelPreview(patch);
    fault::observe = false;
    check(!applied && e.lastHistoryResult().code == RasterHistoryCode::BudgetExceeded &&
              e.previewStorageBytes() == 0 && e.pixelAt(1, 1) == 0,
          "preview budget allocation: oversized tile cache rejected without source mutation");
    check(fault::allocatedBytes < 4096,
          "preview budget allocation: reject before allocating original and final pixel tiles");
    std::cout << (chunked ? "chunked" : "finite")
              << " preview_budget_allocation_bytes=" << fault::allocatedBytes << '\n';
    check(e.cancelPixelPreview(), "preview budget allocation: empty session cancel");
}
} // namespace
int main()
{
    run<iiSharedCanvas::BitmapEditor>(false);
    run<iiSharedCanvas::ChunkedBitmapEditor>(true);
    runPreview<iiSharedCanvas::BitmapEditor>(false);
    runPreview<iiSharedCanvas::ChunkedBitmapEditor>(true);
    runPreviewCacheBudget<iiSharedCanvas::BitmapEditor>(false);
    runPreviewCacheBudget<iiSharedCanvas::ChunkedBitmapEditor>(true);
    return failures ? 1 : 0;
}
