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
        RasterLayer original;
        StrokeCompositeBuffer pending;
        RasterLayer preview;
    };
    std::map<std::pair<int, int>, std::shared_ptr<Tile>> tiles_;
    static int column(int value)
    {
        return value >= 0 ? value / Side : (value + 1) / Side - 1;
    }

  public:
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
                tile->original = makeRasterLayer(Side, Side);
                tile->pending = makeStrokeCompositeBuffer(Side, Side);
                for (int y = 0; y < Side; ++y)
                    for (int x = 0; x < Side; ++x)
                        tile->original.pixels[std::size_t(y) * Side + x] =
                            read({left + x, top + y});
                tile->preview = tile->original;
            }
            else if (tile.use_count() != 1)
                tile = std::make_shared<Tile>(*tile);
            accumulateStrokeSamples(tile->pending, local);
            auto preview = tile->original;
            if (mode == RasterBlendMode::SourceOver || mode == RasterBlendMode::DestinationOut)
            {
                copyStrokePreviewSamplePixels(preview, tile->original, tile->pending, local,
                                              mode == RasterBlendMode::DestinationOut);
            }
            else
            {
                for (const auto &sample : local)
                {
                    const auto index = std::size_t(sample.position.y) * Side + sample.position.x;
                    const auto source = strokeCompositePixelAt(tile->pending, sample.position);
                    if (source >> 24)
                        preview.pixels[index] =
                            compositeRasterPixel(source, tile->original.pixels[index], mode);
                }
            }
            for (const auto &sample : local)
            {
                const auto index = std::size_t(sample.position.y) * Side + sample.position.x;
                tile->preview.pixels[index] = preview.pixels[index];
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
                   : rasterLayerPixelAt(it->second->original,
                                        {point.x - key.first * Side, point.y - key.second * Side});
    }
    template <class Visitor> void visit(Visitor visitor) const
    {
        for (const auto &[coordinate, tile] : tiles_)
            visitor(
                DevicePixelRect{{coordinate.first * Side, coordinate.second * Side}, Side, Side},
                tile->original, tile->preview);
    }
    [[nodiscard]] std::size_t storageBytes() const noexcept
    {
        return tiles_.size() *
               (sizeof(Tile) +
                Side * Side * (2 * sizeof(std::uint32_t) + sizeof(PremultipliedPixel)));
    }
};
} // namespace iiSharedCanvas::detail
