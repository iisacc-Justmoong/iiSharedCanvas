#include <iiSharedCanvas.h>
#include <array>
#include <chrono>
#include <iostream>
#include <limits>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, const char *message) {
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
Document fixture() {
    Document d; d.extent = {9, 7}; d.timeline.frameCount = 3;
    d.artboards = {{"a", "A", {{-4, 3}, {9, 7}}, 0x90445566U},
                   {"b", "B", {{-4, 3}, {9, 7}}, 0xffffffffU}};
    auto pixels = makeRasterLayer(9, 7);
    auto mask = makeRasterLayer(9, 7);
    for (std::size_t i = 0; i < pixels.pixels.size(); ++i) {
        pixels.pixels[i] = 0x80000000U | ((i * 19 % 256) << 16) | ((i * 31 % 256) << 8) | (i * 7 % 256);
        mask.pixels[i] = 0xff000000U | ((i * 43 % 256) * 0x010101U);
    }
    d.assets.emplace_back(RasterAsset{"pixels", pixels});
    d.assets.emplace_back(RasterAsset{"mask", mask});
    d.assets.emplace_back(RasterAsset{"blue", makeRasterLayer(9, 7, 0xff0000ffU)});
    LayerProperties p{"base", "Base"}; p.artboardId = "a";
    LayerMask coverage; coverage.assetId = "mask"; coverage.featherRadius = 0.75; coverage.density = 0.8;
    p.appearance.rasterMask = coverage;
    d.layers.emplace_back(StaticBitmapLayer{p, StaticSource{"pixels"}});
    p = {"dynamic", "Dynamic"}; p.artboardId = "a"; p.opacity = 0.6; p.appearance.fillOpacity = 0.75;
    p.transform = {0.9, 0.2, -0.15, 0.85, 0.25, 0.5};
    p.frameRange = LayerFrameRange{1, 2};
    d.layers.emplace_back(DynamicBitmapLayer{p, KeyframedSource{{0, 2}}});
    d.frames = {{0, {{"dynamic", "pixels"}}}, {2, {{"dynamic", "blue"}}}};
    VectorPath path; path.commands = {MoveTo{{0.25, 0.5}}, LineTo{{8.5, 1.25}}, LineTo{{2.5, 6.5}}, ClosePath{}};
    path.fill = SolidPaint{0x9033cc88U}; path.stroke = StrokeStyle{SolidPaint{0x80ff0000U}, 0.75};
    d.assets.emplace_back(VectorAsset{"vector", {9, 7}, {path}});
    p = {"vector", "Vector"}; p.artboardId = "a";
    d.layers.emplace_back(StaticVectorLayer{p, StaticSource{"vector"}});
    p = {"other", "Other board"}; p.artboardId = "b";
    d.layers.emplace_back(StaticBitmapLayer{p, StaticSource{"blue"}});
    p = {"loose", "Loose artwork"}; p.transform.translationX = -4; p.transform.translationY = 3;
    d.layers.emplace_back(StaticBitmapLayer{p, StaticSource{"blue"}});
    return d;
}
void compareRegions(const Document &d, FrameIndex frame) {
    const auto original = encodeIisc(d); expect(original.ok(), "fixture must encode");
    const auto full = renderArtboard(d, frame, "a");
    expect(full.ok(), "full artboard oracle must render");
    if (!full.ok()) return;
    for (int y = 0; y < 7; ++y) for (int x = 0; x < 9; ++x) {
        const CanvasRegion region{{x - 4, y + 3}, {1, 1}};
        const auto point = renderArtboardRegion(d, frame, "a", region, {1, 1});
        expect(point.ok() && point.origin == region.origin && point.pixels.width == 1 && point.pixels.height == 1
            && point.pixels.pixels.size() == 1 && point.pixels.pixels.front() == full.pixels.pixels[y * 9 + x],
            "one-pixel crops must exactly match native full artboard pixels, including masks/vector coverage and world-coordinate blending");
    }
    const auto crop = renderArtboardRegion(d, frame, "a", {{-3, 4}, {3, 2}}, {3, 2});
    expect(crop.ok() && crop.pixels.width == 3 && crop.pixels.height == 2, "native crop must retain its requested extent");
    if (crop.ok()) for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x)
        expect(crop.pixels.pixels[y * 3 + x] == full.pixels.pixels[(y + 1) * 9 + x + 1], "cropped blocks must retain native pixel positions");
    const auto resized = renderArtboardRegion(d, frame, "a", {{-3, 4}, {3, 2}}, {6, 4}, RasterSampling::Smooth);
    expect(resized.ok() && resized.pixels.width == 6 && resized.pixels.height == 4, "cropped regions must support explicit smooth output bounds");
    expect(encodeIisc(d).bytes == original.bytes, "cropped rendering must preserve every persisted source value");
}
}
int main() {
    auto d = fixture(); expect(validate(d).ok(), "mixed artboard fixture must validate");
    for (const auto mode : std::array{RasterBlendMode::SourceOver, RasterBlendMode::Multiply, RasterBlendMode::Dissolve}) {
        layerProperties(d.layers[1]).blendMode = mode;
        for (FrameIndex frame = 0; frame < 3; ++frame) compareRegions(d, frame);
    }
    const auto isolated = renderArtboardRegion(d, 0, "a", {{-4, 3}, {1, 1}}, {1, 1});
    const auto overview = renderFrameRegion(d, 0, {{-4, 3}, {1, 1}}, {1, 1});
    expect(isolated.ok() && overview.ok() && isolated.pixels.pixels != overview.pixels.pixels,
        "single-artboard crops must exclude overlapping boards and loose artwork");
    d.artboards[0].visible = false;
    const auto hidden = renderArtboardRegion(d, 0, "a", {{-4, 3}, {1, 1}}, {1, 1});
    expect(hidden.ok() && hidden.pixels.pixels == std::vector<std::uint32_t>{0}, "hidden boards must suppress both background and owned artwork");
    d.artboards[0].visible = true;
    expect(renderArtboardRegion(d, 0, "missing", {{-4, 3}, {1, 1}}, {1, 1}).status == FrameRenderStatus::ArtboardNotFound, "missing board must report its typed error");
    expect(renderArtboardRegion(d, 3, "a", {{-4, 3}, {1, 1}}, {1, 1}).status == FrameRenderStatus::FrameOutOfRange, "invalid frame must report its typed error");
    for (const auto region : std::array<CanvasRegion, 4>{{{{-5, 3}, {1, 1}}, {{4, 9}, {2, 1}}, {{-4, 3}, {0, 1}}, {{std::numeric_limits<std::int32_t>::max(), 3}, {2, 1}}}})
        expect(renderArtboardRegion(d, 0, "a", region, {1, 1}).status == FrameRenderStatus::InvalidRegion, "invalid or out-of-board world region must be rejected without allocation");
    expect(renderArtboardRegion(d, 0, "a", {{-4, 3}, {1, 1}}, {0, 1}).status == FrameRenderStatus::InvalidRegion, "invalid output size must be rejected");
    expect(renderArtboardRegion(d, 0, "a", {{-4, 3}, {1, 1}}, {1, 1}, static_cast<RasterSampling>(99)).status == FrameRenderStatus::InvalidRegion, "unknown sampling policy must be rejected");
    d.artboards[1].id = "a";
    expect(renderArtboardRegion(d, 0, "a", {{-4, 3}, {1, 1}}, {1, 1}).status == FrameRenderStatus::InvalidDocument, "invalid document must retain its typed failure");
    Document large; large.extent = {32768, 32768}; large.artboards = {{"large", "Large", {{0, 0}, large.extent}, 0xff102030U}};
    const auto start = std::chrono::steady_clock::now();
    const auto point = renderArtboardRegion(large, 0, "large", {{32767, 32767}, {1, 1}}, {1, 1});
    expect(point.ok() && point.pixels.pixels == std::vector<std::uint32_t>{0xff102030U}, "large canvas point sampling must allocate only its requested pixel, not the full canvas");
    std::cout << "large one-pixel crop microseconds " << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() << '\n';
    return failures ? 1 : 0;
}
