#include "Bitmap/BitmapEditJournal_p.h"
#include "Document/CanvasRegion_p.hpp"
#include "Validation/Validation.h"
#include <Render/DirtyRegion.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace iiSharedCanvas::detail
{
namespace
{
using Coordinate = std::pair<std::int32_t, std::int32_t>;
constexpr std::string_view Pixels = "pixels", Extent = "extent", Chunks = "chunks", Canvas = "canvas";
bool less(Coordinate a, Coordinate b)
{
    return std::pair{a.second, a.first} < std::pair{b.second, b.first};
}
struct CoordinateLess
{
    bool operator()(Coordinate a, Coordinate b) const { return less(a, b); }
};
bool same(DevicePixelRect a, DevicePixelRect b)
{
    return a.origin.x == b.origin.x && a.origin.y == b.origin.y && a.width == b.width &&
           a.height == b.height;
}
std::int32_t divide(std::int32_t value, std::int32_t size)
{
    return value / size - (value % size < 0);
}
bool contains(DevicePixelRect outer, DevicePixelRect inner)
{
    return inner.width > 0 && inner.height > 0 && inner.origin.x >= outer.origin.x &&
           inner.origin.y >= outer.origin.y &&
           std::int64_t(inner.origin.x) + inner.width <=
               std::int64_t(outer.origin.x) + outer.width &&
           std::int64_t(inner.origin.y) + inner.height <=
               std::int64_t(outer.origin.y) + outer.height;
}
std::uint32_t sample(const std::byte *data)
{
    std::uint32_t value;
    std::memcpy(&value, data, 4);
    return value;
}
auto asBytes(std::span<const std::uint32_t> data) { return std::as_bytes(data); }
struct Storage
{
    Document &document;
    std::string_view assetId_;
    BitmapCommitPort *commitPort = nullptr;
    RasterAsset *raster = nullptr;
    ChunkedRasterAsset *chunked = nullptr;
    int size = 0;
    RasterHistoryCode rejection = RasterHistoryCode::WriteRejected;
    explicit Storage(Document &d, std::string_view id, BitmapCommitPort *port = nullptr)
        : document(d), assetId_(id), commitPort(port)
    {
        auto it = std::find_if(d.assets.begin(), d.assets.end(),
                               [&](const auto &a) { return assetId(a) == id; });
        auto *a = it == d.assets.end() ? nullptr : &*it;
        if (a)
        {
            raster = std::get_if<RasterAsset>(a);
            chunked = std::get_if<ChunkedRasterAsset>(a);
        }
        size = d.infiniteCanvas.chunkSize;
    }
    bool valid() const
    {
        if (raster)
        {
            const auto &p = raster->pixels;
            return p.width > 0 && p.height > 0 &&
                   std::uint64_t(p.width) * p.height == p.pixels.size();
        }
        if (!chunked || size < 32 || size > 4096 || (size & (size - 1)) != 0 ||
            document.extent.width <= 0 || document.extent.height <= 0)
            return false;
        std::optional<Coordinate> prior;
        for (const auto &c : chunked->chunks)
        {
            const Coordinate key{c.column, c.row};
            if (prior && !less(*prior, key))
                return false;
            if (std::int64_t(c.column) * size < std::numeric_limits<std::int32_t>::min() ||
                std::int64_t(c.column) * size > std::numeric_limits<std::int32_t>::max() ||
                std::int64_t(c.row) * size < std::numeric_limits<std::int32_t>::min() ||
                std::int64_t(c.row) * size > std::numeric_limits<std::int32_t>::max())
                return false;
            if (c.pixels.width != size || c.pixels.height != size ||
                c.pixels.pixels.size() != std::uint64_t(size) * size)
                return false;
            prior = key;
        }
        return true;
    }
    DevicePixelRect bounds() const
    {
        if (raster)
            return {{0, 0}, raster->pixels.width, raster->pixels.height};
        auto o = canvasOrigin(document);
        return {{o.x, o.y}, document.extent.width, document.extent.height};
    }
    const RasterChunk *chunk(Coordinate key) const
    {
        if (!chunked)
            return nullptr;
        auto it =
            std::lower_bound(chunked->chunks.begin(), chunked->chunks.end(), key,
                             [](const auto &c, auto k) { return less({c.column, c.row}, k); });
        return it != chunked->chunks.end() && it->column == key.first && it->row == key.second
                   ? &*it
                   : nullptr;
    }
    std::uint32_t pixel(DevicePixelPoint p) const
    {
        if (raster)
            return rasterLayerPixelAt(raster->pixels, p);
        if (!chunked || size <= 0)
            return 0;
        const Coordinate key{divide(p.x, size), divide(p.y, size)};
        auto *c = chunk(key);
        return c ? rasterLayerPixelAt(c->pixels, {p.x - key.first * size, p.y - key.second * size})
                 : 0;
    }
    bool read(std::string_view plane, DevicePixelPoint p, unsigned sampleBytes,
              std::span<std::byte> bytes) const
    {
        if (plane == Pixels && sampleBytes == 4 && bytes.size() % 4 == 0)
        {
            const auto count = bytes.size() / 4;
            for (unsigned i = 0; i < count; ++i)
            {
                auto value = pixel({p.x + int(i), p.y});
                std::memcpy(bytes.data() + i * 4, &value, 4);
            }
            return true;
        }
        if (plane == Extent && raster && p.x == 0 && p.y == 0 && sampleBytes == 8 &&
            bytes.size() == 8)
        {
            const std::array<std::int32_t, 2> value{raster->pixels.width, raster->pixels.height};
            std::memcpy(bytes.data(), value.data(), 8);
            return true;
        }
        if (plane == Canvas && chunked && p.x == 0 && p.y == 0 && sampleBytes == 16 &&
            bytes.size() == 16)
        {
            const auto region = canvasRegion(document);
            const std::array<std::int32_t, 4> value{region.origin.x, region.origin.y,
                                                    region.extent.width, region.extent.height};
            std::memcpy(bytes.data(), value.data(), 16);
            return true;
        }
        if (plane == Chunks && chunked && sampleBytes == 1)
        {
            const auto count = bytes.size();
            for (unsigned i = 0; i < count; ++i)
                bytes[i] = std::byte(chunk({p.x + int(i), p.y}) != nullptr);
            return true;
        }
        return false;
    }
    static DocumentAuthorship changedAuthorship(Document &d)
    {
        auto result = d.authorship;
        result.recordChange();
        return result;
    }
    void stamp(DocumentAuthorship &authorship) noexcept
    {
        static_assert(std::is_nothrow_move_assignable_v<DocumentAuthorship>);
        document.authorship = std::move(authorship);
        document.formatVersion.minor = CurrentFormatMinor;
    }
    bool commit(std::span<const RasterHistoryPatch> patches,
                std::span<const BitmapChunkPresence> presence,
                const DocumentAuthorship &authorship,
                std::optional<CanvasRegion> canvasAfter = {})
    {
        if (!commitPort) return true;
        const PreparedBitmapChange prepared{&document, assetId_, raster, chunked, size,
            patches, presence, &authorship, canvasAfter, this,
            [](const void *context, DevicePixelPoint point) {
                return static_cast<const Storage *>(context)->pixel(point);
            }};
        return commitPort->commit(commitPort->context, prepared, rejection,
                                  commitPort->useDraftFallback);
    }
    bool writeFinite(std::span<const RasterHistoryPatch> patches, DevicePixelRect &dirty)
    {
        int width = raster->pixels.width, height = raster->pixels.height;
        for (const auto &p : patches)
            if (p.plane == Extent)
            {
                if (p.sampleBytes != 8 || !same(p.bounds, {{0, 0}, 1, 1}) || p.data.size() != 8)
                    return false;
                std::array<std::int32_t, 2> dimensions;
                std::memcpy(dimensions.data(), p.data.data(), 8);
                width = dimensions[0];
                height = dimensions[1];
            }
        if (width <= 0 || height <= 0 ||
            std::uint64_t(width) * height > std::numeric_limits<std::size_t>::max() / 4)
            return false;
        const bool resized = width != raster->pixels.width || height != raster->pixels.height;
        if (resized && commitPort)
        {
            commitPort->useDraftFallback = true;
            return false;
        }
        std::optional<RasterLayer> replacement;
        if (resized)
        {
            replacement = makeRasterLayer(width, height);
            for (int y = 0; y < std::min(height, raster->pixels.height); ++y)
                std::copy_n(raster->pixels.pixels.data() + std::size_t(y) * raster->pixels.width,
                            std::min(width, raster->pixels.width),
                            replacement->pixels.data() + std::size_t(y) * width);
        }
        auto &target = replacement ? *replacement : raster->pixels;
        struct Row
        {
            std::size_t offset;
            std::span<const std::byte> data;
        };
        std::vector<Row> rows;
        std::map<int, std::vector<std::pair<int, int>>> removalCoverage;
        bool changed = resized;
        if (resized)
            dirty = uniteDevicePixelRects({{0, 0}, raster->pixels.width, raster->pixels.height},
                                          {{0, 0}, width, height});
        for (const auto &p : patches)
        {
            if (p.plane == Extent)
                continue;
            if (p.plane != Pixels || p.sampleBytes != 4 ||
                p.data.size() != std::uint64_t(p.bounds.width) * p.bounds.height * 4)
                return false;
            for (int y = 0; y < p.bounds.height; ++y)
            {
                const auto worldY = std::int64_t(p.bounds.origin.y) + y;
                const auto *line = p.data.data() + std::size_t(y) * p.bounds.width * 4;
                const auto left = std::max(0, p.bounds.origin.x);
                const auto right =
                    std::min<std::int64_t>(width, std::int64_t(p.bounds.origin.x) + p.bounds.width);
                for (int x = 0; x < p.bounds.width; ++x)
                {
                    const auto worldX = std::int64_t(p.bounds.origin.x) + x;
                    if ((worldX < 0 || worldX >= width || worldY < 0 || worldY >= height) &&
                        sample(line + std::size_t(x) * 4) != 0)
                        return false;
                }
                if (resized && worldY >= 0 && worldY < raster->pixels.height)
                {
                    const auto first = std::max(0, p.bounds.origin.x);
                    const auto last = std::min<std::int64_t>(
                        raster->pixels.width, std::int64_t(p.bounds.origin.x) + p.bounds.width);
                    if (first < last)
                        removalCoverage[int(worldY)].push_back({first, int(last)});
                }
                if (worldY < 0 || worldY >= height || left >= right)
                    continue;
                const auto offset = std::size_t(worldY) * width + left;
                const std::span data{line + std::size_t(left - p.bounds.origin.x) * 4,
                                     std::size_t(right - left) * 4};
                if (std::memcmp(target.pixels.data() + offset, data.data(), data.size()) != 0)
                {
                    rows.push_back({offset, data});
                    changed = true;
                    dirty =
                        uniteDevicePixelRects(dirty, {{left, int(worldY)}, int(right - left), 1});
                }
            }
        }
        if (!changed)
            return true;
        // A size delta must not discard unrelated pixels added after the edit.
        if (resized && (width < raster->pixels.width || height < raster->pixels.height))
        {
            for (auto &[y, ranges] : removalCoverage)
                std::ranges::sort(ranges);
            for (int y = 0; y < raster->pixels.height; ++y)
            {
                const auto first = y < height ? std::min(width, raster->pixels.width) : 0;
                auto it = removalCoverage.find(y);
                std::size_t interval = 0;
                for (int x = first; x < raster->pixels.width; ++x)
                {
                    if (!raster->pixels.pixels[std::size_t(y) * raster->pixels.width + x])
                        continue;
                    if (it != removalCoverage.end())
                        while (interval < it->second.size() && it->second[interval].second <= x)
                            ++interval;
                    if (it == removalCoverage.end() || interval == it->second.size() ||
                        it->second[interval].first > x)
                    {
                        rejection = RasterHistoryCode::Conflict;
                        return false;
                    }
                }
            }
        }
        auto authorship =
            changedAuthorship(document); // All allocations precede any live pixel write.
        if (!commit(patches, {}, authorship)) return false;
        for (const auto &row : rows)
            std::memcpy(target.pixels.data() + row.offset, row.data.data(), row.data.size());
        if (replacement)
            raster->pixels = std::move(*replacement);
        stamp(authorship);
        return true;
    }
    bool writeChunks(std::span<const RasterHistoryPatch> patches, DevicePixelRect &dirty)
    {
        struct Change
        {
            const RasterChunk *before = nullptr;
            bool after = false;
            std::optional<RasterChunk> created;
            std::map<int, std::vector<std::pair<int, int>>> removedCoverage;
        };
        struct Row
        {
            std::uint32_t *destination;
            std::span<const std::byte> data;
        };
        std::map<Coordinate, Change, CoordinateLess> changes;
        std::vector<Row> rows;
        bool changed = false;
        std::optional<CanvasRegion> nextCanvas;
        for (const auto &patch : patches)
            if (patch.plane == Canvas)
            {
                if (nextCanvas || patch.sampleBytes != 16 ||
                    !same(patch.bounds, {{0, 0}, 1, 1}) || patch.data.size() != 16)
                    return false;
                std::array<std::int32_t, 4> value;
                std::memcpy(value.data(), patch.data.data(), 16);
                nextCanvas = CanvasRegion{{value[0], value[1]}, {value[2], value[3]}};
            }
        if (nextCanvas && *nextCanvas != canvasRegion(document))
        {
            struct RestoreCanvas {
                Document &document;
                CanvasRegion previous;
                ~RestoreCanvas() { document.infiniteCanvas.origin = previous.origin; document.extent = previous.extent; }
            } restore{document, canvasRegion(document)};
            document.infiniteCanvas.origin = nextCanvas->origin;
            document.extent = nextCanvas->extent;
            if (!validate(document).ok()) return false;
            changed = true;
        }
        const auto prepare = [&](Coordinate key, bool present) -> Change &
        {
            auto [it, inserted] = changes.try_emplace(key);
            if (inserted)
            {
                auto &entry = it->second;
                entry.before = chunk(key);
                entry.after = present;
                if (present && !entry.before)
                    entry.created = RasterChunk{key.first, key.second, makeRasterLayer(size, size)};
            }
            return it->second;
        };
        for (const auto &p : patches)
            if (p.plane == Chunks)
            {
                if (p.sampleBytes != 1 || p.bounds.width <= 0 || p.bounds.height != 1 ||
                    p.data.size() != std::size_t(p.bounds.width))
                    return false;
                for (int i = 0; i < p.bounds.width; ++i)
                {
                    const auto x = std::int64_t(p.bounds.origin.x) + i;
                    if (x < INT32_MIN || x > INT32_MAX) return false;
                    const auto present = std::to_integer<unsigned>(p.data[i]);
                    if (present > 1) return false;
                    const Coordinate key{std::int32_t(x), p.bounds.origin.y};
                    if (changes.contains(key)) return false;
                    const auto &entry = prepare(key, present != 0);
                    if (bool(entry.before) != entry.after)
                    {
                        changed = true;
                        dirty = uniteDevicePixelRects(dirty, bounds());
                    }
                }
            }
        for (const auto &p : patches)
        {
            if (p.plane == Chunks || p.plane == Canvas) continue;
            if (p.plane != Pixels || p.sampleBytes != 4 || p.bounds.width <= 0 ||
                p.bounds.height <= 0 ||
                p.data.size() != std::uint64_t(p.bounds.width) * p.bounds.height * 4)
                return false;
            for (int y = 0; y < p.bounds.height; ++y)
                for (int x = 0; x < p.bounds.width;)
                {
                    const auto wx = std::int64_t(p.bounds.origin.x) + x;
                    const auto wy = std::int64_t(p.bounds.origin.y) + y;
                    if (wx < INT32_MIN || wx > INT32_MAX || wy < INT32_MIN || wy > INT32_MAX)
                        return false;
                    const auto worldX = std::int32_t(wx), worldY = std::int32_t(wy);
                    const Coordinate key{divide(worldX, size), divide(worldY, size)};
                    const int localX = int(wx - std::int64_t(key.first) * size);
                    const int localY = int(wy - std::int64_t(key.second) * size);
                    const int count = std::min(p.bounds.width - x, size - localX);
                    const auto *data = p.data.data() + (std::size_t(y) * p.bounds.width + x) * 4;
                    auto it = changes.find(key);
                    if (it == changes.end())
                    {
                        if (!chunk(key)) return false;
                        prepare(key, true);
                        it = changes.find(key);
                    }
                    auto &entry = it->second;
                    if (!entry.after)
                    {
                        for (int i = 0; i < count; ++i)
                            if (sample(data + std::size_t(i) * 4) != 0) return false;
                        if (entry.before)
                            entry.removedCoverage[localY].push_back({localX, localX + count});
                    }
                    else
                    {
                        auto *destination = entry.created
                            ? entry.created->pixels.pixels.data() + std::size_t(localY) * size + localX
                            : const_cast<std::uint32_t *>(entry.before->pixels.pixels.data()) +
                                  std::size_t(localY) * size + localX;
                        const std::span dataView{data, std::size_t(count) * 4};
                        const bool differs = std::memcmp(destination, data, dataView.size()) != 0;
                        if (entry.created)
                            std::memcpy(destination, data, dataView.size());
                        else
                            rows.push_back({destination, dataView});
                        if (differs)
                        {
                            changed = true;
                            dirty = uniteDevicePixelRects(dirty, {{worldX, worldY}, count, 1});
                        }
                    }
                    x += count;
                }
        }
        // Removal is safe only when every previously nonzero pixel is covered by
        // a zero patch. Track row intervals instead of cloning the whole chunk.
        for (auto &[key, entry] : changes)
        {
            if (entry.after || !entry.before) continue;
            for (auto &[y, ranges] : entry.removedCoverage) std::ranges::sort(ranges);
            for (int y = 0; y < size; ++y)
            {
                const auto ranges = entry.removedCoverage.find(y);
                std::size_t interval = 0;
                for (int x = 0; x < size; ++x)
                {
                    if (!entry.before->pixels.pixels[std::size_t(y) * size + x]) continue;
                    if (ranges != entry.removedCoverage.end())
                        while (interval < ranges->second.size() && ranges->second[interval].second <= x)
                            ++interval;
                    if (ranges == entry.removedCoverage.end() || interval == ranges->second.size() ||
                        ranges->second[interval].first > x)
                    {
                        rejection = RasterHistoryCode::Conflict;
                        return false;
                    }
                }
            }
        }
        if (!changed) return true;
        std::vector<BitmapChunkPresence> presence;
        presence.reserve(changes.size());
        for (const auto &[key, entry] : changes)
            if (bool(entry.before) != entry.after)
                presence.push_back({key.first, key.second, bool(entry.before), entry.after});
        std::vector<RasterChunk> next;
        if (!presence.empty()) next.reserve(chunked->chunks.size() + presence.size());
        auto authorship = changedAuthorship(document);
        if (!commit(patches, presence, authorship, nextCanvas)) return false;
        for (const auto &row : rows)
            std::memcpy(row.destination, row.data.data(), row.data.size());
        if (!presence.empty())
        {
            static_assert(std::is_nothrow_move_constructible_v<RasterChunk>);
            auto it = changes.begin();
            std::size_t old = 0;
            while (old < chunked->chunks.size() || it != changes.end())
            {
                if (it == changes.end())
                    next.push_back(std::move(chunked->chunks[old++]));
                else if (old == chunked->chunks.size() ||
                         less(it->first, {chunked->chunks[old].column, chunked->chunks[old].row}))
                {
                    if (it->second.created) next.push_back(std::move(*it->second.created));
                    ++it;
                }
                else if (less({chunked->chunks[old].column, chunked->chunks[old].row}, it->first))
                    next.push_back(std::move(chunked->chunks[old++]));
                else
                {
                    if (it->second.after) next.push_back(std::move(chunked->chunks[old]));
                    ++it;
                    ++old;
                }
            }
            chunked->chunks = std::move(next);
        }
        if (nextCanvas) {
            document.infiniteCanvas.origin = nextCanvas->origin;
            document.extent = nextCanvas->extent;
        }
        stamp(authorship);
        return true;
    }
    bool write(std::span<const RasterHistoryPatch> patches, DevicePixelRect &dirty)
    {
        return raster ? writeFinite(patches, dirty) : writeChunks(patches, dirty);
    }
};
std::vector<RasterHistoryPatch> views(std::span<const BitmapPatch> patches,
                                      std::optional<CanvasExtent> &extent,
                                      std::map<Coordinate, std::uint8_t> &presence,
                                      std::array<std::int32_t, 2> &dimensions,
                                      std::optional<CanvasRegion> &canvas,
                                      std::array<std::int32_t, 4> &canvasValues)
{
    std::vector<RasterHistoryPatch> result;
    result.reserve(patches.size() + presence.size() + bool(extent) + bool(canvas));
    for (const auto &p : patches)
        result.push_back({Pixels, p.bounds, 4, asBytes(p.pixels)});
    if (extent)
    {
        dimensions = {extent->width, extent->height};
        result.push_back({Extent, {{0, 0}, 1, 1}, 8, std::as_bytes(std::span(dimensions))});
    }
    if (canvas)
    {
        canvasValues = {canvas->origin.x, canvas->origin.y, canvas->extent.width, canvas->extent.height};
        result.push_back({Canvas, {{0, 0}, 1, 1}, 16, std::as_bytes(std::span(canvasValues))});
    }
    for (auto &[key, value] : presence)
        result.push_back(
            {Chunks, {{key.first, key.second}, 1, 1}, 1, std::as_bytes(std::span(&value, 1))});
    return result;
}
bool validInput(std::span<const BitmapPatch> patches, DevicePixelRect bounds,
                const RasterHistoryLimits &limits, bool clipped = true)
{
    if (patches.size() > limits.maximumPatches)
        return false;
    std::size_t bytes = 0;
    for (const auto &p : patches)
    {
        if (p.bounds.width <= 0 || p.bounds.height <= 0 ||
            (clipped && !contains(bounds, p.bounds)) || p.bounds.origin.x < -1000000000 ||
            p.bounds.origin.y < -1000000000 ||
            std::int64_t(p.bounds.origin.x) + p.bounds.width > 1000000000 ||
            std::int64_t(p.bounds.origin.y) + p.bounds.height > 1000000000 ||
            std::uint64_t(p.bounds.width) * p.bounds.height != p.pixels.size())
            return false;
        if (p.pixels.size() > limits.maximumInputBytes / 4 ||
            bytes > limits.maximumInputBytes - p.pixels.size() * 4)
            return false;
        bytes += p.pixels.size() * 4;
    }
    return true;
}
template <class Before>
std::map<Coordinate, std::uint8_t> presenceFor(Storage &storage,
                                               std::span<const BitmapPatch> patches, Before before)
{
    std::map<Coordinate, std::uint8_t> flags;
    if (!storage.chunked)
        return flags;
    for (const auto &p : patches)
        for (int y = 0; y < p.bounds.height; ++y)
            for (int x = 0; x < p.bounds.width; ++x)
            {
                const Coordinate key{divide(p.bounds.origin.x + x, storage.size),
                                     divide(p.bounds.origin.y + y, storage.size)};
                auto [it, inserted] = flags.try_emplace(key);
                if (inserted)
                    it->second = before(key);
                if (p.pixels[std::size_t(y) * p.bounds.width + x] != 0)
                    it->second = 1;
            }
    return flags;
}
bool nonOverlapping(std::span<const BitmapPatch> patches)
{
    struct Event
    {
        std::int64_t x, top, bottom;
        bool start;
    };
    std::vector<Event> events;
    events.reserve(patches.size() * 2);
    for (const auto &patch : patches)
    {
        const auto &b = patch.bounds;
        events.push_back({b.origin.x, b.origin.y, std::int64_t(b.origin.y) + b.height, true});
        events.push_back({std::int64_t(b.origin.x) + b.width, b.origin.y,
                          std::int64_t(b.origin.y) + b.height, false});
    }
    std::ranges::sort(events, [](const auto &a, const auto &b)
                      { return std::tie(a.x, a.start, a.top) < std::tie(b.x, b.start, b.top); });
    std::map<std::int64_t, std::int64_t> active;
    for (const auto &event : events)
    {
        if (!event.start)
        {
            active.erase(event.top);
            continue;
        }
        const auto next = active.lower_bound(event.top);
        if (next != active.end() && next->first < event.bottom)
            return false;
        if (next != active.begin() && std::prev(next)->second > event.top)
            return false;
        active.emplace_hint(next, event.top, event.bottom);
    }
    return true;
}
struct OwnedPatches
{
    std::vector<std::vector<std::uint32_t>> data;
    std::vector<BitmapPatch> patches;
    void tile(DevicePixelRect bounds, const RasterLayer &pixels, DevicePixelRect clip)
    {
        const auto region = intersectDevicePixelRects(bounds, clip);
        if (isEmpty(region))
            return;
        auto &buffer = data.emplace_back(std::size_t(region.width) * region.height);
        for (int y = 0; y < region.height; ++y)
            std::copy_n(pixels.pixels.data() +
                            std::size_t(region.origin.y - bounds.origin.y + y) * pixels.width +
                            region.origin.x - bounds.origin.x,
                        region.width, buffer.data() + std::size_t(y) * region.width);
        patches.push_back({region, buffer});
    }
};
DevicePixelRect regionBounds(CanvasRegion region) noexcept
{
    return {{region.origin.x, region.origin.y}, region.extent.width, region.extent.height};
}
constexpr DevicePixelRect PreviewWorld{{-1000000000, -1000000000}, 2000000000, 2000000000};
} // namespace

bool BitmapEditJournal::checkGeometry(Document &d, std::string_view id)
{
    Storage storage(d, id);
    if (!storage.valid())
    {
        result = {RasterHistoryCode::ReadRejected};
        return false;
    }
    const auto previous = geometry_;
    updateGeometry(d, id);
    if (previous && *previous != *geometry_)
    {
        geometry_ = previous;
        result = {RasterHistoryCode::Conflict};
        return false;
    }
    return true;
}
void BitmapEditJournal::updateGeometry(Document &d, std::string_view id)
{
    Storage storage(d, id);
    auto b = storage.bounds();
    geometry_ = Geometry{bool(storage.chunked),
                         b.origin.x,
                         b.origin.y,
                         b.width,
                         b.height,
                         storage.chunked ? storage.size : 0};
}
std::size_t BitmapEditJournal::previewBytes() const noexcept
{
    return (stroke_ ? stroke_->storageBytes() : 0) +
           (pixelPreview_ ? pixelPreview_->storageBytes() : 0) +
           originalChunks_.size() * (sizeof(Coordinate) + 1);
}
std::vector<BitmapPreviewTile> BitmapEditJournal::stagedPixelTiles() const
{
    return stagedPixels() ? pixelPreview_->tiles() : std::vector<BitmapPreviewTile>{};
}
std::optional<CanvasRegion> BitmapEditJournal::stagedPixelRegion() const noexcept
{
    return stagedPixels() ? stagedRegion_ : std::nullopt;
}
DevicePixelRect BitmapEditJournal::discardStagedPixels() noexcept
{
    if (!stagedPixels()) return {};
    const BitmapPixelPreview empty;
    const auto changed = pixelPreview_->difference(empty, stagedClip_);
    finish();
    return changed;
}
RasterHistoryResult BitmapEditJournal::apply(Document &d, std::string_view id,
                                             std::span<const BitmapPatch> patches,
                                             std::optional<CanvasExtent> extent,
                                             std::map<Coordinate, std::uint8_t> presence,
                                             std::string_view label,
                                             const BitmapStrokeComposite *original,
                                             const BitmapPixelPreview *replacement,
                                             std::optional<CanvasRegion> canvas)
{
    const bool previewing = original || replacement;
    dirty = {};
    try
    {
        if (!checkGeometry(d, id))
            return result;
        Storage storage(d, id, commitPort_);
        std::array<std::int32_t, 2> dimensions;
        std::array<std::int32_t, 4> canvasValues;
        auto writeViews = views(patches, extent, presence, dimensions, canvas, canvasValues);
        auto next = previewing ? *previewBase_ : history_;
        const auto reader = [&](std::string_view plane, DevicePixelPoint p, unsigned sampleBytes,
                                std::span<std::byte> bytes)
        {
            if (previewing && plane == Pixels && sampleBytes == 4 && bytes.size() % 4 == 0)
            {
                const auto count = bytes.size() / 4;
                for (unsigned i = 0; i < count; ++i)
                {
                    const DevicePixelPoint at{p.x + int(i), p.y};
                    const auto fallback = [&](auto q) { return storage.pixel(q); };
                    const auto value = original ? original->originalPixel(at, fallback)
                                                : replacement->originalPixel(at, fallback);
                    std::memcpy(bytes.data() + i * 4, &value, 4);
                }
                return true;
            }
            if (previewing && plane == Chunks && sampleBytes == 1)
            {
                const auto count = bytes.size();
                for (unsigned i = 0; i < count; ++i)
                {
                    auto it = originalChunks_.find({p.x + int(i), p.y});
                    bytes[i] = std::byte(it == originalChunks_.end()
                                             ? bool(storage.chunk({p.x + int(i), p.y}))
                                             : it->second);
                }
                return true;
            }
            const auto initialCanvas = strokeCanvas_ ? strokeCanvas_ : pixelCanvas_;
            if (previewing && plane == Canvas && initialCanvas && p.x == 0 && p.y == 0 &&
                sampleBytes == 16 && bytes.size() == 16)
            {
                const std::array<std::int32_t, 4> initial{initialCanvas->origin.x, initialCanvas->origin.y,
                    initialCanvas->extent.width, initialCanvas->extent.height};
                std::memcpy(bytes.data(), initial.data(), 16);
                return true;
            }
            return storage.read(plane, p, sampleBytes, bytes);
        };
        bool wrote = false;
        const auto writer = [&](std::span<const RasterHistoryPatch> changed)
        {
            wrote = true;
            if (stagedPixels_ && replacement) return true;
            return storage.write(
                previewing ? std::span<const RasterHistoryPatch>(writeViews) : changed, dirty);
        };
        result = next.apply(writeViews, reader, writer, label);
        // An unchanged final stroke may still replace an earlier preview.
        if (result.ok() && previewing && !wrote && !(stagedPixels_ && replacement) &&
            !storage.write(writeViews, dirty))
            result = {RasterHistoryCode::WriteRejected};
        if (result.code == RasterHistoryCode::WriteRejected)
            result.code = storage.rejection;
        if (result.ok())
        {
            history_ = next;
            updateGeometry(d, id);
        }
    }
    catch (const std::bad_alloc &)
    {
        result = {RasterHistoryCode::BudgetExceeded};
    }
    catch (const std::exception &)
    {
        result = {RasterHistoryCode::WriteRejected};
    }
    return result;
}
RasterHistoryResult BitmapEditJournal::beginPixels(Document &d, std::string_view id,
                                                   std::string_view label)
{
    dirty = {};
    try
    {
        if (previewBase_ || stroke_ || pixelPreview_)
            return result = {RasterHistoryCode::InvalidArgument};
        if (label.size() > history_.limits().maximumInputBytes)
            return result = {RasterHistoryCode::BudgetExceeded};
        if (!checkGeometry(d, id))
            return result;
        auto pixels = std::make_shared<BitmapPixelPreview>();
        std::string nextLabel(label);
        previewBase_ = history_;
        pixelPreview_ = std::move(pixels);
        previewLabel_ = std::move(nextLabel);
        if (stagedPixels_)
        {
            Storage storage(d, id);
            stagedRegion_ = canvasRegion(d);
            stagedClip_ = storage.bounds();
            pixelCanvas_ = growStagedPixels_ ? stagedRegion_ : std::nullopt;
        }
        originalChunks_.clear();
        previewChanged_ = false;
        previewAdmission_ = {};
        return result = {};
    }
    catch (const std::bad_alloc &)
    {
        return result = {RasterHistoryCode::BudgetExceeded};
    }
}
RasterHistoryResult BitmapEditJournal::replacePixelPreview(Document &d, std::string_view id,
                                                           std::span<const BitmapPatch> patches)
{
    dirty = {};
    try
    {
        if (!pixelPreview_ || !previewBase_)
            return result = {RasterHistoryCode::InvalidArgument};
        if (!checkGeometry(d, id))
            return result;
        Storage storage(d, id);
        const auto &limits = history_.limits();
        if (patches.size() > limits.maximumPatches)
            return result = {RasterHistoryCode::BudgetExceeded};
        std::size_t inputBytes = 0;
        for (const auto &patch : patches)
        {
            if (patch.pixels.size() > (limits.maximumInputBytes - inputBytes) / 4)
                return result = {RasterHistoryCode::BudgetExceeded};
            inputBytes += patch.pixels.size() * 4;
        }
        if (!validInput(patches, storage.bounds(), limits, !growStagedPixels_) || !nonOverlapping(patches))
            return result = {RasterHistoryCode::InvalidArgument};
        const auto read = [&](auto point) { return storage.pixel(point); };
        if (!(stagedPixels_ ? pixelPreview_->matchesOriginal(stagedClip_, read)
                           : pixelPreview_->matches(storage.bounds(), read)))
            return result = {RasterHistoryCode::Conflict};
        // A staged preview never changed the source, so omitted tiles need not
        // be retained merely to restore them. Live previews still need their
        // prior footprint to restore pixels already published to the owner.
        auto candidate = stagedPixels_ ? std::make_shared<BitmapPixelPreview>()
                                       : std::make_shared<BitmapPixelPreview>(*pixelPreview_);
        if (!candidate->fits(patches, limits.maximumInputBytes))
            return result = {RasterHistoryCode::BudgetExceeded};
        candidate->replace(patches, read);
        if (candidate->storageBytes() > limits.maximumInputBytes)
            return result = {RasterHistoryCode::BudgetExceeded};
        auto outputBounds = storage.bounds();
        auto nextRegion = stagedRegion_;
        if (pixelCanvas_)
        {
            const BitmapPixelPreview empty;
            const auto changed = candidate->difference(empty, PreviewWorld);
            nextRegion = *pixelCanvas_;
            if (!isEmpty(changed) && !contains(regionBounds(*pixelCanvas_), changed))
            {
                nextRegion = expandedCanvasRegion(*pixelCanvas_,
                    {{changed.origin.x, changed.origin.y}, {changed.width, changed.height}}, storage.size);
                if (!nextRegion) return result = {RasterHistoryCode::InvalidArgument};
            }
            outputBounds = uniteDevicePixelRects(outputBounds, regionBounds(*nextRegion));
        }
        const auto differenceBounds = stagedPixels_ ?
            candidate->difference(*pixelPreview_, uniteDevicePixelRects(outputBounds, stagedClip_)) :
            DevicePixelRect{};
        OwnedPatches output;
        candidate->visit([&](auto bounds, const auto &, const auto &pixels)
                         { output.tile(bounds, pixels, outputBounds); });
        auto originals = stagedPixels_ ? std::map<Coordinate, std::uint8_t>{} : originalChunks_;
        auto flags = presenceFor(storage, output.patches,
                                 [&](auto key)
                                 {
                                     auto [it, inserted] = originals.try_emplace(key);
                                     if (inserted)
                                         it->second = bool(storage.chunk(key));
                                     return it->second;
                                 });
        originalChunks_.swap(originals);
        const auto applied = apply(d, id, output.patches, {}, std::move(flags), previewLabel_,
                                   nullptr, candidate.get(), pixelCanvas_ ? nextRegion : std::nullopt);
        if (applied.ok())
        {
            pixelPreview_ = std::move(candidate);
            previewChanged_ = applied.changed;
            previewAdmission_ = applied;
            if (stagedPixels_)
            {
                stagedRegion_ = nextRegion;
                stagedClip_ = outputBounds;
                dirty = differenceBounds;
            }
        }
        else
            originalChunks_.swap(originals);
        return applied;
    }
    catch (const std::bad_alloc &)
    {
        return result = {RasterHistoryCode::BudgetExceeded};
    }
}
RasterHistoryResult BitmapEditJournal::commitPixels(Document &d, std::string_view id)
{
    dirty = {};
    if (!pixelPreview_ || !previewBase_)
        return result = {RasterHistoryCode::InvalidArgument};
    if (!checkGeometry(d, id))
        return result;
    Storage storage(d, id);
    const auto read = [&](auto point) { return storage.pixel(point); };
    if (!(stagedPixels_ ? pixelPreview_->matchesOriginal(stagedClip_, read)
                       : pixelPreview_->matches(storage.bounds(), read)))
        return result = {RasterHistoryCode::Conflict};
    if (stagedPixels_ && previewChanged_)
    {
        try
        {
            OwnedPatches output;
            pixelPreview_->visit([&](auto bounds, const auto &, const auto &pixels)
                                 { output.tile(bounds, pixels, stagedClip_); });
            auto flags = presenceFor(storage, output.patches, [&](auto key) {
                const auto it = originalChunks_.find(key);
                return it == originalChunks_.end() ? bool(storage.chunk(key)) : bool(it->second);
            });
            RasterHistoryResult applied;
            {
                struct RestoreStaging {
                    bool &value;
                    ~RestoreStaging() { value = true; }
                } restore{stagedPixels_};
                stagedPixels_ = false;
                applied = apply(d, id, output.patches, {}, std::move(flags), previewLabel_,
                                nullptr, pixelPreview_.get(), pixelCanvas_ ? stagedRegion_ : std::nullopt);
            }
            if (!applied.ok()) return result = applied;
            finish();
            return result = applied;
        }
        catch (const std::bad_alloc &) { return result = {RasterHistoryCode::BudgetExceeded}; }
        catch (...) { return result = {RasterHistoryCode::WriteRejected}; }
    }
    const auto admitted = previewAdmission_;
    finish();
    return result = admitted;
}
RasterHistoryResult BitmapEditJournal::replace(Document &d, std::string_view id,
                                               std::span<const BitmapPatch> patches)
{
    dirty = {};
    try
    {
        Storage s(d, id);
        if (!s.valid() || !validInput(patches, s.bounds(), history_.limits()))
            return result = {RasterHistoryCode::InvalidArgument};
        auto flags = presenceFor(s, patches, [&](auto key) { return bool(s.chunk(key)); });
        return apply(d, id, patches, {}, std::move(flags), "Replace bitmap regions");
    }
    catch (const std::bad_alloc &)
    {
        return result = {RasterHistoryCode::BudgetExceeded};
    }
}
RasterHistoryResult BitmapEditJournal::replacePixels(Document &d, std::string_view id,
                                                     const RasterLayer &pixels)
{
    dirty = {};
    try
    {
        Storage s(d, id);
        if (!s.raster || pixels.width <= 0 || pixels.height <= 0 ||
            std::uint64_t(pixels.width) * pixels.height != pixels.pixels.size())
            return result = {RasterHistoryCode::InvalidArgument};
        const auto old = s.bounds();
        std::vector<BitmapPatch> patches{{{{0, 0}, pixels.width, pixels.height}, pixels.pixels}};
        std::vector<std::uint32_t> zeros;
        if (old.width > pixels.width || old.height > pixels.height)
        {
            zeros.resize(std::size_t(std::max(old.width, pixels.width)));
            if (old.width > pixels.width)
                for (int y = 0; y < old.height; ++y)
                    patches.push_back({{{pixels.width, y}, old.width - pixels.width, 1},
                                       std::span(zeros).first(old.width - pixels.width)});
            if (old.height > pixels.height)
                for (int y = pixels.height; y < old.height; ++y)
                    patches.push_back({{{0, y}, std::min(old.width, pixels.width), 1},
                                       std::span(zeros).first(std::min(old.width, pixels.width))});
        }
        if (!validInput(patches, {}, history_.limits(), false))
            return result = {RasterHistoryCode::BudgetExceeded};
        return apply(d, id, patches, CanvasExtent{pixels.width, pixels.height}, {},
                     "Replace bitmap pixels");
    }
    catch (const std::bad_alloc &)
    {
        return result = {RasterHistoryCode::BudgetExceeded};
    }
}
RasterHistoryResult BitmapEditJournal::clear(Document &d, std::string_view id, std::uint32_t argb)
{
    dirty = {};
    try
    {
        Storage s(d, id);
        if (!s.valid())
            return result = {RasterHistoryCode::InvalidArgument};
        std::vector<BitmapPatch> patches;
        std::vector<std::uint32_t> fill;
        std::map<Coordinate, std::uint8_t> flags;
        if (s.raster)
        {
            fill.resize(s.raster->pixels.width, argb);
            patches.reserve(s.raster->pixels.height);
            for (int y = 0; y < s.raster->pixels.height; ++y)
                patches.push_back({{{0, y}, s.raster->pixels.width, 1}, fill});
        }
        else
        {
            if (s.chunked->chunks.empty())
                return apply(d, id, {}, {}, {}, "Clear bitmap");
            fill.resize(std::size_t(s.size) * s.size);
            patches.reserve(s.chunked->chunks.size());
            for (const auto &c : s.chunked->chunks)
            {
                flags[{c.column, c.row}] = 0;
                patches.push_back({{{c.column * s.size, c.row * s.size}, s.size, s.size}, fill});
            }
        }
        return apply(d, id, patches, {}, std::move(flags), "Clear bitmap");
    }
    catch (const std::bad_alloc &)
    {
        return result = {RasterHistoryCode::BudgetExceeded};
    }
}
RasterHistoryResult BitmapEditJournal::replaceRegion(Document &d, std::string_view id,
                                                     CanvasOrigin origin, const RasterLayer &pixels)
{
    dirty = {};
    try
    {
        Storage s(d, id);
        const DevicePixelRect region{{origin.x, origin.y}, pixels.width, pixels.height};
        if (!s.chunked || !same(region, s.bounds()) || pixels.width <= 0 || pixels.height <= 0 ||
            std::uint64_t(pixels.width) * pixels.height != pixels.pixels.size())
            return result = {RasterHistoryCode::InvalidArgument};
        std::vector<BitmapPatch> patches{{region, pixels.pixels}};
        std::vector<std::uint32_t> zeros(std::size_t(s.size) * s.size);
        auto flags = presenceFor(s, patches, [](auto) { return false; });
        for (const auto &c : s.chunked->chunks)
        {
            flags.try_emplace({c.column, c.row}, 0);
            const DevicePixelRect b{{c.column * s.size, c.row * s.size}, s.size, s.size};
            const auto inside = intersectDevicePixelRects(b, region);
            const auto add = [&](DevicePixelRect q)
            {
                if (!isEmpty(q))
                    patches.push_back({q, std::span(zeros).first(std::size_t(q.width) * q.height)});
            };
            if (isEmpty(inside))
                add(b);
            else
            {
                add({b.origin, b.width, inside.origin.y - b.origin.y});
                add({{b.origin.x, inside.origin.y + inside.height},
                     b.width,
                     b.origin.y + b.height - inside.origin.y - inside.height});
                add({{b.origin.x, inside.origin.y}, inside.origin.x - b.origin.x, inside.height});
                add({{inside.origin.x + inside.width, inside.origin.y},
                     b.origin.x + b.width - inside.origin.x - inside.width,
                     inside.height});
            }
        }
        if (!validInput(patches, {}, history_.limits(), false))
            return result = {RasterHistoryCode::BudgetExceeded};
        return apply(d, id, patches, {}, std::move(flags), "Replace chunked bitmap region");
    }
    catch (const std::bad_alloc &)
    {
        return result = {RasterHistoryCode::BudgetExceeded};
    }
}
RasterHistoryResult BitmapEditJournal::step(Document &d, std::string_view id, bool redo)
{
    dirty = {};
    try
    {
        if (!checkGeometry(d, id))
            return result;
        Storage s(d, id, commitPort_);
        const auto read = [&](auto plane, auto p, auto count, auto bytes)
        { return s.read(plane, p, count, bytes); };
        const auto write = [&](auto patches) { return s.write(patches, dirty); };
        result = redo ? history_.redo(read, write) : history_.undo(read, write);
        if (result.code == RasterHistoryCode::WriteRejected)
            result.code = s.rejection;
        if (result.ok())
            updateGeometry(d, id);
    }
    catch (const std::bad_alloc &)
    {
        result = {RasterHistoryCode::BudgetExceeded};
    }
    catch (const std::exception &)
    {
        result = {RasterHistoryCode::WriteRejected};
    }
    return result;
}
void BitmapEditJournal::begin(Document *document)
{
    auto next = std::make_shared<BitmapStrokeComposite>();
    previewBase_ = history_;
    stroke_ = std::move(next);
    strokeCanvas_ = growStrokeCanvas_ && document ? std::optional(canvasRegion(*document)) : std::nullopt;
    originalChunks_.clear();
    previewChanged_ = false;
    previewAdmission_ = {};
    dirty = {};
    result = {};
}
void BitmapEditJournal::finish() noexcept
{
    stroke_.reset();
    pixelPreview_.reset();
    previewLabel_.clear();
    previewBase_.reset();
    originalChunks_.clear();
    strokeCanvas_.reset();
    stagedPixels_ = false;
    growStagedPixels_ = false;
    pixelCanvas_.reset();
    stagedRegion_.reset();
    stagedClip_ = {};
    previewChanged_ = false;
    previewAdmission_ = {};
}
std::uint32_t BitmapEditJournal::sourcePixel(Document &d, std::string_view id,
                                             DevicePixelPoint p) const
{
    Storage s(d, id);
    const auto read = [&](auto q) { return s.pixel(q); };
    if (pixelPreview_)
        return pixelPreview_->originalPixel(p, read);
    return stroke_ ? stroke_->originalPixel(p, read) : s.pixel(p);
}
BitmapEditSource::BitmapEditSource(const BitmapEditJournal &journal, Document &d,
                                   std::string_view id, CanvasOrigin o)
    : origin(o), original(journal.stroke_.get())
{
    Storage storage(d, id);
    if (storage.valid())
    {
        raster = storage.raster ? &storage.raster->pixels : nullptr;
        chunks = storage.chunked;
        chunkSize = storage.size;
    }
}
std::uint32_t BitmapEditSource::sample(DevicePixelPoint local) const
{
    const auto x = std::int64_t(origin.x) + local.x, y = std::int64_t(origin.y) + local.y;
    if (x < std::numeric_limits<std::int32_t>::min() ||
        x > std::numeric_limits<std::int32_t>::max() ||
        y < std::numeric_limits<std::int32_t>::min() ||
        y > std::numeric_limits<std::int32_t>::max())
        return 0;
    const DevicePixelPoint world{std::int32_t(x), std::int32_t(y)};
    const auto read = [&](DevicePixelPoint p) -> std::uint32_t
    {
        if (raster)
            return rasterLayerPixelAt(*raster, p);
        if (!chunks || chunkSize <= 0)
            return 0;
        const Coordinate key{divide(p.x, chunkSize), divide(p.y, chunkSize)};
        auto it =
            std::lower_bound(chunks->chunks.begin(), chunks->chunks.end(), key,
                             [](const auto &c, auto k) { return less({c.column, c.row}, k); });
        return it != chunks->chunks.end() && it->column == key.first && it->row == key.second
                   ? rasterLayerPixelAt(it->pixels,
                                        {p.x - key.first * chunkSize, p.y - key.second * chunkSize})
                   : 0;
    };
    return original ? original->originalPixel(world, read) : read(world);
}
RasterHistoryResult BitmapEditJournal::cancel(Document &d, std::string_view id)
{
    dirty = {};
    if (!previewBase_)
        return result = {RasterHistoryCode::InvalidArgument};
    if (previewChanged_ && !step(d, id, false).ok())
        return result;
    if (!previewChanged_)
        result = {};
    history_ = *previewBase_;
    finish();
    return result;
}
RasterHistoryResult BitmapEditJournal::preview(Document &d, std::string_view id,
                                               const std::vector<RasterSample> &samples,
                                               RasterBlendMode mode)
{
    dirty = {};
    try
    {
        if (!stroke_ || !previewBase_)
            return result = {RasterHistoryCode::InvalidArgument};
        if (!checkGeometry(d, id))
            return result;
        Storage s(d, id);
        bool matches = true;
        stroke_->visit(
            [&](auto bounds, const auto &, const auto &preview)
            {
                const auto clip = intersectDevicePixelRects(bounds, s.bounds());
                for (int y = 0; y < clip.height && matches; ++y)
                    for (int x = 0; x < clip.width; ++x)
                        if (s.pixel({clip.origin.x + x, clip.origin.y + y}) !=
                            rasterLayerPixelAt(preview, {clip.origin.x + x - bounds.origin.x,
                                                         clip.origin.y + y - bounds.origin.y}))
                        {
                            matches = false;
                            break;
                        }
            });
        if (!matches)
            return result = {RasterHistoryCode::Conflict};
        auto candidate = std::make_shared<BitmapStrokeComposite>(*stroke_);
        candidate->paint(
            samples, mode, [&](DevicePixelPoint p) { return s.pixel(p); }, [](auto, auto) {});
        if (candidate->storageBytes() > history_.limits().maximumInputBytes)
            return result = {RasterHistoryCode::BudgetExceeded};
        std::optional<CanvasRegion> nextCanvas;
        auto outputBounds = s.bounds();
        if (strokeCanvas_)
        {
            bool haveChanges = false;
            std::int64_t left = 0, top = 0, right = 0, bottom = 0;
            candidate->visit([&](auto bounds, const auto &original, const auto &preview)
            {
                for (int y = 0; y < bounds.height; ++y)
                    for (int x = 0; x < bounds.width; ++x)
                    {
                        const auto at = std::size_t(y) * bounds.width + x;
                        if (original.pixels[at] == preview.pixels[at]) continue;
                        const auto wx = std::int64_t(bounds.origin.x) + x;
                        const auto wy = std::int64_t(bounds.origin.y) + y;
                        if (!haveChanges) { left = wx; top = wy; right = wx + 1; bottom = wy + 1; haveChanges = true; }
                        else { left = std::min(left, wx); top = std::min(top, wy);
                               right = std::max(right, wx + 1); bottom = std::max(bottom, wy + 1); }
                    }
            });
            nextCanvas = *strokeCanvas_;
            if (haveChanges)
            {
                if (left < -1000000000 || top < -1000000000 || right > 1000000000 ||
                    bottom > 1000000000 || right - left > INT32_MAX || bottom - top > INT32_MAX)
                    return result = {RasterHistoryCode::InvalidArgument};
                const DevicePixelRect changedBounds{{std::int32_t(left), std::int32_t(top)},
                                                     int(right - left), int(bottom - top)};
                const DevicePixelRect initialBounds{{strokeCanvas_->origin.x, strokeCanvas_->origin.y},
                    strokeCanvas_->extent.width, strokeCanvas_->extent.height};
                if (!contains(initialBounds, changedBounds))
                {
                    const CanvasRegion requested{{std::int32_t(left), std::int32_t(top)},
                                                 {int(right - left), int(bottom - top)}};
                    nextCanvas = expandedCanvasRegion(*strokeCanvas_, requested, s.size);
                    if (!nextCanvas) return result = {RasterHistoryCode::InvalidArgument};
                }
            }
            const DevicePixelRect target{{nextCanvas->origin.x, nextCanvas->origin.y},
                                          nextCanvas->extent.width, nextCanvas->extent.height};
            outputBounds = uniteDevicePixelRects(outputBounds, target);
        }
        OwnedPatches output;
        candidate->visit([&](auto bounds, const auto &, const auto &preview)
                         { output.tile(bounds, preview, outputBounds); });
        auto originals = originalChunks_;
        auto flags = presenceFor(s, output.patches,
                                 [&](auto key)
                                 {
                                     auto [it, inserted] = originals.try_emplace(key);
                                     if (inserted)
                                         it->second = bool(s.chunk(key));
                                     return it->second;
                                 });
        originalChunks_.swap(originals);
        const auto result =
            apply(d, id, output.patches, {}, std::move(flags), "Bitmap stroke", candidate.get(), nullptr, nextCanvas);
        if (result.ok())
        {
            stroke_ = std::move(candidate);
            previewChanged_ = result.changed;
            previewAdmission_ = result;
        }
        else
            originalChunks_.swap(originals);
        return result;
    }
    catch (const std::bad_alloc &)
    {
        return result = {RasterHistoryCode::BudgetExceeded};
    }
}
} // namespace iiSharedCanvas::detail
