#include <iiSharedCanvas.h>

#include <cmath>
#include <iostream>
#include <limits>
#include <type_traits>

namespace {
int failures = 0;
void expect(bool value, const char *message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
bool near(double a, double b) { return std::abs(a - b) < 0.000001; }

iiSharedCanvas::Document mixedDocument()
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = {8, 4};
    document.timeline = {{24, 1}, 8};
    document.assets.emplace_back(RasterAsset{"background", makeRasterLayer(8, 4, 0xff000000)});
    VectorPath path;
    path.commands = {MoveTo{{0, 0}}, LineTo{{1, 0}}, LineTo{{1, 1}}, LineTo{{0, 1}}, ClosePath{}};
    path.fill = SolidPaint{0xff00ff00};
    document.assets.emplace_back(VectorAsset{"shape", {1, 1}, {path}});
    document.assets.emplace_back(VideoAsset{"movie", {12, 1},
        {makeRasterLayer(1, 1, 0xffff0000), makeRasterLayer(1, 1, 0xff0000ff)}});
    document.layers.emplace_back(BitmapLayer{{"background", "Background"}, StaticSource{"background"}});
    VectorLayer vector{{"graphics", "Motion graphics"}, StaticSource{"shape"}};
    MotionKeyframe first;
    first.value.position = {0, 2};
    MotionKeyframe last = first;
    last.frame = 4;
    last.value.position.x = 4;
    vector.properties.motion = {first, last};
    document.layers.emplace_back(std::move(vector));
    VideoLayer video{{"footage", "Video"}, StaticSource{"movie"}};
    video.properties.frameRange = LayerFrameRange{1, 6};
    document.layers.emplace_back(std::move(video));
    return document;
}
}

int main()
{
    using namespace iiSharedCanvas;
    static_assert(std::is_aggregate_v<VideoAsset> && std::is_aggregate_v<VideoLayer>);
    static_assert(std::is_aggregate_v<MotionKeyframe> && std::is_aggregate_v<MotionValue>);
    static_assert(std::variant_size_v<Asset> == 4 && std::variant_size_v<iiSharedCanvas::Layer> == 3);
    auto document = mixedDocument();
    expect(validate(document).ok(), "mixed image/vector/video/motion document must validate");
    auto *video = findVideoLayer(document, "footage");
    expect(video && findVideoAsset(document, "movie") && contentKind(document.layers[2]) == ContentKind::Video,
           "video must have a typed layer and asset identity");
    if (!video) { return 1; }
    expect(!videoFrameIndexAt(document, *video, 0) && videoFrameIndexAt(document, *video, 1) == 0
        && videoFrameIndexAt(document, *video, 2) == 0 && videoFrameIndexAt(document, *video, 3) == 1
        && !videoFrameIndexAt(document, *video, 5), "video sampling must respect local clip time, rational rates and exhaustion");
    video->playback.endBehavior = VideoEndBehavior::Hold;
    expect(videoFrameIndexAt(document, *video, 6) == 1 && !videoFrameIndexAt(document, *video, 7),
           "hold may extend a trimmed source but may not extend the layer existence range");
    video->playback.sourceInFrame = 1;
    video->playback.sourceOutFrame = 2;
    expect(videoFrameIndexAt(document, *video, 1) == 1, "source trims must start at the selected frame");
    video->playback = {};

    const auto snapshot = encodeIisc(document);
    const auto rendered = renderFrame(document, 3);
    expect(rendered.ok() && rendered.pixels.pixels[0] == 0xff0000ff
        && rendered.pixels.pixels[2 * 8 + 3] == 0xff00ff00
        && rendered.pixels.pixels[2 * 8] == 0xff000000,
        "one rendered canvas must contain the video frame and the moving native vector over the image");
    const auto end = renderFrame(document, 5);
    expect(end.ok() && end.pixels.pixels[0] == 0xff000000, "exhausted transparent video must reveal the lower layer");
    expect(snapshot.ok() && encodeIisc(document).bytes == snapshot.bytes, "sampling and rendering must never mutate source data");

    auto &properties = layerProperties(document.layers[1]);
    properties.opacity = 0.8;
    properties.transform.translationX = 10;
    properties.motion[1].value.opacity = 0.0;
    auto sampled = sampleLayerAt(document, document.layers[1], 2);
    expect(near(sampled.transform.translationX, 12) && near(sampled.opacity, 0.4),
           "motion must compose with the base transform and multiply the base opacity");
    properties.motion[0].interpolation = MotionInterpolation::Hold;
    expect(near(sampleLayerAt(document, document.layers[1], 3).transform.translationX, 10),
           "hold interpolation must preserve the left key until the right key");
    expect(near(sampleLayerAt(document, document.layers[1], 4).transform.translationX, 14),
           "an exact key boundary must sample that key");
    properties.motion[0].interpolation = MotionInterpolation::SmoothStep;
    expect(near(sampleLayerAt(document, document.layers[1], 1).transform.translationX, 10.625),
           "smooth interpolation must apply cubic smoothstep within a segment");
    properties.motion = {{0, {{}, {2, 3}, {1, 1}, 90, 1}, MotionInterpolation::Linear}};
    sampled = sampleLayerAt(document, document.layers[1], 2);
    expect(near(sampled.transform.m12, 2) && near(sampled.transform.m21, -3)
        && near(sampled.transform.translationX, 14) && near(sampled.transform.translationY, -1),
        "rotation and nonuniform scale must pivot around the anchor before the base transform");

    document = mixedDocument();
    auto invalid = document;
    findVideoAsset(invalid, "movie")->frames[1].width = 2;
    expect(!validate(invalid).ok(), "inconsistent video frame extents must fail");
    invalid = document;
    findVideoAsset(invalid, "movie")->frameRate.denominator = 0;
    expect(!validate(invalid).ok(), "zero video rate denominators must fail");
    invalid = document;
    findVideoLayer(invalid, "footage")->playback.sourceOutFrame = 0;
    expect(!validate(invalid).ok(), "empty video trim ranges must fail");
    invalid = document;
    layerSource(invalid.layers[0]) = StaticSource{"movie"};
    expect(!validate(invalid).ok(), "bitmap layers must not silently accept video assets");
    invalid = document;
    layerProperties(invalid.layers[1]).motion[1].frame = 0;
    expect(!validate(invalid).ok(), "motion keyframes must be strictly ordered");
    invalid = document;
    layerProperties(invalid.layers[1]).motion[1].value.opacity = std::numeric_limits<double>::quiet_NaN();
    expect(!validate(invalid).ok(), "nonfinite motion values must fail");
    invalid = document;
    invalid.formatVersion.minor = 5;
    expect(!validate(invalid).ok(), "video and motion require the new format version");

    DocumentEditor editor(document);
    const auto revision = editor.revision();
    const auto original = encodeIisc(document).bytes;
    auto badKeys = layerProperties(document.layers[1]).motion;
    badKeys.back().frame = 8;
    expect(!editor.setLayerMotion("graphics", badKeys).ok() && editor.revision() == revision
        && encodeIisc(document).bytes == original, "invalid motion edits must preserve the entire document and revision");
    expect(!editor.setFrameCount(4).ok(), "timeline shortening must respect motion keys and clip ranges");
    expect(!editor.setVideoPlayback("footage", {2, {}, VideoEndBehavior::Transparent}).ok(),
           "out-of-bounds video edits must roll back");
    expect(editor.removeAsset("movie").code == DocumentEditCode::AssetReferenced,
           "referenced video assets may not be removed");
    expect(editor.renameAsset("movie", "renamed").changed
        && std::get<StaticSource>(findVideoLayer(document, "footage")->source).assetId == "renamed",
        "video references must follow a stable asset rename");
    const auto current = editor.revision();
    expect(!editor.setLayerMotion("graphics", layerProperties(document.layers[1]).motion).changed
        && !editor.setVideoPlayback("footage", {}).changed && editor.revision() == current,
        "identical motion and video edits must not create revisions");
    auto replacement = *findVideoAsset(document, "renamed");
    replacement.frames.clear();
    expect(!editor.replaceVideoAsset("renamed", replacement).ok() && editor.revision() == current,
           "invalid video replacement must be transactional");

    Document legacy;
    legacy.extent = {1, 1};
    legacy.formatVersion.minor = 0;
    DocumentEditor legacyEditor(legacy);
    expect(legacyEditor.insertVideoAsset({"movie", {24, 1}, {makeRasterLayer(1, 1)}}).changed
        && legacy.formatVersion.minor == CurrentFormatMinor, "native video insertion must upgrade legacy documents");

    document = mixedDocument();
    document.timeline = {{30000, 1001}, 100000};
    video = findVideoLayer(document, "footage");
    video->properties.frameRange.reset();
    auto *asset = findVideoAsset(document, "movie");
    asset->frameRate = {24000, 1001};
    expect(videoFrameIndexAt(document, *video, 1) == 0 && videoFrameIndexAt(document, *video, 2) == 1,
           "NTSC rational frame mapping must not round at the wrong boundary");
    document.timeline.frameRate = {0xffffffffU, 0xfffffffeU};
    asset->frameRate = document.timeline.frameRate;
    expect(videoFrameIndexAt(document, *video, 1) == 1 && !videoFrameIndexAt(document, *video, 99999),
           "full-width rational arithmetic must not overflow or lose frame boundaries");
    asset->frameRate = {0xfffffffcU, 0xfffffffdU};
    expect(videoFrameIndexAt(document, *video, 1) == 0,
           "almost-equal rational rates must preserve a sub-ULP difference below the frame boundary");
    asset->frameRate = {0xfffffffdU, 0xfffffffcU};
    expect(videoFrameIndexAt(document, *video, 1) == 1,
           "almost-equal rational rates above the boundary must advance the source frame");

    document = mixedDocument();
    DocumentEditor motionEditor(document);
    auto alternate = *findVectorAsset(document, "shape");
    alternate.paths.front().fill = SolidPaint{0xffffff00};
    expect(motionEditor.insertVectorAsset("alternate", alternate.viewport, alternate.paths).ok()
        && motionEditor.setKeyframedSource("graphics", {{0, "shape"}, {3, "alternate"}}).ok(),
        "source-switch keys may coexist with property motion on the same vector layer");
    expect(renderFrame(document, 3).pixels.pixels[2 * 8 + 3] == 0xffffff00,
           "source switching and position interpolation must evaluate independently");
    const auto beforeInvalid = encodeIisc(document).bytes;
    expect(!motionEditor.setKeyframedSource("footage", {{0, "movie"}}).ok()
        && encodeIisc(document).bytes == beforeInvalid,
        "video may not be converted to the bitmap/vector asset-switch protocol");
    expect(motionEditor.setLayerMotion("graphics", {}).changed
        && near(sampleLayerAt(document, document.layers[1], 3).transform.translationX, 0),
        "clearing property keys must restore the base transform");

    auto overflow = mixedDocument();
    auto &overflowProperties = layerProperties(overflow.layers[1]);
    overflowProperties.transform.m11 = 2;
    overflowProperties.motion[0].value.scale.x = std::numeric_limits<double>::max();
    expect(renderFrame(overflow, 0).status == FrameRenderStatus::InvalidDocument,
           "an overflowing composed motion transform must fail before raster indexing");
    overflowProperties.transform = {};
    overflowProperties.motion[0].value.scale = {1e200, 1e200};
    expect(renderFrame(overflow, 0).status == FrameRenderStatus::InvalidDocument,
           "finite coefficients whose determinant overflows must fail before inverse mapping");
    return failures ? 1 : 0;
}
