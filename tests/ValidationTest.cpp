#include <iiSharedCanvas.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

bool contains(const iiSharedCanvas::ValidationResult &result,
              iiSharedCanvas::ValidationCode code)
{
    return std::any_of(result.issues.begin(), result.issues.end(),
                       [code](const auto &issue) { return issue.code == code; });
}

iiSharedCanvas::Document validDocument()
{
    using namespace iiSharedCanvas;

    VectorPath path;
    path.commands = {MoveTo{{0.0, 0.0}}, LineTo{{32.0, 32.0}}};
    path.stroke = StrokeStyle{SolidPaint{0xff000000U}, 2.0};

    Document document;
    document.extent = {32, 32};
    document.timeline = {{30, 1}, 30};
    document.assets.emplace_back(RasterAsset{"raster", makeRasterLayer(32, 32)});
    document.assets.emplace_back(VectorAsset{"vector", {32, 32}, {path}});
    document.layers.push_back({"animated", "Animated", true, 1.0, {},
                               RasterBlendMode::SourceOver,
                               KeyframedSource{ContentKind::Raster, {{0, "raster"}}}});
    return document;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    Document duplicateAssets = validDocument();
    duplicateAssets.assets.emplace_back(RasterAsset{"raster", makeRasterLayer(1, 1)});
    expect(contains(validate(duplicateAssets), ValidationCode::DuplicateAssetId),
           "duplicate asset ids must be rejected");

    Document mixedKeyframes = validDocument();
    auto &source = std::get<KeyframedSource>(mixedKeyframes.layers[0].source);
    source.keyframes.push_back({10, "vector"});
    expect(contains(validate(mixedKeyframes), ValidationCode::ContentKindMismatch),
           "one animated track must not mix raster and vector assets");

    Document unorderedKeyframes = validDocument();
    auto &unordered = std::get<KeyframedSource>(unorderedKeyframes.layers[0].source);
    unordered.keyframes.push_back({0, "raster"});
    expect(contains(validate(unorderedKeyframes), ValidationCode::InvalidKeyframes),
           "keyframe positions must be unique and strictly increasing");

    Document missingAsset = validDocument();
    std::get<KeyframedSource>(missingAsset.layers[0].source).keyframes[0].assetId = "missing";
    expect(contains(validate(missingAsset), ValidationCode::MissingAsset),
           "all layer sources must resolve to an asset");

    Document invalidRaster = validDocument();
    std::get<RasterAsset>(invalidRaster.assets[0]).pixels.pixels.pop_back();
    expect(contains(validate(invalidRaster), ValidationCode::InvalidRasterAsset),
           "raster dimensions and pixel storage must agree");

    Document invalidTimeline = validDocument();
    invalidTimeline.timeline.frameRate.denominator = 0;
    expect(contains(validate(invalidTimeline), ValidationCode::InvalidTimeline),
           "timeline frame rate must be a valid rational number");

    Document futureVersion = validDocument();
    futureVersion.formatVersion = {2, 0};
    expect(contains(validate(futureVersion), ValidationCode::UnsupportedFormatVersion),
           "unknown format versions must fail closed");

    Document invalidCanvas = validDocument();
    invalidCanvas.extent.width = 0;
    expect(contains(validate(invalidCanvas), ValidationCode::InvalidCanvasExtent),
           "canvas dimensions must be positive");

    Document duplicateLayers = validDocument();
    duplicateLayers.layers.push_back(duplicateLayers.layers.front());
    expect(contains(validate(duplicateLayers), ValidationCode::DuplicateLayerId),
           "layer ids must be unique");

    Document invalidLayer = validDocument();
    invalidLayer.layers.front().opacity = std::numeric_limits<double>::quiet_NaN();
    expect(contains(validate(invalidLayer), ValidationCode::InvalidLayer),
           "layer values must be finite");

    Document unsupportedLayerBlend = validDocument();
    unsupportedLayerBlend.layers.front().blendMode = RasterBlendMode::DestinationOut;
    expect(contains(validate(unsupportedLayerBlend), ValidationCode::InvalidLayer),
           "brush-only destination-out must not silently become a document layer blend mode");

    Document invalidVector = validDocument();
    auto &path = std::get<VectorAsset>(invalidVector.assets[1]).paths.front();
    path.commands.front() = LineTo{{1.0, 1.0}};
    expect(contains(validate(invalidVector), ValidationCode::InvalidVectorAsset),
           "vector paths must start with MoveTo");

    Document outOfRangeKeyframe = validDocument();
    std::get<KeyframedSource>(outOfRangeKeyframe.layers[0].source)
        .keyframes.front().frame = outOfRangeKeyframe.timeline.frameCount;
    expect(contains(validate(outOfRangeKeyframe), ValidationCode::InvalidKeyframes),
           "keyframes must begin at zero and remain inside the timeline");

    return failures == 0 ? 0 : 1;
}
