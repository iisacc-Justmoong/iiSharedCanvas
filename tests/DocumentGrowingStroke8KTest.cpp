#include <Bitmap/BitmapEditor.h>
#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Render/FrameRenderer.h>
#include <iiFileProviderNative.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <string_view>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GROWING_8K_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(GROWING_8K_ASAN)
#define GROWING_8K_ASAN 1
#endif
#ifndef GROWING_8K_ASAN
#define GROWING_8K_ASAN 0
#endif
namespace allocations {
thread_local bool active = false;
thread_local std::size_t largest = 0, total = 0;
}
#if !GROWING_8K_ASAN
void *operator new(std::size_t n) {
    if (allocations::active) { allocations::largest = std::max(allocations::largest, n); allocations::total += n; }
    if (auto *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
#endif

using namespace iiSharedCanvas;
namespace {
constexpr int Side = 8192;
int failures = 0;
void expect(bool value, const char *message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
std::uint64_t checksum(const RasterLayer &raster, std::uint64_t &visited) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto pixel : raster.pixels) { value ^= pixel; value *= 1099511628211ULL; ++visited; }
    return value;
}
StrokePoint point(double x, double y, double time) {
    StrokePoint sample; sample.position = {x, y}; sample.time = time; sample.pressure = 1; return sample;
}
FrameRenderResult view(const Document &document) {
    return renderFrameRegion(document, 0, {{0, 4096}, {32, 16}}, {32, 16});
}
}
int main() {
    Document source; source.extent = {Side, Side}; source.canvasMode = CanvasMode::Infinite;
    source.infiniteCanvas = {{0, 0}, 256};
    std::uint32_t random = 23;
    for (const auto *id : {"background", "mask"}) {
        auto raster = makeRasterLayer(Side, Side);
        for (auto &pixel : raster.pixels) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            pixel = std::string_view(id) == "mask" ? random : (0xff000000U | (random & 0x00ffffffU));
        }
        source.assets.emplace_back(RasterAsset{id, std::move(raster)});
    }
    source.assets.emplace_back(ChunkedRasterAsset{"paint", {{0, 16, makeRasterLayer(256, 256, 0xff123456)}}});
    source.assets.emplace_back(RasterAsset{"overlay", makeRasterLayer(32, 16, 0xff804020)});
    source.layers.emplace_back(StaticBitmapLayer{{"background-layer", "Background"}, StaticSource{"background"}});
    source.layers.emplace_back(StaticBitmapLayer{{"paint-layer", "Paint"}, StaticSource{"paint"}});
    LayerMask mask; mask.assetId = "mask"; mask.linkedToLayer = false; mask.channel = MaskChannel::Alpha;
    mask.outsideCoverage = 0;
    layerProperties(source.layers.back()).appearance.rasterMask = mask;
    LayerProperties overlay{"overlay-layer", "Overlay"};
    overlay.opacity = .35; overlay.transform.translationY = 4096;
    source.layers.emplace_back(StaticBitmapLayer{std::move(overlay), StaticSource{"overlay"}});
    const auto unfiltered = view(source);
    LayerFilter filter; filter.name = "Invert"; filter.operation = RasterEffect{RasterEffectKind::Invert};
    layerProperties(source.layers.back()).appearance.filters.push_back(filter);
    const auto filtered = view(source);
    expect(unfiltered.ok() && filtered.ok() && unfiltered.pixels.pixels != filtered.pixels.pixels,
           "8K: the overlay filter actually changes the rendered pixels");
    layerProperties(source.layers[1]).appearance.rasterMask->enabled = false;
    const auto unmasked = view(source);
    layerProperties(source.layers[1]).appearance.rasterMask->enabled = true;
    expect(unmasked.ok() && filtered.ok() && unmasked.pixels.pixels != filtered.pixels.pixels,
           "8K: the high-entropy mask actually changes the rendered pixels");
    std::uint64_t visited = 0;
    const auto backgroundHash = checksum(findRasterAsset(source, "background")->pixels, visited);
    const auto maskHash = checksum(findRasterAsset(source, "mask")->pixels, visited);
    const auto originalRegion = canvasRegion(source);
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false, "8K: temporary directory"); return 1; }
    const auto path = (temporary.path() / "growing-8k.iisc").string();
    DocumentFile owner; const auto created = owner.create(path, source);
    if (!created.ok()) { std::cerr << "8K create: " << created.message << '\n'; return 1; }
    source = {};
    const auto fileBytes = std::filesystem::file_size(path);
    expect(fileBytes > 400ULL * 1024 * 1024, "8K: two dense assets occupy the working file");
    const auto *backgroundPixels = findRasterAsset(*owner.document(), "background")->pixels.pixels.data();
    const auto *maskPixels = findRasterAsset(*owner.document(), "mask")->pixels.pixels.data();
    DocumentHistoryLimits limits; limits.maximumBytes = 1024 * 1024; limits.maximumBitmapInputBytes = 8 * 1024 * 1024;
    DocumentHistory history(owner, limits);
    expect(history.setLayerName("overlay-layer", "Ink").changed && history.undo().changed,
           "8K: establish redo branch");
    BitmapBrush brush; brush.size = 7; brush.argb = 0xffe02070;
    allocations::largest = 0; allocations::total = 0; allocations::active = true;
    const auto started = std::chrono::steady_clock::now();
    const auto first = history.beginStroke("paint", brush, point(-8, 4100, 1), StrokeCanvasMode::Grow);
    bool samplesOk = first.changed;
    for (int i = 1; i <= 12; ++i)
        samplesOk = history.continueStroke(point(-8 + i * 2, 4100, 1 + i * .01)).ok() && samplesOk;
    const auto last = history.endStroke(point(18, 4100, 1.14));
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    allocations::active = false;
    const auto largest = allocations::largest, total = allocations::total;
    const auto retained = history.statistics().retainedBytes;
    const auto expanded = canvasRegion(*owner.document());
    expect(samplesOk && last.changed && last.discardedRedoEntries == 1 &&
           expanded.origin.x < originalRegion.origin.x && history.statistics().undoEntries == 1,
           "8K: one continuous stroke grows the canvas and publishes one entry");
    expect(findRasterAsset(*owner.document(), "background")->pixels.pixels.data() == backgroundPixels &&
           findRasterAsset(*owner.document(), "mask")->pixels.pixels.data() == maskPixels,
           "8K: neither dense image buffer is replaced by growth or paint");
#if !GROWING_8K_ASAN
    expect(largest < 32 * 1024 * 1024, "8K: no stroke allocation is a full dense image");
#endif
    const auto painted = view(*owner.document());
    expect(painted.ok() && filtered.ok() && painted.pixels.pixels != filtered.pixels.pixels,
           "8K: growing paint changes the real filtered and masked composition");
    expect(history.undo().changed && canvasRegion(*owner.document()) == originalRegion,
           "8K: one undo restores the original region");
    const auto undone = view(*owner.document());
    expect(undone.ok() && filtered.ok() && undone.pixels.pixels == filtered.pixels.pixels &&
           checksum(findRasterAsset(*owner.document(), "background")->pixels, visited) == backgroundHash &&
           checksum(findRasterAsset(*owner.document(), "mask")->pixels, visited) == maskHash,
           "8K: undo restores the rendered view and dense asset checksums");
    expect(history.redo().changed && canvasRegion(*owner.document()) == expanded,
           "8K: redo restores the expanded region");
    const auto redone = view(*owner.document());
    expect(redone.ok() && painted.ok() && redone.pixels.pixels == painted.pixels.pixels,
           "8K: redo restores the rendered filtered and masked view");
    Document reopened; const auto read = DocumentFile::readSnapshot(path, reopened);
    if (!read.ok()) std::cerr << "8K reopen: " << read.message << '\n';
    expect(read.ok() && canvasRegion(reopened) == expanded, "8K: reopen the grown file");
    if (read.ok()) {
        const auto reopenedView = view(reopened);
        expect(reopenedView.ok() && painted.ok() && reopenedView.pixels.pixels == painted.pixels.pixels &&
               checksum(findRasterAsset(reopened, "background")->pixels, visited) == backgroundHash &&
               checksum(findRasterAsset(reopened, "mask")->pixels, visited) == maskHash,
               "8K: reopened composition and dense asset checksums match");
    }
    std::cout << "GROWING_STROKE_8K file_bytes=" << fileBytes << " checked_pixels=" << visited
              << " retained=" << retained << " largest_cpp=" << largest << " total_cpp=" << total
              << " elapsed_us=" << elapsed << " filter_and_mask=rendered\n";
    return failures ? 1 : 0;
}
