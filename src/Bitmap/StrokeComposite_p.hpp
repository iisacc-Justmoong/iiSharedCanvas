#pragma once

#include <Layer/RasterLayer.h>
#include <Render/Compositor.h>
#include <map>
#include <memory>
#include <utility>

namespace iiSharedCanvas::detail
{
// Runtime pixel tiles only. Neither samples nor input paths survive paint().
class BitmapStrokeComposite final
{
    static constexpr int Side = 64;
    struct Tile
    {
        // Original pixels never change during a stroke. Sharing them between
        // prepared versions keeps failed preparations isolated without copying
        // another complete 64x64 raster for every input sample.
        std::shared_ptr<const RasterLayer> original;
        StrokeCompositeBuffer pending;
        RasterLayer preview;
        bool changed = false;
    };
    std::map<std::pair<int, int>, std::shared_ptr<Tile>> tiles_;
    std::size_t changedTiles_ = 0;
    static int column(int value)
    {
        return value >= 0 ? value / Side : (value + 1) / Side - 1;
    }
    static void updateTile(Tile &tile, const std::vector<RasterSample> &local, RasterBlendMode mode)
    {
        accumulateStrokeSamples(tile.pending, local);
        if (mode == RasterBlendMode::SourceOver || mode == RasterBlendMode::DestinationOut)
            copyStrokePreviewSamplePixels(tile.preview, *tile.original, tile.pending, local,
                                          mode == RasterBlendMode::DestinationOut);
        else
            for (const auto &sample : local)
            {
                const auto index = std::size_t(sample.position.y) * Side + sample.position.x;
                const auto source = strokeCompositePixelAt(tile.pending, sample.position);
                tile.preview.pixels[index] = source >> 24
                    ? compositeRasterPixel(source, tile.original->pixels[index], mode)
                    : tile.original->pixels[index];
            }
        tile.changed = tile.original->pixels != tile.preview.pixels;
    }

  public:
    // Prepare only the tiles touched by this input. Nodes and pixel buffers are
    // allocated before publication, so accepting a batch needs no allocation.
    class PreparedPaint final
    {
        friend class BitmapStrokeComposite;
        std::map<std::pair<int, int>, std::shared_ptr<Tile>> tiles_;
        std::size_t added_ = 0;
      public:
        template <class Visitor> void visit(Visitor visitor) const
        {
            for (const auto &[key, tile] : tiles_)
                visitor(DevicePixelRect{{key.first * Side, key.second * Side}, Side, Side},
                        *tile->original, tile->preview);
        }
        template <class ReadPixel>
        [[nodiscard]] std::uint32_t originalPixel(DevicePixelPoint point, ReadPixel read) const
        {
            const auto key = std::pair{column(point.x), column(point.y)};
            const auto found = tiles_.find(key);
            return found == tiles_.end() ? read(point) :
                rasterLayerPixelAt(*found->second->original,
                                   {point.x - key.first * Side, point.y - key.second * Side});
        }
        [[nodiscard]] bool changed() const noexcept
        {
            for (const auto &[key, tile] : tiles_)
                if (tile->changed) return true;
            return false;
        }
    };
    // The saved preparation is a temporary overlay on this persistent
    // composite. Restore only its footprint before replacing the overlay.
    template <class Visitor> void visitUnderlying(const PreparedPaint &prepared, Visitor visitor) const
    {
        for (const auto &[key, tile] : prepared.tiles_)
        {
            const auto found = tiles_.find(key);
            visitor(DevicePixelRect{{key.first * Side, key.second * Side}, Side, Side},
                    found == tiles_.end() ? *tile->original : found->second->preview);
        }
    }
    template <class ReadPixel>
    PreparedPaint preparePaint(const std::vector<RasterSample> &samples, RasterBlendMode mode,
                               ReadPixel read) const
    {
        struct Group final {
            std::size_t count = 0;
            std::vector<RasterSample> samples;
        };
        std::map<std::pair<int, int>, Group> groups;
        // Reserve each tile's complete batch once. Repeated vector growth can
        // otherwise allocate and copy nearly twice the projected sample count
        // on every coalesced pointer update.
        std::pair<int, int> last{};
        Group *current = nullptr;
        for (const auto &sample : samples) {
            const auto key = std::pair{column(sample.position.x), column(sample.position.y)};
            if (!current || key != last) {
                current = &groups[key];
                last = key;
            }
            ++current->count;
        }
        for (auto &entry : groups)
            entry.second.samples.reserve(entry.second.count);
        current = nullptr;
        for (auto sample : samples)
        {
            const int x = column(sample.position.x), y = column(sample.position.y);
            const auto key = std::pair{x, y};
            if (!current || key != last) {
                current = &groups.find(key)->second;
                last = key;
            }
            sample.position.x -= x * Side;
            sample.position.y -= y * Side;
            sample.blendMode = RasterBlendMode::SourceOver;
            current->samples.push_back(sample);
        }
        PreparedPaint prepared;
        for (const auto &[key, group] : groups)
        {
            const auto prior = tiles_.find(key);
            auto tile = prior == tiles_.end() ? std::make_shared<Tile>()
                                             : std::make_shared<Tile>(*prior->second);
            if (prior == tiles_.end())
            {
                ++prepared.added_;
                auto original = std::make_shared<RasterLayer>(makeRasterLayer(Side, Side));
                tile->pending = makeStrokeCompositeBuffer(Side, Side);
                for (int y = 0; y < Side; ++y)
                    for (int x = 0; x < Side; ++x)
                        original->pixels[std::size_t(y) * Side + x] =
                            read({key.first * Side + x, key.second * Side + y});
                tile->preview = *original;
                tile->original = std::move(original);
            }
            updateTile(*tile, group.samples, mode);
            prepared.tiles_.emplace(key, std::move(tile));
        }
        return prepared;
    }
    template <class Visitor> void visitPrior(const PreparedPaint &prepared, Visitor visitor) const
    {
        for (const auto &[key, unused] : prepared.tiles_)
        {
            const auto found = tiles_.find(key);
            if (found != tiles_.end())
                visitor(DevicePixelRect{{key.first * Side, key.second * Side}, Side, Side},
                        found->second->preview);
        }
    }
    [[nodiscard]] std::size_t storageBytesAfter(const PreparedPaint &prepared) const noexcept
    {
        constexpr auto tileBytes = sizeof(Tile) + sizeof(RasterLayer) + 4 * sizeof(void *) +
            Side * Side * (2 * sizeof(std::uint32_t) + sizeof(PremultipliedPixel));
        return (tiles_.size() + prepared.added_) * tileBytes;
    }
    void commitPaint(PreparedPaint &&prepared) noexcept
    {
        while (!prepared.tiles_.empty())
        {
            auto node = prepared.tiles_.extract(prepared.tiles_.begin());
            const auto prior = tiles_.find(node.key());
            if (prior != tiles_.end())
            {
                changedTiles_ -= prior->second->changed;
                changedTiles_ += node.mapped()->changed;
                prior->second = std::move(node.mapped());
            }
            else
            {
                changedTiles_ += node.mapped()->changed;
                tiles_.insert(std::move(node));
            }
        }
    }
    [[nodiscard]] bool changed() const noexcept { return changedTiles_ != 0; }
    template <class ReadPixel, class WritePixel>
    void paint(const std::vector<RasterSample> &samples, RasterBlendMode mode, ReadPixel read,
               WritePixel write)
    {
        std::map<std::pair<int, int>, std::vector<RasterSample>> groups;
        for (auto sample : samples)
        {
            const int x = column(sample.position.x), y = column(sample.position.y);
            sample.position.x -= x * Side;
            sample.position.y -= y * Side;
            // Pending coverage is always source-over; final composition chooses
            // the stroke's source-over, destination-out or layer blend mode.
            sample.blendMode = RasterBlendMode::SourceOver;
            groups[{x, y}].push_back(sample);
        }
        for (const auto &[coordinate, local] : groups)
        {
            auto &tile = tiles_[coordinate];
            const int left = coordinate.first * Side, top = coordinate.second * Side;
            if (!tile)
            {
                tile = std::make_shared<Tile>();
                auto original = std::make_shared<RasterLayer>(makeRasterLayer(Side, Side));
                tile->pending = makeStrokeCompositeBuffer(Side, Side);
                for (int y = 0; y < Side; ++y)
                    for (int x = 0; x < Side; ++x)
                        original->pixels[std::size_t(y) * Side + x] =
                            read({left + x, top + y});
                tile->preview = *original;
                tile->original = std::move(original);
            }
            else if (tile.use_count() != 1)
                tile = std::make_shared<Tile>(*tile);
            const bool before = tile->changed;
            updateTile(*tile, local, mode);
            changedTiles_ -= before;
            changedTiles_ += tile->changed;
            for (const auto &sample : local)
            {
                const auto index = std::size_t(sample.position.y) * Side + sample.position.x;
                write(DevicePixelPoint{left + sample.position.x, top + sample.position.y},
                      tile->preview.pixels[index]);
            }
        }
    }
    template <class ReadPixel>
    std::uint32_t originalPixel(DevicePixelPoint point, ReadPixel read) const
    {
        const auto key = std::pair{column(point.x), column(point.y)};
        const auto it = tiles_.find(key);
        return it == tiles_.end()
                   ? read(point)
                   : rasterLayerPixelAt(*it->second->original,
                                        {point.x - key.first * Side, point.y - key.second * Side});
    }
    template <class ReadPixel>
    std::uint32_t previewPixel(DevicePixelPoint point, ReadPixel read) const
    {
        const auto key = std::pair{column(point.x), column(point.y)};
        const auto it = tiles_.find(key);
        return it == tiles_.end() ? read(point) : rasterLayerPixelAt(it->second->preview,
             {point.x - key.first * Side, point.y - key.second * Side});
    }
    template <class Visitor> void visit(Visitor visitor) const
    {
        for (const auto &[coordinate, tile] : tiles_)
            visitor(
                DevicePixelRect{{coordinate.first * Side, coordinate.second * Side}, Side, Side},
                *tile->original, tile->preview);
    }
    [[nodiscard]] std::size_t storageBytes() const noexcept
    {
        return tiles_.size() *
               (sizeof(Tile) + sizeof(RasterLayer) + 4 * sizeof(void *) +
                Side * Side * (2 * sizeof(std::uint32_t) + sizeof(PremultipliedPixel)));
    }
};
} // namespace iiSharedCanvas::detail
