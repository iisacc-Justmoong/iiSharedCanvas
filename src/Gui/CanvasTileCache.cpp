#include "Gui/CanvasTileCache.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace iiSharedCanvas {
namespace {

// 64-bit endpoints make overlap safe even at the edges of the 32-bit canvas.
bool validRegion(CanvasRegion r) noexcept
{
    return r.extent.width > 0 && r.extent.height > 0;
}
bool overlap(CanvasRegion a, CanvasRegion b) noexcept
{
    return std::int64_t(a.origin.x) < std::int64_t(b.origin.x) + b.extent.width
        && std::int64_t(b.origin.x) < std::int64_t(a.origin.x) + a.extent.width
        && std::int64_t(a.origin.y) < std::int64_t(b.origin.y) + b.extent.height
        && std::int64_t(b.origin.y) < std::int64_t(a.origin.y) + a.extent.height;
}
std::uint64_t pixels(const CanvasCachedTile &tile) noexcept
{
    return std::uint64_t(tile.image->width()) * tile.image->height();
}
bool validTile(const CanvasCachedTile &tile) noexcept
{
    return validRegion(tile.region) && tile.image && tile.image->width() && tile.image->height();
}

} // namespace

ii::core::Result<std::vector<FrameRenderTileRequest>> CanvasDirtyTilePlanner::execute(
    const CanvasTileCache &snapshot, std::uint64_t expectedRevision, std::span<const CanvasRegion> damaged,
    RasterSampling sampling, std::size_t maximumRequests) const
{
    using ii::core::ErrorCode;
    if (snapshot.revision != expectedRevision)
        return ii::core::fail(ErrorCode::busy, "Stale canvas tile revision");
    if (!maximumRequests || (sampling != RasterSampling::Nearest && sampling != RasterSampling::Smooth)
        || std::any_of(damaged.begin(), damaged.end(), [](auto r) { return !validRegion(r); }))
        return ii::core::fail(ErrorCode::invalid_argument, "Invalid canvas damage request");
    try {
        std::vector<FrameRenderTileRequest> requests;
        for (std::size_t i = 0; i < snapshot.tiles.size(); ++i) {
            const auto &tile = snapshot.tiles[i];
            if (!validTile(tile)) return ii::core::fail(ErrorCode::invalid_argument, "Invalid cached canvas tile");
            for (std::size_t j = 0; j < i; ++j)
                if (overlap(tile.region, snapshot.tiles[j].region))
                    return ii::core::fail(ErrorCode::invalid_argument, "Cached canvas tiles overlap");
            if (std::any_of(damaged.begin(), damaged.end(), [&](auto region) { return overlap(tile.region, region); })) {
                if (requests.size() == maximumRequests)
                    return ii::core::fail(ErrorCode::full, "Canvas damage exceeds the tile request limit");
                requests.push_back({tile.region, {int(tile.image->width()), int(tile.image->height())}, sampling});
            }
        }
        return requests;
    } catch (const std::bad_alloc &) {
        return ii::core::fail(ErrorCode::failed, "Cannot allocate canvas tile requests");
    } catch (const std::length_error &) {
        return ii::core::fail(ErrorCode::failed, "Canvas tile request list is too large");
    }
}

ii::core::Result<CanvasTileCache> CanvasTileCacheUpdater::execute(const CanvasTileCache &snapshot,
    std::uint64_t expectedRevision, std::uint64_t nextRevision, std::span<const CanvasRegion> invalidated,
    std::span<const FrameRenderTile> updates, std::size_t maximumTiles, std::uint64_t maximumPixels) const
{
    using ii::core::ErrorCode;
    if (snapshot.revision != expectedRevision)
        return ii::core::fail(ErrorCode::busy, "Stale canvas tile revision");
    if (nextRevision < expectedRevision || !maximumTiles || !maximumPixels
        || std::any_of(invalidated.begin(), invalidated.end(), [](auto r) { return !validRegion(r); }))
        return ii::core::fail(ErrorCode::invalid_argument, "Invalid canvas tile cache request");
    try {
        CanvasTileCache result = snapshot;
        std::uint64_t resident = 0;
        for (std::size_t i = 0; i < result.tiles.size(); ++i) {
            const auto &tile = result.tiles[i];
            if (!validTile(tile) || resident > std::numeric_limits<std::uint64_t>::max() - pixels(tile))
                return ii::core::fail(ErrorCode::invalid_argument, "Invalid cached canvas tile");
            resident += pixels(tile);
            for (std::size_t j = 0; j < i; ++j)
                if (overlap(tile.region, result.tiles[j].region))
                    return ii::core::fail(ErrorCode::invalid_argument, "Cached canvas tiles overlap");
        }
        const auto discard = [&](auto predicate) {
            auto it = result.tiles.begin();
            while (it != result.tiles.end()) {
                if (predicate(*it)) { resident -= pixels(*it); it = result.tiles.erase(it); }
                else ++it;
            }
        };
        for (auto region : invalidated)
            discard([&](const auto &tile) { return overlap(tile.region, region); });
        for (const auto &tile : updates) {
            const auto &source = tile.pixels;
            if (!validRegion(tile.region) || source.width <= 0 || source.height <= 0
                || source.width > int(ii::gui::maximum_image_dimension)
                || source.height > int(ii::gui::maximum_image_dimension))
                return ii::core::fail(ErrorCode::invalid_argument, "Invalid canvas tile dimensions");
            const auto count = std::uint64_t(source.width) * std::uint64_t(source.height);
            if (count != source.pixels.size() || count > maximumPixels)
                return ii::core::fail(ErrorCode::invalid_argument, "Invalid or excessive canvas tile pixels");
            std::vector<std::uint8_t> rgba(std::size_t(count) * 4);
            for (std::size_t i = 0; i < source.pixels.size(); ++i) {
                const auto argb = source.pixels[i];
                rgba[i * 4] = std::uint8_t(argb >> 16); rgba[i * 4 + 1] = std::uint8_t(argb >> 8);
                rgba[i * 4 + 2] = std::uint8_t(argb); rgba[i * 4 + 3] = std::uint8_t(argb >> 24);
            }
            auto image = ii::gui::rgba_image(std::uint32_t(source.width), std::uint32_t(source.height),
                std::move(rgba), ii::gui::ImageBudget{std::uint64_t(ii::gui::maximum_image_dimension)
                                                      * ii::gui::maximum_image_dimension});
            if (!image) return std::unexpected(image.error());
            discard([&](const auto &old) { return overlap(old.region, tile.region); });
            while (!result.tiles.empty() && (result.tiles.size() >= maximumTiles || resident > maximumPixels - count)) {
                resident -= pixels(result.tiles.front()); result.tiles.erase(result.tiles.begin());
            }
            result.tiles.push_back({tile.region, std::move(*image)}); resident += count;
        }
        while (!result.tiles.empty() && (result.tiles.size() > maximumTiles || resident > maximumPixels)) {
            resident -= pixels(result.tiles.front()); result.tiles.erase(result.tiles.begin());
        }
        result.revision = nextRevision;
        return result;
    } catch (const std::bad_alloc &) {
        return ii::core::fail(ErrorCode::failed, "Cannot allocate canvas tiles");
    } catch (const std::length_error &) {
        return ii::core::fail(ErrorCode::failed, "Canvas tile cache is too large");
    }
}

ii::core::Result<std::shared_ptr<const ii::gui::WidgetDrawing>> CanvasTilePresenter::execute(
    const CanvasTileCache &cache, const ii::gui::EditorCanvasQuery &query) const
{
    using ii::core::ErrorCode;
    const auto &view = query.transform;
    if (cache.revision != query.revision)
        return ii::core::fail(ErrorCode::busy, "Canvas tile drawing revision does not match the cache");
    if (!std::isfinite(query.surface.width) || !std::isfinite(query.surface.height)
        || query.surface.width < 0 || query.surface.height < 0
        || query.surface.width > ii::gui::maximum_image_dimension || query.surface.height > ii::gui::maximum_image_dimension
        || !std::isfinite(view.scale) || view.scale < .0001 || view.scale > 8192
        || !std::isfinite(view.offset_x) || std::abs(view.offset_x) > 1e15
        || !std::isfinite(view.offset_y) || std::abs(view.offset_y) > 1e15
        || !std::isfinite(view.rotation_radians))
        return ii::core::fail(ErrorCode::invalid_argument, "Invalid canvas tile viewport");
    if (view.rotation_radians != 0 || view.flip_x || view.flip_y)
        return ii::core::fail(ErrorCode::unsupported, "Rotated or reflected tiles require full-frame presentation");
    try {
        auto drawing = std::make_shared<ii::gui::WidgetDrawing>();
        for (std::size_t i = 0; i < cache.tiles.size(); ++i) {
            const auto &tile = cache.tiles[i];
            if (!validTile(tile)) return ii::core::fail(ErrorCode::invalid_argument, "Invalid cached canvas tile");
            for (std::size_t j = 0; j < i; ++j)
                if (overlap(tile.region, cache.tiles[j].region))
                    return ii::core::fail(ErrorCode::invalid_argument, "Cached canvas tiles overlap");
            const auto x = tile.region.origin.x * view.scale + view.offset_x;
            const auto y = tile.region.origin.y * view.scale + view.offset_y;
            const auto width = tile.region.extent.width * view.scale;
            const auto height = tile.region.extent.height * view.scale;
            if (x >= query.surface.width || y >= query.surface.height || x + width <= 0 || y + height <= 0) continue;
            ii::gui::ImageRun run;
            run.bounds = {float(x), float(y), float(width), float(height)};
            run.image = tile.image;
            drawing->images.push_back(std::move(run));
        }
        return std::shared_ptr<const ii::gui::WidgetDrawing>(std::move(drawing));
    } catch (const std::bad_alloc &) {
        return ii::core::fail(ErrorCode::failed, "Cannot allocate canvas tile drawing");
    } catch (const std::length_error &) {
        return ii::core::fail(ErrorCode::failed, "Canvas tile drawing is too large");
    }
}

} // namespace iiSharedCanvas
