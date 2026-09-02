#include <iiSharedCanvas.h>

#include <iostream>
#include <optional>
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

    document.layers.emplace_back(BitmapLayer{
        {"layer-raster", "Raster", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"raster-static"},
    });
    document.layers.emplace_back(VectorLayer{
        {"layer-vector", "Vector", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"vector-static"},
    });
    document.layers.emplace_back(BitmapLayer{
        {"layer-animated-raster", "Animated raster", true, 1.0, {},
         RasterBlendMode::SourceOver},
        KeyframedSource{{0, 24}},
    });
    document.layers.emplace_back(VectorLayer{
        {"layer-animated-vector", "Animated vector", true, 1.0, {},
         RasterBlendMode::SourceOver},
        KeyframedSource{{0, 12}},
    });
    document.frames = {
        {0, {{"layer-animated-raster", "raster-frame-0"},
             {"layer-animated-vector", "vector-frame-0"}}},
        {12, {{"layer-animated-vector", "vector-frame-12"}}},
        {24, {{"layer-animated-raster", "raster-frame-24"}}},
    };
    return document;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    const Document document = makeDocument();
    expect(validate(document).ok(),
           "a document must accept static raster, static vector, and keyframed raster/vector layers together");
    expect(std::holds_alternative<BitmapLayer>(document.layers[0])
               && std::holds_alternative<VectorLayer>(document.layers[1])
               && contentKind(document.layers[0]) == ContentKind::Raster
               && contentKind(document.layers[1]) == ContentKind::Vector,
           "the layer variant must expose bitmap and vector identity directly");
    expect(findBitmapLayer(document, "layer-raster") != nullptr
               && findVectorLayer(document, "layer-vector") != nullptr
               && findVectorLayer(document, "layer-raster") == nullptr,
           "typed layer lookup must never return a layer of the other content kind");
    const Frame *frameZero = findFrame(document, 0);
    const Frame *frameTwelve = findFrame(document, 12);
    expect(frameZero
               && frameZero->keyframes.size() == 2
               && findKeyframe(*frameZero, "layer-animated-raster") != nullptr
               && keyframeIndex(*frameZero, "layer-animated-vector")
                    == std::optional<std::size_t>{1}
               && frameIndex(document, 12) == std::optional<std::size_t>{1}
               && frameTwelve
               && findKeyframe(*frameTwelve, "layer-animated-vector")->assetId
                    == "vector-frame-12",
           "a frame must directly own independently addressable layer keyframes");
    expect(findFrame(document, 11) == nullptr
               && frameIndex(document, 11) == std::nullopt
               && findKeyframe(document, "layer-animated-vector", 11) == nullptr
               && findKeyframe(document, "layer-animated-vector", 12)
                    == findKeyframe(*frameTwelve, "layer-animated-vector"),
           "exact sparse-frame lookup must remain distinct from hold sampling");
    const std::vector<AssetReference> frameReferences = assetReferences(
        document, "vector-frame-0");
    expect(frameReferences.size() == 1
               && frameReferences.front().layerIndex == 3
               && frameReferences.front().frameIndex
                    == std::optional<std::size_t>{0}
               && frameReferences.front().keyframeIndex
                    == std::optional<std::size_t>{1},
           "frame-owned asset references must resolve their owning layer index in one traversal");

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

    Document ranged = makeDocument();
    layerProperties(ranged.layers[0]).frameRange = LayerFrameRange{1, 24};
    layerProperties(ranged.layers[1]).frameRange = LayerFrameRange{12, 28};
    layerProperties(ranged.layers[3]).frameRange = LayerFrameRange{6, 28};
    expect(validate(ranged).ok(),
           "static and keyframed layers must accept independent overlapping frame ranges");
    expect(!layerExistsAt(ranged, ranged.layers[0], 0)
               && layerExistsAt(ranged, ranged.layers[0], 1)
               && layerExistsAt(ranged, ranged.layers[0], 24)
               && !layerExistsAt(ranged, ranged.layers[0], 25)
               && !layerExistsAt(ranged, ranged.layers[1], 11)
               && layerExistsAt(ranged, ranged.layers[1], 12)
               && layerExistsAt(ranged, ranged.layers[1], 28)
               && !layerExistsAt(ranged, ranged.layers[1], 29)
               && !layerExistsAt(ranged, ranged.layers[1], 48),
           "layer existence ranges must include both firstFrame and lastFrame exactly");
    expect(resolveAssetAt(ranged, ranged.layers[0], 0) == nullptr
               && assetId(*resolveAssetAt(ranged, ranged.layers[0], 1))
                    == "raster-static"
               && assetId(*resolveAssetAt(ranged, ranged.layers[0], 24))
                    == "raster-static"
               && resolveAssetAt(ranged, ranged.layers[0], 25) == nullptr,
           "static layer sampling must fail closed outside its existence range");
    const Asset *heldAtRangeStart = resolveAssetAt(ranged, ranged.layers[3], 6);
    expect(heldAtRangeStart && assetId(*heldAtRangeStart) == "vector-frame-0",
           "a keyframed layer must preserve pre-range keys and hold their value at range entry");

    Document sparseLookup;
    sparseLookup.extent = {1, 1};
    sparseLookup.timeline = {{24, 1}, 2048};
    sparseLookup.assets.emplace_back(
        RasterAsset{"background", makeRasterLayer(1, 1, 0xff010203U)});
    sparseLookup.assets.emplace_back(
        RasterAsset{"busy", makeRasterLayer(1, 1, 0xff040506U)});
    sparseLookup.layers.emplace_back(BitmapLayer{
        {"background-layer", "Background", true, 1.0, {},
         RasterBlendMode::SourceOver},
        KeyframedSource{{0}},
    });
    std::vector<FrameIndex> busyFrames;
    busyFrames.reserve(2048);
    sparseLookup.frames.reserve(2048);
    for (FrameIndex frame = 0; frame < 2048; ++frame) {
        busyFrames.push_back(frame);
        sparseLookup.frames.push_back({
            frame,
            frame == 0
                ? std::vector<Keyframe>{{"background-layer", "background"},
                                        {"busy-layer", "busy"}}
                : std::vector<Keyframe>{{"busy-layer", "busy"}},
        });
    }
    sparseLookup.layers.emplace_back(BitmapLayer{
        {"busy-layer", "Busy", true, 1.0, {}, RasterBlendMode::SourceOver},
        KeyframedSource{std::move(busyFrames)},
    });
    const Asset *lateBackground = resolveAssetAt(
        sparseLookup, sparseLookup.layers.front(), 2047);
    expect(lateBackground && assetId(*lateBackground) == "background",
           "hold sampling must use a layer's derived owner-frame index instead of scanning unrelated frames");

    expect(contentKind(document.assets[0]) == ContentKind::Raster,
           "iiPaintEngine RasterLayer must be the raster asset representation");

    return failures == 0 ? 0 : 1;
}
