#include <iiSharedCanvas.h>
#include <Render/Compositor.h>

#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
thread_local bool measuring = false;
thread_local std::size_t requested = 0;
}
void *operator new(std::size_t size) {
    if (measuring) requested += size;
    if (auto *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

using namespace iiSharedCanvas;
namespace {
Document fixture() {
    Document document;
    document.extent = {512, 512};
    auto lower = makeRasterLayer(512, 512);
    auto upper = makeRasterLayer(512, 512);
    for (std::size_t i = 0; i < lower.pixels.size(); ++i) {
        lower.pixels[i] = (std::uint32_t(i % 256) << 24) | (std::uint32_t(i * 2654435761U) & 0xffffffU);
        upper.pixels[i] = (std::uint32_t((i * 3) % 256) << 24) | (std::uint32_t(i * 2246822519U) & 0xffffffU);
    }
    document.assets.emplace_back(RasterAsset{"lower", std::move(lower)});
    document.assets.emplace_back(RasterAsset{"upper", std::move(upper)});
    document.layers.emplace_back(StaticBitmapLayer{{"lower-layer", "Lower"}, StaticSource{"lower"}});
    document.layers.emplace_back(StaticBitmapLayer{{"upper-layer", "Upper"}, StaticSource{"upper"}});
    return document;
}
void equal(const FrameTileRenderResult &actual, const FrameTileRenderResult &expected) {
    if (!actual.ok() || !expected.ok() || actual.tiles.size() != expected.tiles.size())
        throw std::runtime_error("frame composition failed");
    for (std::size_t i = 0; i < actual.tiles.size(); ++i)
        if (actual.tiles[i].pixels.pixels != expected.tiles[i].pixels.pixels)
            throw std::runtime_error("retained base changed composited pixels");
}
void compare(const Document &document, const std::vector<FrameRenderTileRequest> &requests) {
    equal(renderFrameTiles(document, 0, requests),
          composeFrameLayers(renderFrameLayers(document, 0, requests)));
}
}

int main() try {
    for (std::uint32_t alpha = 0; alpha < 256; ++alpha)
        for (std::uint32_t channel = 0; channel < 256; ++channel) {
            const auto pixel = (alpha << 24) | (channel << 16) |
                               ((255 - channel) << 8) | ((channel * 37) & 255);
            if (compositeRasterPixel(pixel, 0, RasterBlendMode::SourceOver) !=
                (alpha ? pixel : 0))
                throw std::runtime_error("normal layer over transparent differs from its source pixel");
        }
    auto document = fixture();
    const std::vector<FrameRenderTileRequest> full{{{{0, 0}, {512, 512}}, {512, 512}}};
    auto single = fixture();
    single.layers.pop_back();
    const auto &source = std::get<RasterAsset>(single.assets.front()).pixels;
    requested = 0; measuring = true;
    const auto layer = renderFrameLayers(single, 0, full);
    measuring = false; const auto layerBytes = requested;
    if (!layer.ok() || layer.layers.size() != 1 || layer.layers.front().tiles.size() != 1)
        throw std::runtime_error("single layer rendering failed");
    const auto &pixels = layer.layers.front().tiles.front().pixels.pixels;
    if (pixels.size() != source.pixels.size())
        throw std::runtime_error("single layer rendering changed the output extent");
    for (std::size_t i = 0; i < pixels.size(); ++i)
        if (pixels[i] != compositeRasterPixel(source.pixels[i], 0, RasterBlendMode::SourceOver))
            throw std::runtime_error("single layer rendering changed pixel composition");
    std::cout << "SINGLE_LAYER requested=" << layerBytes << '\n';
    if (layerBytes > 512 * 512 * 4 * 3)
        throw std::runtime_error("single layer allocated avoidable intermediate tiles");
    requested = 0; measuring = true;
    auto baseline = composeFrameLayers(renderFrameLayers(document, 0, full));
    measuring = false; const auto baselineBytes = requested;
    requested = 0; measuring = true;
    auto actual = renderFrameTiles(document, 0, full);
    measuring = false; const auto actualBytes = requested;
    equal(actual, baseline);
    std::cout << "FRAME_COMPOSITION baseline=" << baselineBytes << " actual=" << actualBytes << '\n';
    if (actualBytes + 512 * 512 * 3 > baselineBytes)
        throw std::runtime_error("single base layer allocated an avoidable full output tile");
    requested = 0; measuring = true;
    auto region = renderFrameRegion(document, 0, full.front().region, full.front().outputExtent);
    measuring = false; const auto regionBytes = requested;
    if (!region.ok() || region.pixels.pixels != actual.tiles.front().pixels.pixels)
        throw std::runtime_error("region output differs from composed tile");
    std::cout << "FRAME_REGION tile=" << actualBytes << " region=" << regionBytes << '\n';
    if (regionBytes > actualBytes + 128 * 1024)
        throw std::runtime_error("region output copied an already owned tile");

    const std::vector<FrameRenderTileRequest> regions{
        {{{0, 0}, {256, 256}}, {256, 256}, RasterSampling::Smooth},
        {{{256, 256}, {256, 256}}, {128, 128}, RasterSampling::Smooth}};
    compare(document, regions);
    layerProperties(document.layers[0]).opacity = .6;
    compare(document, regions);
    layerProperties(document.layers[0]).opacity = 1;
    layerProperties(document.layers[0]).visible = false;
    compare(document, regions);
    layerProperties(document.layers[0]).visible = true;
    layerProperties(document.layers[1]).appearance.clippedToBelow = true;
    compare(document, regions);
    layerProperties(document.layers[1]).appearance.clippedToBelow = false;
    layerProperties(document.layers[1]).blendMode = RasterBlendMode::Multiply;
    compare(document, regions);
    VectorPath background;
    background.commands = {MoveTo{{0, 0}}, LineTo{{512, 0}}, LineTo{{512, 512}},
                           LineTo{{0, 512}}, ClosePath{}};
    background.fill = SolidPaint{0xffffffffU};
    document.assets.emplace_back(VectorAsset{"background", document.extent, {background}});
    document.layers[0] = StaticVectorLayer{{"background-layer", "Background"}, StaticSource{"background"}};
    compare(document, regions);
    return 0;
} catch (const std::exception &error) {
    measuring = false;
    std::cerr << error.what() << '\n';
    return 1;
}
