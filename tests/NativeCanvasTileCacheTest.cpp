#include <Gui/CanvasTileCache.h>

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
}

int main()
{
    try {
        using namespace iiSharedCanvas;
        CanvasTileCacheUpdater update;
        CanvasTilePresenter present;
        CanvasTileCache empty; empty.revision = 1;
        std::array tiles{
            FrameRenderTile{{{0, 0}, {2, 2}}, makeRasterLayer(2, 2, 0xffff0000)},
            FrameRenderTile{{{2, 0}, {2, 2}}, makeRasterLayer(2, 2, 0xff0000ff)}};
        auto first = update.execute(empty, 1, 1, {}, tiles, 2, 8);
        require(first && first->tiles.size() == 2 && first->revision == 1,
                "two tiles must be cached");
        auto retained = first->tiles[1].image;
        const std::array crossing{CanvasRegion{{1, 0}, {2, 2}}};
        auto plan = CanvasDirtyTilePlanner{}.execute(*first, 1, crossing, RasterSampling::Nearest, 2);
        require(plan && plan->size() == 2 && (*plan)[0].region == tiles[0].region
                    && (*plan)[1].region == tiles[1].region,
                "dirty coverage crossing a tile boundary must schedule both complete tiles");
        require(!CanvasDirtyTilePlanner{}.execute(*first, 0, crossing, RasterSampling::Nearest, 2),
                "stale dirty-tile planning must fail");
        require(!CanvasDirtyTilePlanner{}.execute(*first, 1, crossing, RasterSampling::Nearest, 1),
                "dirty-tile planning must enforce its request budget without a partial result");
        std::array dirty{CanvasRegion{{0, 0}, {2, 2}}};
        tiles[0].pixels.pixels[0] = 0xff00ff00;
        auto next = update.execute(*first, 1, 2, dirty, std::span(tiles).first(1), 2, 8);
        require(next && next->revision == 2 && next->tiles.size() == 2,
                "updated tile and untouched tile must remain cached");
        require(next->tiles[0].image == retained && next->tiles[1].image != first->tiles[0].image,
                "unchanged tile image must be reused across content revisions");
        require(first->revision == 1 && first->tiles[0].image->rgba()[0] == 255,
                "an update must not mutate the prior snapshot");
        require(!update.execute(*next, 1, 3, {}, std::span(tiles).first(1), 2, 8),
                "stale asynchronous revisions must be rejected");
        auto limited = update.execute(*next, 2, 2, {}, {}, 1, 4);
        require(limited && limited->tiles.size() == 1 && limited->tiles[0].image == next->tiles.back().image,
                "budget reduction must evict the oldest tile");
        auto invalid = tiles[0]; invalid.pixels.pixels.pop_back();
        require(!update.execute(*next, 2, 3, {}, std::span(&invalid, 1), 2, 8),
                "invalid pixels must not create a partial cache");
        ii::gui::EditorCanvasQuery query; query.surface = {8, 4}; query.revision = 2;
        query.transform = {.scale = 2, .offset_x = 1, .offset_y = 2};
        auto drawing = present.execute(*next, query);
        require(drawing && (*drawing)->images.size() == 2, "each cached tile must be a separate iiGUI image");
        require((*drawing)->images[0].image == retained && (*drawing)->images[0].bounds.x == 5
                    && (*drawing)->images[0].bounds.y == 2 && (*drawing)->images[0].bounds.width == 4,
                "tile display must preserve identity and map document bounds through the viewport");
        query.transform.rotation_radians = .5;
        require(!present.execute(*next, query), "rotated tile view must use the full-frame fallback");
        query.transform = {}; query.revision = 1;
        require(!present.execute(*next, query), "a stale cache must not be presented for a new revision");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
