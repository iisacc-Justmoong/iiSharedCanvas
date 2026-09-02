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
    document.layers.emplace_back(BitmapLayer{
        {"animated", "Animated", true, 1.0, {}, RasterBlendMode::SourceOver},
        KeyframedSource{{0}},
    });
    document.layers.emplace_back(VectorLayer{
        {"animated-vector", "Animated vector", true, 1.0, {},
         RasterBlendMode::SourceOver},
        KeyframedSource{{0}},
    });
    document.frames.push_back({
        0,
        {{"animated", "raster"}, {"animated-vector", "vector"}},
    });
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
    mixedKeyframes.frames.push_back({10, {{"animated", "vector"}}});
    std::get<KeyframedSource>(layerSource(mixedKeyframes.layers[0])).frameIndices.push_back(10);
    expect(contains(validate(mixedKeyframes), ValidationCode::ContentKindMismatch),
           "one animated track must not mix raster and vector assets");

    Document unorderedFrames = validDocument();
    unorderedFrames.frames.push_back({10, {{"animated", "raster"}}});
    std::get<KeyframedSource>(layerSource(unorderedFrames.layers[0])).frameIndices.push_back(10);
    std::swap(unorderedFrames.frames.front(), unorderedFrames.frames.back());
    expect(contains(validate(unorderedFrames), ValidationCode::InvalidKeyframes),
           "frame records must remain in strictly increasing timeline order");

    Document duplicateFrames = validDocument();
    duplicateFrames.frames.push_back({0, {{"animated", "raster"}}});
    std::get<KeyframedSource>(layerSource(duplicateFrames.layers[0])).frameIndices.push_back(0);
    expect(contains(validate(duplicateFrames), ValidationCode::InvalidKeyframes),
           "two sparse frame records must not own the same timeline position");

    Document emptyFrame = validDocument();
    emptyFrame.frames.push_back({10, {}});
    expect(contains(validate(emptyFrame), ValidationCode::InvalidKeyframes),
           "a sparse frame record must directly own at least one keyframe");

    Document duplicateLayerKey = validDocument();
    duplicateLayerKey.frames.front().keyframes.push_back({"animated", "raster"});
    expect(contains(validate(duplicateLayerKey), ValidationCode::InvalidKeyframes),
           "one frame must not contain two keyframes for the same stable layer id");

    Document unorderedLayerKeys = validDocument();
    std::swap(unorderedLayerKeys.frames.front().keyframes.front(),
              unorderedLayerKeys.frames.front().keyframes.back());
    expect(contains(validate(unorderedLayerKeys), ValidationCode::InvalidKeyframes),
           "one frame's keyframes must use canonical layer-id order");

    Document missingDerivedIndex = validDocument();
    std::get<KeyframedSource>(
        layerSource(missingDerivedIndex.layers.front())).frameIndices.clear();
    expect(contains(validate(missingDerivedIndex), ValidationCode::InvalidKeyframes),
           "a keyframed layer index must contain every frame that owns one of its keys");

    Document extraDerivedIndex = validDocument();
    std::get<KeyframedSource>(
        layerSource(extraDerivedIndex.layers.front())).frameIndices.push_back(10);
    expect(contains(validate(extraDerivedIndex), ValidationCode::InvalidKeyframes),
           "a keyframed layer index must not name a frame that owns no matching key");

    Document duplicateDerivedIndex = validDocument();
    std::get<KeyframedSource>(
        layerSource(duplicateDerivedIndex.layers.front())).frameIndices.push_back(0);
    expect(contains(validate(duplicateDerivedIndex), ValidationCode::InvalidKeyframes),
           "a keyframed layer index must be strictly increasing and duplicate-free");

    Document missingAsset = validDocument();
    missingAsset.frames.front().keyframes.front().assetId = "missing";
    expect(contains(validate(missingAsset), ValidationCode::MissingAsset),
           "all layer sources must resolve to an asset");

    Document missingLayer = validDocument();
    missingLayer.frames.front().keyframes.front().layerId = "missing";
    expect(contains(validate(missingLayer), ValidationCode::InvalidKeyframes),
           "a frame keyframe must resolve its stable owning layer id");

    Document staticLayerKeyframe = validDocument();
    layerSource(staticLayerKeyframe.layers.front()) = StaticSource{"raster"};
    expect(contains(validate(staticLayerKeyframe), ValidationCode::InvalidKeyframes),
           "a frame must not retain a keyframe that names a static layer");

    Document bitmapLayerWithVector = validDocument();
    layerSource(bitmapLayerWithVector.layers[0]) = StaticSource{"vector"};
    expect(contains(validate(bitmapLayerWithVector), ValidationCode::ContentKindMismatch),
           "a bitmap layer must reject a vector asset even for a static source");

    Document vectorLayerWithBitmap = validDocument();
    vectorLayerWithBitmap.frames.front().keyframes[1].assetId = "raster";
    expect(contains(validate(vectorLayerWithBitmap), ValidationCode::ContentKindMismatch),
           "a vector layer must reject a bitmap asset in every keyframe");

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
    layerProperties(invalidLayer.layers.front()).opacity =
        std::numeric_limits<double>::quiet_NaN();
    expect(contains(validate(invalidLayer), ValidationCode::InvalidLayer),
           "layer values must be finite");

    Document unsupportedLayerBlend = validDocument();
    layerProperties(unsupportedLayerBlend.layers.front()).blendMode =
        RasterBlendMode::DestinationOut;
    expect(contains(validate(unsupportedLayerBlend), ValidationCode::InvalidLayer),
           "brush-only destination-out must not silently become a document layer blend mode");

    Document overlappingRanges = validDocument();
    layerProperties(overlappingRanges.layers[0]).frameRange = LayerFrameRange{1, 24};
    layerProperties(overlappingRanges.layers[1]).frameRange = LayerFrameRange{12, 28};
    expect(validate(overlappingRanges).ok(),
           "layers must independently accept overlapping inclusive frame ranges");

    Document singleFrameRange = validDocument();
    layerProperties(singleFrameRange.layers[0]).frameRange = LayerFrameRange{14, 14};
    expect(validate(singleFrameRange).ok(),
           "a layer must be allowed to exist for exactly one inclusive frame");

    Document reversedRange = validDocument();
    layerProperties(reversedRange.layers[0]).frameRange = LayerFrameRange{8, 7};
    expect(contains(validate(reversedRange), ValidationCode::InvalidLayerFrameRange),
           "a layer range must reject firstFrame after lastFrame");

    Document rangePastTimeline = validDocument();
    layerProperties(rangePastTimeline.layers[0]).frameRange =
        LayerFrameRange{1, rangePastTimeline.timeline.frameCount};
    expect(contains(validate(rangePastTimeline), ValidationCode::InvalidLayerFrameRange),
           "an inclusive layer lastFrame must remain inside the timeline");

    Document legacyRange = validDocument();
    legacyRange.formatVersion = {1, 2};
    layerProperties(legacyRange.layers[0]).frameRange = LayerFrameRange{1, 24};
    expect(contains(validate(legacyRange), ValidationCode::InvalidLayerFrameRange),
           "formats before 1.3 must not silently discard a layer frame range");

    Document preservedOutsideKeys = validDocument();
    layerProperties(preservedOutsideKeys.layers[0]).frameRange = LayerFrameRange{5, 20};
    expect(validate(preservedOutsideKeys).ok(),
           "a layer range must preserve valid frame-zero keys for hold sampling at range entry");

    Document validGeneration = validDocument();
    validGeneration.formatVersion = {1, 2};
    StableDiffusionMetadata generation;
    generation.positivePrompt = "product photograph";
    generation.samplingPasses.push_back({
        "3", 1234, 30, 5.5, "dpmpp_2m", "karras", 1.0,
        std::nullopt, std::nullopt,
    });
    generation.comfyUi.promptJson =
        R"json({"3":{"class_type":"KSampler","inputs":{"seed":1234,"steps":30,"cfg":5.5}}})json";
    validGeneration.stableDiffusionMetadata = generation;
    expect(validate(validGeneration).ok(),
           "a format 1.2 document must accept validated Stable Diffusion metadata");

    Document invalidGeneration = validGeneration;
    invalidGeneration.stableDiffusionMetadata->samplingPasses.front().steps = 0;
    expect(contains(validate(invalidGeneration),
                    ValidationCode::InvalidStableDiffusionMetadata),
           "document validation must reject invalid generation parameters");

    Document legacyGeneration = validGeneration;
    legacyGeneration.formatVersion = {1, 1};
    expect(contains(validate(legacyGeneration),
                    ValidationCode::InvalidStableDiffusionMetadata),
           "format 1.1 must not silently discard format 1.2 generation metadata");

    Document invalidVector = validDocument();
    auto &path = std::get<VectorAsset>(invalidVector.assets[1]).paths.front();
    path.commands.front() = LineTo{{1.0, 1.0}};
    expect(contains(validate(invalidVector), ValidationCode::InvalidVectorAsset),
           "vector paths must start with MoveTo");

    Document outOfRangeKeyframe = validDocument();
    outOfRangeKeyframe.frames.front().index = outOfRangeKeyframe.timeline.frameCount;
    for (Layer &layer : outOfRangeKeyframe.layers) {
        std::get<KeyframedSource>(layerSource(layer)).frameIndices.front() =
            outOfRangeKeyframe.timeline.frameCount;
    }
    expect(contains(validate(outOfRangeKeyframe), ValidationCode::InvalidKeyframes),
           "frame-owned keyframes must remain inside the timeline");

    Document missingFrameZero = validDocument();
    missingFrameZero.frames.front().index = 1;
    for (Layer &layer : missingFrameZero.layers) {
        std::get<KeyframedSource>(layerSource(layer)).frameIndices.front() = 1;
    }
    expect(contains(validate(missingFrameZero), ValidationCode::InvalidKeyframes),
           "every keyframed layer must still begin with a keyframe at frame zero");

    return failures == 0 ? 0 : 1;
}
