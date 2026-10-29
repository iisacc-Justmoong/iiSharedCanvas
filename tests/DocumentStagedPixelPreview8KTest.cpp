#include <Document/DocumentHistory.h>
#include <File/DocumentFile.h>
#include <Render/FrameRenderer.h>
#include <iiFileProviderNative.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <new>
#include <string_view>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STAGED_8K_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(STAGED_8K_ASAN)
#define STAGED_8K_ASAN 1
#endif
#ifndef STAGED_8K_ASAN
#define STAGED_8K_ASAN 0
#endif
namespace allocations {
thread_local bool active = false;
thread_local std::size_t largest = 0, total = 0;
}
#if !STAGED_8K_ASAN
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
void expect(bool value, std::string_view message) { if (!value) { ++failures; std::cerr << message << '\n'; } }
std::uint64_t checksum(const RasterLayer &raster, std::uint64_t &visited) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (auto pixel : raster.pixels) { hash ^= pixel; hash *= 1099511628211ULL; }
    visited += raster.pixels.size(); return hash;
}
FrameRenderResult view(const Document &d) {
    return renderFrameRegion(d, 0, {{4096, 4096}, {32, 16}}, {32, 16});
}
}
int main() {
    Document source; source.extent = {Side, Side};
    std::uint32_t random = 31;
    for (const auto *id : {"paint", "mask"}) {
        auto raster = makeRasterLayer(Side, Side);
        for (auto &p : raster.pixels) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            p = std::string_view(id) == "mask" ? random : (0xff000000U | (random & 0xffffffU));
        }
        source.assets.emplace_back(RasterAsset{id, std::move(raster)});
    }
    source.assets.emplace_back(RasterAsset{"overlay", makeRasterLayer(32, 16, 0xff804020)});
    source.layers.emplace_back(StaticBitmapLayer{{"paint-layer", "Paint"}, StaticSource{"paint"}});
    LayerMask mask; mask.assetId = "mask"; mask.linkedToLayer = false; mask.channel = MaskChannel::Alpha;
    layerProperties(source.layers.back()).appearance.rasterMask = mask;
    LayerProperties overlay{"overlay-layer", "Overlay"}; overlay.opacity = .35;
    overlay.transform.translationX = 4096; overlay.transform.translationY = 4096;
    source.layers.emplace_back(StaticBitmapLayer{std::move(overlay), StaticSource{"overlay"}});
    const auto unfiltered = view(source);
    LayerFilter filter; filter.name = "Invert"; filter.operation = RasterEffect{RasterEffectKind::Invert};
    layerProperties(source.layers.back()).appearance.filters.push_back(filter);
    const auto filtered = view(source);
    expect(unfiltered.ok() && filtered.ok() && unfiltered.pixels.pixels != filtered.pixels.pixels,
           "8K staged: the separate filter changes composition");
    layerProperties(source.layers[0]).appearance.rasterMask->enabled = false;
    const auto unmasked = view(source);
    layerProperties(source.layers[0]).appearance.rasterMask->enabled = true;
    expect(unmasked.ok() && filtered.ok() && unmasked.pixels.pixels != filtered.pixels.pixels,
           "8K staged: the mask changes composition");
    std::uint64_t visited = 0;
    const auto paintHash = checksum(findRasterAsset(source, "paint")->pixels, visited);
    const auto maskHash = checksum(findRasterAsset(source, "mask")->pixels, visited);
    const std::filesystem::path parent(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    std::filesystem::create_directories(parent); iiFileProvider::native::TemporaryDirectory temporary(parent);
    if (!temporary.isValid()) { expect(false, "8K staged: temporary directory"); return 1; }
    const auto path = (temporary.path() / "staged-8k.iisc").string();
    DocumentFile owner; const auto created = owner.create(path, source);
    if (!created.ok()) { std::cerr << "8K staged create: " << created.message << '\n'; return 1; }
    source = {};
    const auto bytes = std::filesystem::file_size(path);
    const auto *paintPixels = findRasterAsset(*owner.document(), "paint")->pixels.pixels.data();
    const auto *maskPixels = findRasterAsset(*owner.document(), "mask")->pixels.pixels.data();
    DocumentHistoryLimits limits; limits.maximumBytes = 1024 * 1024; limits.maximumBitmapInputBytes = 8 * 1024 * 1024;
    DocumentHistory history(owner, limits);
    expect(history.setLayerName("overlay-layer", "Ink").changed && history.undo().changed,
           "8K staged: establish redo");
    const auto revision = owner.revision();
    std::vector<std::uint32_t> replacement(8 * 8, 0xffe02070);
    allocations::active = true; allocations::largest = 0; allocations::total = 0;
    const auto started = std::chrono::steady_clock::now();
    bool staged = history.beginStagedPixelPreview("paint").ok();
    for (int i = 0; i < 14; ++i) {
        const std::array patch{BitmapPatch{{{4096 + i, 4096}, 8, 8}, replacement}};
        staged = history.replacePixelPreview(patch).ok() && staged;
    }
    const auto previewUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    allocations::active = false;
    const auto largest = allocations::largest, total = allocations::total;
    const auto pending = history.previewPendingHistoryBytes(), cache = history.previewStorageBytes();
    expect(staged && owner.revision() == revision && history.statistics().redoEntries == 1 &&
           findRasterAsset(*owner.document(), "paint")->pixels.pixels.data() == paintPixels &&
           findRasterAsset(*owner.document(), "mask")->pixels.pixels.data() == maskPixels &&
           view(*owner.document()).pixels.pixels == filtered.pixels.pixels,
           "8K staged: replacements leave owner, revision, redo and composition unchanged");
#if !STAGED_8K_ASAN
    expect(largest < 32 * 1024 * 1024, "8K staged: no ordinary input allocation is a full image");
#endif
    const auto commitStart = std::chrono::steady_clock::now();
    const auto committed = history.commitPixelPreview();
    const auto commitUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - commitStart).count();
    const auto written = owner.lastWriteStatistics();
    const auto painted = view(*owner.document());
    expect(committed.changed && owner.revision() == revision + 1 && committed.discardedRedoEntries == 1 &&
           written.payloadBytesWritten < 65536 && painted.ok() && painted.pixels.pixels != filtered.pixels.pixels,
           "8K staged: one bounded commit updates actual filtered and masked composition");
    expect(history.undo().changed && view(*owner.document()).pixels.pixels == filtered.pixels.pixels &&
           checksum(findRasterAsset(*owner.document(), "paint")->pixels, visited) == paintHash &&
           checksum(findRasterAsset(*owner.document(), "mask")->pixels, visited) == maskHash,
           "8K staged: undo restores original composition and dense checksums");
    expect(history.redo().changed && view(*owner.document()).pixels.pixels == painted.pixels.pixels,
           "8K staged: redo restores committed composition");
    const auto finalPaintHash = checksum(findRasterAsset(*owner.document(), "paint")->pixels, visited);
    Document reopened; const auto read = DocumentFile::readSnapshot(path, reopened);
    expect(read.ok() && view(reopened).pixels.pixels == painted.pixels.pixels &&
           checksum(findRasterAsset(reopened, "paint")->pixels, visited) == finalPaintHash &&
           checksum(findRasterAsset(reopened, "mask")->pixels, visited) == maskHash,
           "8K staged: reopened file preserves composition and dense checksums");
    std::cout << "STAGED_PREVIEW_8K file_bytes=" << bytes << " checked_pixels=" << visited
              << " pending=" << pending << " cache=" << cache << " largest_cpp=" << largest
              << " total_cpp=" << total << " preview_us=" << previewUs << " commit_us=" << commitUs
              << " filter_and_mask=rendered\n";
    return failures ? 1 : 0;
}
