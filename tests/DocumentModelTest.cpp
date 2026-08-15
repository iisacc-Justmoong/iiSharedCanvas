#include <iiSharedCanvas.h>

#include <iostream>
#include <string>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

iiSharedCanvas::VectorAsset makeVectorAsset(std::string id, std::uint32_t color)
{
    iiSharedCanvas::VectorPath path;
    path.commands = {
        iiSharedCanvas::MoveTo{{0.0, 0.0}},
        iiSharedCanvas::LineTo{{64.0, 0.0}},
        iiSharedCanvas::LineTo{{64.0, 64.0}},
        iiSharedCanvas::ClosePath{},
    };
    path.fill = iiSharedCanvas::SolidPaint{color};
    return {std::move(id), {64, 64}, {std::move(path)}};
}

iiSharedCanvas::Document makeDocument()
{
    using namespace iiSharedCanvas;

    Document document;
    document.extent = {1920, 1080};
    document.timeline = {{24, 1}, 48};
    document.assets.emplace_back(RasterAsset{"raster-static", makeRasterLayer(64, 64, 0xffffffffU)});
    document.assets.emplace_back(RasterAsset{"raster-frame-0", makeRasterLayer(64, 64, 0xff000000U)});
    document.assets.emplace_back(RasterAsset{"raster-frame-24", makeRasterLayer(64, 64, 0xffffffffU)});
    document.assets.emplace_back(makeVectorAsset("vector-static", 0xffff0000U));
    document.assets.emplace_back(makeVectorAsset("vector-frame-0", 0xff00ff00U));
    document.assets.emplace_back(makeVectorAsset("vector-frame-12", 0xff0000ffU));

    document.layers.push_back({"layer-raster", "Raster", true, 1.0, {},
                               RasterBlendMode::SourceOver, StaticSource{"raster-static"}});
    document.layers.push_back({"layer-vector", "Vector", true, 1.0, {},
                               RasterBlendMode::SourceOver, StaticSource{"vector-static"}});
    document.layers.push_back({"layer-animated-raster", "Animated raster", true, 1.0, {},
                               RasterBlendMode::SourceOver,
                               KeyframedSource{ContentKind::Raster,
                                               {{0, "raster-frame-0"}, {24, "raster-frame-24"}}}});
    document.layers.push_back({"layer-animated-vector", "Animated vector", true, 1.0, {},
                               RasterBlendMode::SourceOver,
                               KeyframedSource{ContentKind::Vector,
                                               {{0, "vector-frame-0"}, {12, "vector-frame-12"}}}});
    return document;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    const Document document = makeDocument();
    expect(validate(document).ok(),
           "a document must accept static raster, static vector, and keyframed raster/vector layers together");

    const Asset *staticRaster = resolveAssetAt(document, document.layers[0], 47);
    expect(staticRaster && assetId(*staticRaster) == "raster-static",
           "a static raster layer must resolve at every document frame");

    const Asset *rasterBeforeCut = resolveAssetAt(document, document.layers[2], 23);
    const Asset *rasterAfterCut = resolveAssetAt(document, document.layers[2], 24);
    expect(rasterBeforeCut && assetId(*rasterBeforeCut) == "raster-frame-0",
           "raster keyframes must use deterministic hold sampling before a cut");
    expect(rasterAfterCut && assetId(*rasterAfterCut) == "raster-frame-24",
           "raster keyframes must switch on the exact keyframe");

    const Asset *vectorBeforeCut = resolveAssetAt(document, document.layers[3], 11);
    const Asset *vectorAfterCut = resolveAssetAt(document, document.layers[3], 12);
    expect(vectorBeforeCut && assetId(*vectorBeforeCut) == "vector-frame-0",
           "vector keyframes must use deterministic hold sampling before a cut");
    expect(vectorAfterCut && assetId(*vectorAfterCut) == "vector-frame-12",
           "vector keyframes must switch on the exact keyframe");

    expect(resolveAssetAt(document, document.layers[3], 48) == nullptr,
           "sampling outside the timeline must fail closed");
    expect(contentKind(document.assets[0]) == ContentKind::Raster,
           "iiPaintEngine RasterLayer must be the raster asset representation");

    return failures == 0 ? 0 : 1;
}
