#include <iiSharedCanvasNative.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MANY_CHUNKS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(MANY_CHUNKS_ASAN)
#define MANY_CHUNKS_ASAN 1
#endif
#ifndef MANY_CHUNKS_ASAN
#define MANY_CHUNKS_ASAN 0
#endif
namespace allocation {
thread_local bool observe = false;
thread_local std::size_t live = 0, peak = 0;
struct alignas(std::max_align_t) Header { std::size_t size; bool tracked; };
}
#if !MANY_CHUNKS_ASAN
void *operator new(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() - sizeof(allocation::Header)) throw std::bad_alloc();
    auto *header = static_cast<allocation::Header *>(std::malloc(sizeof(allocation::Header) + (n ? n : 1)));
    if (!header) throw std::bad_alloc();
    header->size = n; header->tracked = allocation::observe;
    if (header->tracked) {
        allocation::live += n;
        allocation::peak = std::max(allocation::peak, allocation::live);
    }
    return header + 1;
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept {
    if (!p) return;
    auto *header = static_cast<allocation::Header *>(p) - 1;
    if (header->tracked) allocation::live -= header->size;
    std::free(header);
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }
#endif

using namespace iiSharedCanvas;
int main() {
    Document document; document.extent = {256, 256}; document.canvasMode = CanvasMode::Infinite;
    document.infiniteCanvas = {{0, 0}, 32};
    ChunkedRasterAsset asset{"paint", {}};
    for (int row = 0; row < 8; ++row) for (int column = 0; column < 8; ++column)
        asset.chunks.push_back({column, row, makeRasterLayer(32, 32, 0xff224466U)});
    document.assets.emplace_back(std::move(asset));
    document.layers.emplace_back(StaticBitmapLayer{{"layer", "Paint"}, StaticSource{"paint"}});
    auto referenceDocument = document;
    std::vector<std::uint32_t> finalPixels(64 * 64, 0xff224466U);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
        if (x < 16 && y < 16) finalPixels[std::size_t(y) * 64 + x] = 0;
        if (x >= 32 && y >= 32) finalPixels[std::size_t(y) * 64 + x] = 0xff00ee44U;
        auto &chunk = findChunkedRasterAsset(referenceDocument, "paint")->chunks[std::size_t(y / 32) * 8 + x / 32];
        chunk.pixels.pixels[std::size_t(y % 32) * 32 + x % 32] = finalPixels[std::size_t(y) * 64 + x];
    }
    const auto reference = renderFrameRegion(referenceDocument, 0, {{0, 0}, {256, 256}}, {256, 256});
    const std::array tiles{BitmapPreviewTile{{{0, 0}, 64, 64}, finalPixels}};
    const std::array overlays{FrameBitmapOverlay{"paint", tiles}};
    allocation::live = allocation::peak = 0; allocation::observe = true;
    const auto result = renderFrameRegion(document, 0, {{0, 0}, {256, 256}}, {256, 256},
                                          RasterSampling::Nearest, overlays);
    allocation::observe = false;
    bool okay = reference.ok() && result.ok() && reference.pixels.pixels == result.pixels.pixels;
    if (!okay) std::cerr << "many chunks: overlay output differs from independently edited document\n";
#if !MANY_CHUNKS_ASAN
    if (allocation::peak >= 4 * 1024 * 1024) {
        std::cerr << "many chunks: live C++ allocations retain too many full output images\n";
        okay = false;
    }
#endif
    std::cout << "OVERLAY_MANY_CHUNKS count=64 output=256x256 peak_live_cpp=" << allocation::peak << '\n';
    return okay ? 0 : 1;
}
