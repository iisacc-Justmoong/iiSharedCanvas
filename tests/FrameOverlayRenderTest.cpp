#include <iiSharedCanvasNative.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace iiSharedCanvas;
static_assert(BitmapPreviewTileSide == 64);
namespace {
int failures = 0;
void expect(bool value, std::string_view message) {
    if (!value) { ++failures; std::cerr << message << '\n'; }
}
Layer bitmap(std::string id, std::string source) {
    return StaticBitmapLayer{{std::move(id), "Raster"}, StaticSource{std::move(source)}};
}
bool same(const RasterLayer &a, const RasterLayer &b) {
    return a.width == b.width && a.height == b.height && a.pixels == b.pixels;
}
std::uint32_t &at(RasterLayer &r, int x, int y) { return r.pixels[std::size_t(y) * r.width + x]; }
std::uint32_t at(const RasterLayer &r, int x, int y) { return r.pixels[std::size_t(y) * r.width + x]; }
Document basic() {
    Document d; d.extent = {8, 8};
    d.assets.emplace_back(RasterAsset{"background", makeRasterLayer(8, 8, 0xff2040e0)});
    d.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(8, 8, 0xffd02010)});
    d.layers.push_back(bitmap("background-layer", "background"));
    d.layers.push_back(bitmap("paint-layer", "paint"));
    return d;
}
void basicHistory() {
    auto d = basic(); auto expected = d; const auto before = encodeIisc(d).bytes;
    const auto revision = d.authorship.revision(); DocumentHistory h(d);
    const std::array<std::uint32_t, 2> pixels{0x00000000, 0xff22cc44};
    const std::array patches{BitmapPatch{{{3, 3}, 2, 1}, pixels}};
    expect(h.beginStagedPixelPreview("paint").ok() && h.replacePixelPreview(patches).changed,
           "basic: stage transparent and opaque final pixels");
    auto tiles = h.stagedPixelPreviewTiles();
    const std::array overlays{FrameBitmapOverlay{"paint", tiles}};
    auto &reference = findRasterAsset(expected, "paint")->pixels;
    at(reference, 3, 3) = pixels[0]; at(reference, 4, 3) = pixels[1];
    const auto view = renderFrameRegion(d, 0, {{0, 0}, {8, 8}}, {8, 8}, RasterSampling::Nearest, overlays);
    const auto committed = renderFrame(expected, 0);
    expect(view.ok() && committed.ok() && same(view.pixels, committed.pixels),
           "basic: replacement pixels must match independent edited document");
    if (view.ok()) expect(at(view.pixels, 3, 3) == 0xff2040e0 && at(view.pixels, 4, 3) == 0xff22cc44,
                          "basic: erasure reveals backdrop and opaque replacement is visible");
    expect(encodeIisc(d).bytes == before && d.authorship.revision() == revision,
           "basic: rendering must not modify owner or revision");
    const std::array<std::uint32_t, 1> moved{0xffffbb00U};
    const std::array replacement{BitmapPatch{{{6, 6}, 1, 1}, moved}};
    expect(h.replacePixelPreview(replacement).changed, "basic: replace the entire previous preview");
    tiles = h.stagedPixelPreviewTiles();
    const std::array nextOverlay{FrameBitmapOverlay{"paint", tiles}};
    expected = d; at(findRasterAsset(expected, "paint")->pixels, 6, 6) = moved[0];
    const auto movedView = renderFrame(d, 0, nextOverlay), movedReference = renderFrame(expected, 0);
    expect(movedView.ok() && movedReference.ok() && same(movedView.pixels, movedReference.pixels) &&
           at(movedView.pixels, 3, 3) == 0xffd02010U,
           "basic: omitted previous pixels return to the original source");
    expect(h.cancelPixelPreview().changed && encodeIisc(d).bytes == before,
           "basic: cancellation preserves owner");
    const auto afterCancel = renderFrame(d, 0);
    expect(afterCancel.ok() && at(afterCancel.pixels, 3, 3) == 0xffd02010,
           "basic: cancellation restores unmodified display");
}
Document complex() {
    Document d; d.extent = {96, 80};
    d.assets.emplace_back(RasterAsset{"background", makeRasterLayer(96, 80, 0xff38506a)});
    auto paint = makeRasterLayer(80, 70); auto mask = makeRasterLayer(80, 70);
    for (int y = 0; y < 70; ++y) for (int x = 0; x < 80; ++x) {
        at(paint, x, y) = 0x80000000U | (std::uint32_t((x * 13) & 255) << 16)
            | (std::uint32_t((y * 11) & 255) << 8) | std::uint32_t((x + y) & 255);
        at(mask, x, y) = 0xff000000U | std::uint32_t((x * 3 + y * 5) & 255) * 0x010101U;
    }
    d.assets.emplace_back(RasterAsset{"paint", std::move(paint)});
    d.assets.emplace_back(RasterAsset{"mask", std::move(mask)});
    d.layers.push_back(bitmap("background-layer", "background"));
    auto top = bitmap("paint-one", "paint"); auto &p = layerProperties(top);
    p.transform = {0.93, 0.19, -0.17, 0.98, 11, 2};
    LayerMask maskSettings; maskSettings.assetId = "mask"; maskSettings.featherRadius = 1.25;
    maskSettings.channel = MaskChannel::Luminance; maskSettings.outsideCoverage = 0.3;
    p.appearance.rasterMask = maskSettings;
    p.appearance.filterMask = maskSettings;
    p.appearance.maskStack = {{"stack-mask", "Stack", maskSettings, MaskCombineMode::Intersect}};
    LayerFilter filter; filter.operation = RasterEffect{RasterEffectKind::Invert};
    p.appearance.filters = {filter};
    d.layers.push_back(std::move(top));
    auto shared = bitmap("paint-two", "paint");
    layerProperties(shared).transform = {1, 0, 0, 1, -7, 9};
    layerProperties(shared).opacity = 0.6;
    d.layers.push_back(std::move(shared));
    return d;
}
std::vector<std::uint32_t> tileFrom(const RasterLayer &source) {
    std::vector<std::uint32_t> tile(64 * 64);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
        tile[std::size_t(y) * 64 + x] = source.pixels[std::size_t(y) * source.width + x];
    return tile;
}
void complexOverlay() {
    auto d = complex(); auto expected = d; const auto before = encodeIisc(d).bytes;
    auto paint = tileFrom(findRasterAsset(d, "paint")->pixels);
    auto mask = tileFrom(findRasterAsset(d, "mask")->pixels);
    auto &referencePaint = findRasterAsset(expected, "paint")->pixels;
    auto &referenceMask = findRasterAsset(expected, "mask")->pixels;
    for (int y = 12; y < 27; ++y) for (int x = 14; x < 31; ++x) {
        paint[std::size_t(y) * 64 + x] = (x % 3) ? 0x00000000U : 0xff00dc80U;
        mask[std::size_t(y) * 64 + x] = 0xffeeeeeeU;
        at(referencePaint, x, y) = paint[std::size_t(y) * 64 + x];
        at(referenceMask, x, y) = mask[std::size_t(y) * 64 + x];
    }
    const std::array paintTiles{BitmapPreviewTile{{{0, 0}, 64, 64}, paint}};
    const std::array maskTiles{BitmapPreviewTile{{{0, 0}, 64, 64}, mask}};
    const std::array overlays{FrameBitmapOverlay{"paint", paintTiles}, FrameBitmapOverlay{"mask", maskTiles}};
    for (const auto sampling : {RasterSampling::Nearest, RasterSampling::Smooth}) {
        for (const CanvasExtent output : {CanvasExtent{96, 80}, CanvasExtent{48, 40}}) {
            const FrameRenderTileRequest request{{{0, 0}, {96, 80}}, output, sampling};
            const auto actual = renderFrameRegion(d, 0, request.region, output, sampling, overlays);
            const auto reference = renderFrameRegion(expected, 0, request.region, output, sampling);
            const auto original = renderFrameRegion(d, 0, request.region, output, sampling);
            expect(actual.ok() && reference.ok() && same(actual.pixels, reference.pixels),
                   "complex: transformed shared raster, mask, filter and sampling match edited document");
            expect(reference.ok() && original.ok() && reference.pixels.pixels != original.pixels.pixels,
                   "complex: changed pixels and mask affect the compared region");
            const auto layers = renderFrameLayers(d, 0, {request}, overlays);
            const auto expectedLayers = renderFrameLayers(expected, 0, {request});
            const auto only = renderFrameLayerTiles(d, 0, 1, {request}, overlays);
            expect(layers.ok() && expectedLayers.ok() && only.ok() && layers.layers.size() == 3 &&
                   expectedLayers.layers.size() == 3 && !only.tiles.empty() &&
                   same(only.tiles[0].pixels, expectedLayers.layers[1].tiles[0].pixels) &&
                   same(layers.layers[2].tiles[0].pixels, expectedLayers.layers[2].tiles[0].pixels),
                   "complex: layer and batch entry points apply both overlays and shared asset references");
            const auto tiled = renderFrameTiles(d, 0, {request}, overlays);
            expect(tiled.ok() && actual.ok() && !tiled.tiles.empty() && same(tiled.tiles[0].pixels, actual.pixels),
                   "complex: composed tile and direct region agree");
        }
    }
    const auto full = renderFrame(d, 0, overlays); const auto reference = renderFrame(expected, 0);
    expect(full.ok() && reference.ok() && same(full.pixels, reference.pixels),
           "complex: full-frame entry point applies overlays");
    expect(encodeIisc(d).bytes == before, "complex: original multi-asset document remains unchanged");
}
void chunked() {
    Document d; d.extent = {64, 64}; d.canvasMode = CanvasMode::Infinite;
    d.infiniteCanvas = {{-32, -32}, 32};
    d.assets.emplace_back(ChunkedRasterAsset{"paint", {{-1, -1, makeRasterLayer(32, 32, 0xffdd2211)},
                                                        {1, 0, makeRasterLayer(32, 32, 0xff1155dd)}}});
    auto layer = bitmap("paint-layer", "paint");
    layerProperties(layer).transform = {0.9, 0.2, -0.2, 0.9, 5, -3};
    d.layers.push_back(std::move(layer)); auto expected = d;
    const auto before = encodeIisc(d).bytes; DocumentHistory history(d), referenceHistory(expected);
    std::array<std::uint32_t, 36> erased{}, green{}, yellow{};
    green.fill(0x8011ee44); yellow.fill(0x7feedd33);
    const std::array patches{BitmapPatch{{{-28, -28}, 6, 6}, erased},
                             BitmapPatch{{{-65, 1}, 6, 6}, green},
                             BitmapPatch{{{66, 1}, 6, 6}, yellow}};
    expect(history.beginStagedPixelPreview("paint", PixelPreviewCanvasMode::Grow).ok() &&
           history.replacePixelPreview(patches).changed &&
           referenceHistory.beginStagedPixelPreview("paint", PixelPreviewCanvasMode::Grow).ok() &&
           referenceHistory.replacePixelPreview(patches).changed && referenceHistory.commitPixelPreview().changed,
           "chunked: stage and commit the same changed existing and new chunks");
    auto tiles = history.stagedPixelPreviewTiles(); const std::array overlays{FrameBitmapOverlay{"paint", tiles}};
    for (auto sampling : {RasterSampling::Nearest, RasterSampling::Smooth}) {
        const auto actual = renderFrameRegion(d, 0, {{-96, -64}, {192, 128}}, {96, 64}, sampling, overlays);
        const auto reference = renderFrameRegion(expected, 0, {{-96, -64}, {192, 128}}, {96, 64}, sampling);
        const auto original = renderFrameRegion(d, 0, {{-96, -64}, {192, 128}}, {96, 64}, sampling);
        expect(actual.ok() && reference.ok() && same(actual.pixels, reference.pixels),
               "chunked: negative and new chunks match committed output under transformation");
        expect(reference.ok() && original.ok() && reference.pixels.pixels != original.pixels.pixels,
               "chunked: compared region includes the edited chunks");
    }
    expect(encodeIisc(d).bytes == before && history.cancelPixelPreview().changed,
           "chunked: rendering does not alter pixels or canvas geometry");
}
void artboard() {
    auto d = basic();
    d.artboards = {{"board", "Board", {{0, 0}, {8, 8}}, 0x00000000U}};
    for (auto &layer : d.layers) layerProperties(layer).artboardId = "board";
    auto expected = d; DocumentHistory history(d);
    const std::array<std::uint32_t, 1> color{0xff00ff88U};
    const std::array patches{BitmapPatch{{{3, 3}, 1, 1}, color}};
    expect(history.beginStagedPixelPreview("paint").ok() && history.replacePixelPreview(patches).changed,
           "artboard: stage source pixel");
    at(findRasterAsset(expected, "paint")->pixels, 3, 3) = color[0];
    auto tiles = history.stagedPixelPreviewTiles(); const std::array overlays{FrameBitmapOverlay{"paint", tiles}};
    const auto actual = renderArtboardRegion(d, 0, "board", {{0, 0}, {8, 8}}, {16, 16},
                                              RasterSampling::Smooth, overlays);
    const auto reference = renderArtboardRegion(expected, 0, "board", {{0, 0}, {8, 8}}, {16, 16},
                                                 RasterSampling::Smooth);
    expect(actual.ok() && reference.ok() && same(actual.pixels, reference.pixels),
           "artboard: isolated rendering applies staged pixels before smooth sampling");
    expect(history.cancelPixelPreview().changed, "artboard: cancel staged pixel");
}
void animatedAsset() {
    auto d = basic(); d.timeline.frameCount = 2;
    d.assets.emplace_back(RasterAsset{"other", makeRasterLayer(8, 8, 0xff2244ddU)});
    d.layers[1] = DynamicBitmapLayer{{"paint-layer", "Paint"}, KeyframedSource{{0, 1}}};
    d.frames = {{0, {{"paint-layer", "paint"}}}, {1, {{"paint-layer", "other"}}}};
    DocumentHistory history(d); const std::array<std::uint32_t, 1> value{0xff22ee44};
    const std::array patch{BitmapPatch{{{3, 3}, 1, 1}, value}};
    expect(history.beginStagedPixelPreview("paint").ok() && history.replacePixelPreview(patch).changed,
           "animation: stage a frame-zero asset");
    auto tiles = history.stagedPixelPreviewTiles(); const std::array overlays{FrameBitmapOverlay{"paint", tiles}};
    const auto first = renderFrame(d, 0, overlays), second = renderFrame(d, 1, overlays);
    expect(first.ok() && second.ok() && at(first.pixels, 3, 3) == value[0] &&
           at(second.pixels, 3, 3) == 0xff2244ddU,
           "animation: overlay follows selected asset, not layer or all frames");
    expect(history.cancelPixelPreview().changed, "animation: cancel staged pixel");
}
void extremeCoordinates() {
    Document d; d.extent = {64, 64}; d.canvasMode = CanvasMode::Infinite;
    d.infiniteCanvas = {{0, 0}, 32};
    d.assets.emplace_back(ChunkedRasterAsset{"paint", {}});
    d.layers.push_back(bitmap("layer", "paint"));
    constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    std::vector<std::uint32_t> negative(64 * 64), positive(64 * 64);
    negative[0] = 0xff11cc44U; positive[std::size_t(62) * 64 + 62] = 0xffdd4422U;
    const std::array tiles{BitmapPreviewTile{{{minimum, minimum}, 64, 64}, negative},
                           BitmapPreviewTile{{{maximum - 63, maximum - 63}, 64, 64}, positive}};
    const std::array overlays{FrameBitmapOverlay{"paint", tiles}};
    const auto low = renderFrameRegion(d, 0, {{minimum, minimum}, {1, 1}}, {1, 1},
                                       RasterSampling::Nearest, overlays);
    const auto high = renderFrameRegion(d, 0, {{maximum - 1, maximum - 1}, {1, 1}}, {1, 1},
                                        RasterSampling::Nearest, overlays);
    expect(low.ok() && high.ok() && low.pixels.pixels[0] == negative[0] &&
           high.pixels.pixels[0] == positive[std::size_t(62) * 64 + 62],
           "coordinates: aligned tiles at both 32-bit extremes render without overflow");
}
void invalid() {
    auto d = basic(); const FrameRenderTileRequest request{{{0, 0}, {8, 8}}, {8, 8}};
    std::vector<std::uint32_t> pixels(64 * 64, 0xff00ff00U);
    const std::array good{BitmapPreviewTile{{{0, 0}, 64, 64}, pixels}};
    const std::array shortTile{BitmapPreviewTile{{{0, 0}, 64, 64}, std::span(pixels).first(1)}};
    const std::array misaligned{BitmapPreviewTile{{{1, 0}, 64, 64}, pixels}};
    const std::array duplicates{good[0], good[0]};
    const auto reject = [&](std::span<const FrameBitmapOverlay> overlays, std::string_view name) {
        expect(renderFrameRegion(d, 0, request.region, request.outputExtent, RasterSampling::Nearest, overlays).status ==
                   FrameRenderStatus::InvalidOverlay &&
               renderFrameLayerTiles(d, 0, 1, {request}, overlays).status == FrameRenderStatus::InvalidOverlay,
               name);
    };
    const std::array badSize{FrameBitmapOverlay{"paint", shortTile}}; reject(badSize, "invalid: truncated tile");
    const std::array badAlignment{FrameBitmapOverlay{"paint", misaligned}}; reject(badAlignment, "invalid: tile alignment");
    const std::array badDuplicate{FrameBitmapOverlay{"paint", duplicates}}; reject(badDuplicate, "invalid: duplicate tiles");
    const std::array badAsset{FrameBitmapOverlay{"missing", good}}; reject(badAsset, "invalid: unknown asset");
    d.assets.emplace_back(VectorAsset{"vector", {8, 8}, {}});
    const std::array badType{FrameBitmapOverlay{"vector", good}}; reject(badType, "invalid: non-raster asset");
    const std::array badRepeat{FrameBitmapOverlay{"paint", good}, FrameBitmapOverlay{"paint", good}};
    reject(badRepeat, "invalid: duplicate overlay asset");
}
}
int main() {
    basicHistory(); complexOverlay(); chunked(); artboard(); animatedAsset(); extremeCoordinates(); invalid();
    return failures ? 1 : 0;
}
