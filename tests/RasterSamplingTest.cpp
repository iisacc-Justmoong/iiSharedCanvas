#include <iiSharedCanvas.h>

#include <filesystem>
#include <iostream>

namespace {
int failures = 0;
void expect(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
iiSharedCanvas::Document bitmap(RasterLayer pixels) {
    using namespace iiSharedCanvas;
    Document d;
    d.extent = {pixels.width, pixels.height};
    d.assets.emplace_back(RasterAsset{"source", std::move(pixels)});
    d.layers.emplace_back(StaticBitmapLayer{{"image", "Image"}, StaticSource{"source"}});
    return d;
}
}

int main() {
    using namespace iiSharedCanvas;
    auto checker = makeRasterLayer(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            checker.pixels[y * 64 + x] = (x + y) % 2 ? 0xffffffffU : 0xff000000U;
    auto document = bitmap(checker);
    const auto encoded = encodeIisc(document);
    expect(encoded.ok(), "full-resolution raster snapshot must encode");
    const auto decoded = decodeIisc(encoded.bytes);
    expect(decoded.ok(), "full-resolution raster snapshot must decode");
    expect(std::get<RasterAsset>(decoded.document.assets.front()).pixels.pixels == checker.pixels,
           ".iisc must preserve every source pixel before resampling");
    const auto path = std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR) / "sampling.iisc";
    std::filesystem::remove(path);
    DocumentFile file;
    expect(file.create(path.string(), decoded.document).ok(), "working .iisc must create");
    file.close();
    expect(file.open(path.string()).ok(), "working .iisc must reopen");
    expect(std::get<RasterAsset>(file.document()->assets.front()).pixels.pixels == checker.pixels,
           "working-file emission and reload must remain lossless");

    const auto reduced = renderFrameRegion(*file.document(), 0, {{0, 0}, {64, 64}},
                                           {16, 16}, RasterSampling::Smooth);
    expect(reduced.ok(), "smooth minification must render");
    for (const auto pixel : reduced.pixels.pixels)
        expect(pixel == 0xff808080U, "minification must average all covered pixels, not select checker squares");
    const auto exact = renderFrameRegion(document, 0, {{0, 0}, {64, 64}}, {64, 64}, RasterSampling::Smooth);
    expect(exact.ok() && exact.pixels.pixels == checker.pixels, "native scale must preserve exact source pixels");
    const auto nearest = renderFrameRegion(document, 0, {{0, 0}, {64, 64}}, {16, 16});
    expect(nearest.ok() && nearest.pixels.pixels.front() == 0xff000000U,
           "explicit legacy nearest sampling must remain available");
    const auto tiles = renderFrameTiles(document, 0, {
        {{{0, 0}, {32, 64}}, {8, 16}, RasterSampling::Smooth},
        {{{32, 0}, {32, 64}}, {8, 16}, RasterSampling::Smooth}});
    expect(tiles.ok(), "independent tiles must render");
    for (const auto &tile : tiles.tiles)
        for (auto pixel : tile.pixels.pixels)
            expect(pixel == 0xff808080U, "tiled emission must match the full-region average");

    auto alpha = makeRasterLayer(2, 1);
    alpha.pixels = {0xffff0000U, 0x000000ffU};
    auto transparent = bitmap(alpha);
    const auto mixed = renderFrameRegion(transparent, 0, {{0, 0}, {2, 1}}, {1, 1}, RasterSampling::Smooth);
    expect(mixed.ok() && mixed.pixels.pixels.front() == 0x80ff0000U,
           "transparent colors must not contaminate premultiplied filtering");
    auto gradient = makeRasterLayer(2, 1);
    gradient.pixels = {0xff000000U, 0xffffffffU};
    auto enlarged = bitmap(gradient);
    const auto up = renderFrameRegion(enlarged, 0, {{0, 0}, {2, 1}}, {4, 1}, RasterSampling::Smooth);
    expect(up.ok() && up.pixels.pixels[1] == 0xff404040U && up.pixels.pixels[2] == 0xffbfbfbfU,
           "magnification must interpolate at pixel centers");

    auto transformed = document;
    transformed.extent = {32, 32};
    layerProperties(transformed.layers.front()).transform.m11 = 0.5;
    layerProperties(transformed.layers.front()).transform.m22 = 0.5;
    const auto scaled = renderFrameRegion(transformed, 0, {{0, 0}, {32, 32}}, {32, 32}, RasterSampling::Smooth);
    expect(scaled.ok() && scaled.pixels.pixels.front() == 0xff808080U,
           "layer transforms and viewport reduction must use the same quality policy");

    auto video = document;
    video.assets.clear();
    video.assets.emplace_back(VideoAsset{"video", {24, 1}, {checker}});
    video.layers.clear();
    video.layers.emplace_back(VideoLayer{{"clip", "Clip"}, StaticSource{"video"}});
    const auto videoFrame = renderFrameRegion(video, 0, {{0, 0}, {64, 64}}, {16, 16}, RasterSampling::Smooth);
    expect(videoFrame.ok() && videoFrame.pixels.pixels.front() == 0xff808080U,
           "decoded video frames must receive the same antialiasing");
    expect(!renderFrameTiles(document, 0, {{{{0, 0}, {64, 64}}, {16, 16}, static_cast<RasterSampling>(99)}}).ok(),
           "unknown sampling policies must fail closed");
    file.close();
    return failures ? 1 : 0;
}
