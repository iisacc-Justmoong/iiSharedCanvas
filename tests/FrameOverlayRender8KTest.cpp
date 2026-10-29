#include <iiSharedCanvasNative.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string_view>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OVERLAY_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(OVERLAY_ASAN)
#define OVERLAY_ASAN 1
#endif
#ifndef OVERLAY_ASAN
#define OVERLAY_ASAN 0
#endif
namespace allocation { thread_local bool observe = false; thread_local std::size_t largest = 0, total = 0; }
#if !OVERLAY_ASAN
void *operator new(std::size_t n) {
    if (allocation::observe) { allocation::largest = std::max(allocation::largest, n); allocation::total += n; }
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
int main() {
    int failures = 0;
    const auto expect = [&](bool value, std::string_view message) {
        if (!value) { ++failures; std::cerr << message << '\n'; }
    };
    constexpr int side = 8192;
    constexpr std::uint32_t originalPaint = 0xffbc5422U, originalMask = 0xff888888U;
    Document document; document.extent = {side, side};
    document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(side, side, originalPaint)});
    document.assets.emplace_back(RasterAsset{"mask", makeRasterLayer(side, side, originalMask)});
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    LayerMask mask; mask.assetId = "mask"; mask.featherRadius = 1.25;
    layerProperties(document.layers[0]).appearance.rasterMask = mask;
    const CanvasRegion region{{4092, 4092}, {32, 32}};
    const auto before = renderFrameRegion(document, 0, region, {32, 32}, RasterSampling::Smooth);
    const auto revision = document.authorship.revision();
    DocumentHistory history(document);
    std::array<std::uint32_t, 64> patchPixels{};
    for (std::size_t i = 0; i < patchPixels.size(); ++i) patchPixels[i] = (i % 2) ? 0xff22ee66U : 0;
    const std::array patches{BitmapPatch{{{4096, 4096}, 8, 8}, patchPixels}};
    expect(history.beginStagedPixelPreview("paint").ok() && history.replacePixelPreview(patches).changed,
           "8K: stage paint pixels");
    auto paintTiles = history.stagedPixelPreviewTiles();
    std::vector<std::uint32_t> maskPixels(64 * 64, originalMask);
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        maskPixels[std::size_t(y) * 64 + x] = 0xffeeeeeeU;
    const std::array maskTiles{BitmapPreviewTile{{{4096, 4096}, 64, 64}, maskPixels}};
    const std::array overlays{FrameBitmapOverlay{"paint", paintTiles}, FrameBitmapOverlay{"mask", maskTiles}};
    allocation::largest = allocation::total = 0; allocation::observe = true;
    const auto preview = renderFrameRegion(document, 0, region, {32, 32}, RasterSampling::Smooth, overlays);
    allocation::observe = false;
    const auto finiteLargest = allocation::largest, finiteTotal = allocation::total;
    expect(preview.ok() && before.ok() && preview.pixels.pixels != before.pixels.pixels,
           "8K: staged paint and mask change the visible region");
    const auto *paint = findRasterAsset(document, "paint"), *original = findRasterAsset(document, "mask");
    const bool unchangedPaint = std::all_of(paint->pixels.pixels.begin(), paint->pixels.pixels.end(),
                                             [](auto pixel) { return pixel == originalPaint; });
    const bool unchangedMask = std::all_of(original->pixels.pixels.begin(), original->pixels.pixels.end(),
                                            [](auto pixel) { return pixel == originalMask; });
    expect(unchangedPaint && unchangedMask && document.authorship.revision() == revision,
           "8K: all 134217728 original pixels and document revision remain unchanged");
#if !OVERLAY_ASAN
    expect(allocation::largest < 4 * 1024 * 1024 && allocation::total < 16 * 1024 * 1024,
           "8K: small-region overlay render must not clone a full asset");
#endif
    expect(history.commitPixelPreview().changed, "8K: commit staged paint");
    auto &referenceMask = findRasterAsset(document, "mask")->pixels;
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        referenceMask.pixels[std::size_t(4096 + y) * side + 4096 + x] = 0xffeeeeeeU;
    const auto reference = renderFrameRegion(document, 0, region, {32, 32}, RasterSampling::Smooth);
    expect(reference.ok() && preview.ok() && reference.pixels.pixels == preview.pixels.pixels,
           "8K: preview output equals committed paint and independently edited mask");
    Document sparse; sparse.extent = {64, 64}; sparse.canvasMode = CanvasMode::Infinite;
    sparse.infiniteCanvas = {{0, 0}, 4096};
    sparse.assets.emplace_back(ChunkedRasterAsset{"sparse", {}});
    sparse.layers.emplace_back(StaticBitmapLayer{{"sparse-layer", "Sparse"}, StaticSource{"sparse"}});
    std::vector<std::uint32_t> sparsePixels(64 * 64, 0);
    sparsePixels[65] = 0xff22ee66U;
    const std::array sparseTiles{BitmapPreviewTile{{{4096, 4096}, 64, 64}, sparsePixels}};
    const std::array sparseOverlay{FrameBitmapOverlay{"sparse", sparseTiles}};
    allocation::largest = allocation::total = 0; allocation::observe = true;
    const auto sparsePreview = renderFrameRegion(sparse, 0, {{4096, 4096}, {32, 32}}, {32, 32},
                                                  RasterSampling::Nearest, sparseOverlay);
    allocation::observe = false;
    expect(sparsePreview.ok() && sparsePreview.pixels.pixels[33] == 0xff22ee66U &&
           findChunkedRasterAsset(sparse, "sparse")->chunks.empty(),
           "sparse: an unallocated 4096x4096 chunk is visible without mutating the asset");
#if !OVERLAY_ASAN
    expect(allocation::largest < 4 * 1024 * 1024 && allocation::total < 16 * 1024 * 1024,
           "sparse: rendering a missing large chunk must not allocate its full pixel array");
#endif
    std::cout << "OVERLAY_RENDER_8K pixels_checked=" << 2ULL * side * side
              << " largest_cpp=" << finiteLargest << " total_cpp=" << finiteTotal
              << " sparse_largest_cpp=" << allocation::largest << " sparse_total_cpp=" << allocation::total
              << " region=32x32 filter=none mask=feathered\n";
    return failures ? 1 : 0;
}
