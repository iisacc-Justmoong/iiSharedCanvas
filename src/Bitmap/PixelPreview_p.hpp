#pragma once

#include "Bitmap/BitmapEdit.h"
#include <Layer/RasterLayer.h>
#include <Render/DirtyRegion.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace iiSharedCanvas::detail
{
// Original and final pixels only. Replacing a complete preview restores every
// omitted old region; neither input samples nor rendering commands are retained.
class BitmapPixelPreview final
{
    static constexpr int Side = BitmapPreviewTileSide;
    struct Tile
    {
        RasterLayer original;
        RasterLayer final;
    };
    std::map<std::pair<int, int>, std::shared_ptr<Tile>> tiles_;
    static int column(int value) noexcept
    {
        return value >= 0 ? value / Side : (value + 1) / Side - 1;
    }

  public:
    [[nodiscard]] bool fits(std::span<const BitmapPatch> patches, std::size_t maximumBytes) const
    {
        constexpr auto TileBytes = sizeof(Tile) + Side * Side * 2 * sizeof(std::uint32_t);
        const auto allowed = maximumBytes / TileBytes;
        if (tiles_.size() > allowed)
            return false;
        std::set<std::pair<int, int>> added;
        for (const auto &patch : patches)
        {
            const int lastColumn = column(patch.bounds.origin.x + patch.bounds.width - 1);
            const int lastRow = column(patch.bounds.origin.y + patch.bounds.height - 1);
            for (int row = column(patch.bounds.origin.y); row <= lastRow; ++row)
                for (int col = column(patch.bounds.origin.x); col <= lastColumn; ++col)
                {
                    const auto key = std::pair{col, row};
                    if (tiles_.contains(key) || added.contains(key))
                        continue;
                    if (added.size() == allowed - tiles_.size())
                        return false;
                    added.insert(key);
                }
        }
        return true;
    }
    template <class ReadPixel> void replace(std::span<const BitmapPatch> patches, ReadPixel read)
    {
        for (auto &[key, tile] : tiles_)
        {
            if (tile.use_count() != 1)
                tile = std::make_shared<Tile>(*tile);
            tile->final = tile->original;
        }
        for (const auto &patch : patches)
            for (int y = 0; y < patch.bounds.height; ++y)
                for (int x = 0; x < patch.bounds.width;)
                {
                    const int worldX = patch.bounds.origin.x + x;
                    const int worldY = patch.bounds.origin.y + y;
                    const auto key = std::pair{column(worldX), column(worldY)};
                    auto &tile = tiles_[key];
                    if (!tile)
                    {
                        tile = std::make_shared<Tile>();
                        tile->original = makeRasterLayer(Side, Side);
                        for (int row = 0; row < Side; ++row)
                            for (int col = 0; col < Side; ++col)
                                tile->original.pixels[std::size_t(row) * Side + col] =
                                    read(DevicePixelPoint{key.first * Side + col,
                                                          key.second * Side + row});
                        tile->final = tile->original;
                    }
                    const int localX = worldX - key.first * Side;
                    const int localY = worldY - key.second * Side;
                    const int count = std::min(patch.bounds.width - x, Side - localX);
                    std::copy_n(patch.pixels.data() + std::size_t(y) * patch.bounds.width + x,
                                count,
                                tile->final.pixels.data() + std::size_t(localY) * Side + localX);
                    x += count;
                }
    }
    template <class ReadPixel>
    [[nodiscard]] std::uint32_t originalPixel(DevicePixelPoint p, ReadPixel read) const
    {
        const auto key = std::pair{column(p.x), column(p.y)};
        auto it = tiles_.find(key);
        return it == tiles_.end()
                   ? read(p)
                   : rasterLayerPixelAt(it->second->original,
                                        {p.x - key.first * Side, p.y - key.second * Side});
    }
    template <class Visitor> void visit(Visitor visitor) const
    {
        for (const auto &[key, tile] : tiles_)
            visitor(DevicePixelRect{{key.first * Side, key.second * Side}, Side, Side},
                    tile->original, tile->final);
    }
    [[nodiscard]] std::vector<BitmapPreviewTile> tiles() const
    {
        std::vector<BitmapPreviewTile> result;
        result.reserve(tiles_.size());
        visit([&](auto bounds, const auto &, const auto &pixels) {
            result.push_back({bounds, pixels.pixels});
        });
        return result;
    }
    // Exact displayed difference between two complete preview states. Missing
    // tiles are interpreted as their counterpart's captured original pixels.
    [[nodiscard]] DevicePixelRect difference(const BitmapPixelPreview &previous,
                                             DevicePixelRect clip) const noexcept
    {
        bool changed = false;
        int left = 0, top = 0, right = 0, bottom = 0;
        auto current = tiles_.begin(), before = previous.tiles_.begin();
        while (current != tiles_.end() || before != previous.tiles_.end())
        {
            const bool onlyCurrent = before == previous.tiles_.end() ||
                (current != tiles_.end() && current->first < before->first);
            const bool onlyBefore = current == tiles_.end() ||
                (before != previous.tiles_.end() && before->first < current->first);
            const auto key = onlyBefore ? before->first : current->first;
            const Tile *now = onlyBefore ? nullptr : current->second.get();
            const Tile *old = onlyCurrent ? nullptr : before->second.get();
            const auto &a = now ? now->final : old->original;
            const auto &b = old ? old->final : now->original;
            const DevicePixelRect tileBounds{{key.first * Side, key.second * Side}, Side, Side};
            const auto inside = intersectDevicePixelRects(tileBounds, clip);
            for (int y = 0; y < inside.height; ++y)
                for (int x = 0; x < inside.width; ++x)
                {
                    const int worldX = inside.origin.x + x, worldY = inside.origin.y + y;
                    const auto offset = std::size_t(worldY - tileBounds.origin.y) * Side +
                                        worldX - tileBounds.origin.x;
                    if (a.pixels[offset] == b.pixels[offset]) continue;
                    if (!changed) { left = worldX; top = worldY; right = worldX + 1;
                                    bottom = worldY + 1; changed = true; }
                    else { left = std::min(left, worldX); top = std::min(top, worldY);
                           right = std::max(right, worldX + 1); bottom = std::max(bottom, worldY + 1); }
                }
            if (!onlyBefore) ++current;
            if (!onlyCurrent) ++before;
        }
        return changed ? DevicePixelRect{{left, top}, right - left, bottom - top} : DevicePixelRect{};
    }
    template <class ReadPixel>
    [[nodiscard]] bool matchesOriginal(DevicePixelRect clip, ReadPixel read) const
    {
        bool result = true;
        visit([&](auto bounds, const auto &original, const auto &) {
            const auto inside = intersectDevicePixelRects(bounds, clip);
            for (int y = 0; y < inside.height && result; ++y)
                for (int x = 0; x < inside.width; ++x)
                    if (read(DevicePixelPoint{inside.origin.x + x, inside.origin.y + y}) !=
                        rasterLayerPixelAt(original, {inside.origin.x + x - bounds.origin.x,
                                                      inside.origin.y + y - bounds.origin.y}))
                    { result = false; break; }
        });
        return result;
    }
    template <class ReadPixel>
    [[nodiscard]] bool matches(DevicePixelRect clip, ReadPixel read) const
    {
        bool result = true;
        visit(
            [&](auto bounds, const auto &, const auto &final)
            {
                const auto inside = intersectDevicePixelRects(bounds, clip);
                for (int y = 0; y < inside.height && result; ++y)
                    for (int x = 0; x < inside.width; ++x)
                        if (read(DevicePixelPoint{inside.origin.x + x, inside.origin.y + y}) !=
                            rasterLayerPixelAt(final, {inside.origin.x + x - bounds.origin.x,
                                                       inside.origin.y + y - bounds.origin.y}))
                        {
                            result = false;
                            break;
                        }
            });
        return result;
    }
    [[nodiscard]] std::size_t storageBytes() const noexcept
    {
        return tiles_.size() * (sizeof(Tile) + Side * Side * 2 * sizeof(std::uint32_t));
    }
};
} // namespace iiSharedCanvas::detail
