#pragma once
#include <Core/PaintRect.h>
#include <History/RasterEditHistory.h>
#include <cstdint>
#include <span>

namespace iiSharedCanvas
{
// Which side of the latest local history entry is currently visible.
enum class BitmapHistoryState { Applied, Undone };
inline constexpr int BitmapPreviewTileSide = 64;
// Borrowed row-major final ARGB pixels. Finite coordinates are asset-local;
// chunked coordinates are world pixels within the allocated canvas region.
struct BitmapPatch
{
    DevicePixelRect bounds{};
    std::span<const std::uint32_t> pixels;
};
// Borrowed row-major final pixels in a staged preview. Bounds are
// BitmapPreviewTileSide square and aligned to that grid, asset-local for finite bitmaps and chunk-grid
// coordinates before any layer transform.
struct BitmapPreviewTile
{
    DevicePixelRect bounds{};
    std::span<const std::uint32_t> pixels;
};
} // namespace iiSharedCanvas
