#include <iiSharedCanvas.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes)
{
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc ^ 0xffffffffU;
}

void refreshPayloadChecksum(std::vector<std::uint8_t> &bytes)
{
    const std::uint32_t checksum = crc32(
        std::span<const std::uint8_t>(bytes).subspan(iiSharedCanvas::IiscHeaderSize));
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[24 + index] = static_cast<std::uint8_t>((checksum >> (index * 8U)) & 0xffU);
    }
}

void overwriteU32(std::vector<std::uint8_t> &bytes,
                  std::size_t offset,
                  std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void overwriteU64(std::vector<std::uint8_t> &bytes,
                  std::size_t offset,
                  std::uint64_t value)
{
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(
            (value >> (index * 8U)) & 0xffU);
    }
}

void truncatePayload(std::vector<std::uint8_t> &bytes, std::size_t count)
{
    bytes.resize(bytes.size() - count);
    overwriteU64(bytes, 16, bytes.size() - iiSharedCanvas::IiscHeaderSize);
    refreshPayloadChecksum(bytes);
}

std::vector<std::uint8_t> bytesFromHex(std::string_view hex)
{
    const auto nibble = [](char value) -> std::uint8_t {
        return value >= '0' && value <= '9'
            ? static_cast<std::uint8_t>(value - '0')
            : static_cast<std::uint8_t>(value - 'a' + 10);
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        bytes.push_back(static_cast<std::uint8_t>(
            (nibble(hex[index]) << 4U) | nibble(hex[index + 1])));
    }
    return bytes;
}

iiSharedCanvas::Document legacyGoldenDocument(std::uint16_t minor)
{
    using namespace iiSharedCanvas;
    Document document;
    document.formatVersion = {1, minor};
    document.extent = {1, 1};
    document.timeline = {{24, 1}, 2};
    document.assets.emplace_back(
        RasterAsset{"p", makeRasterLayer(1, 1, 0xff010203U)});
    document.layers.emplace_back(BitmapLayer{
        {"l", "L", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"p"},
    });
    if (minor >= 2) {
        StableDiffusionMetadata metadata;
        metadata.positivePrompt = "legacy fixture";
        metadata.software = "iiSharedCanvas 0.2.0";
        document.stableDiffusionMetadata = std::move(metadata);
    }
    return document;
}

iiSharedCanvas::VectorAsset vectorAsset(std::string id, std::uint32_t fillColor)
{
    using namespace iiSharedCanvas;
    VectorPath path;
    path.commands = {
        MoveTo{{0.0, 0.0}},
        LineTo{{4.0, 0.0}},
        QuadraticTo{{4.0, 2.0}, {4.0, 4.0}},
        CubicTo{{3.0, 4.0}, {1.0, 4.0}, {0.0, 4.0}},
        ClosePath{},
    };
    path.fill = SolidPaint{fillColor};
    path.stroke = StrokeStyle{SolidPaint{0xff010203U}, 1.25};
    return {std::move(id), {4, 4}, {std::move(path)}};
}

const iiSharedCanvas::KeyframedSource *keyframedSource(
    const iiSharedCanvas::Document &document,
    const std::string &layerId)
{
    const iiSharedCanvas::Layer *layer = iiSharedCanvas::findLayer(document,
                                                                   layerId);
    return layer
        ? std::get_if<iiSharedCanvas::KeyframedSource>(
              &iiSharedCanvas::layerSource(*layer))
        : nullptr;
}

iiSharedCanvas::Document completeDocument()
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = {4, 4};
    document.timeline = {{30000, 1001}, 3};

    RasterLayer varied = makeRasterLayer(2, 2, 0x00000000U);
    varied.pixels = {0x00000000U, 0x80ff0000U, 0xff00ff00U, 0xff0000ffU};
    document.assets.emplace_back(RasterAsset{"raster/static", std::move(varied)});
    document.assets.emplace_back(RasterAsset{"raster-frame-0", makeRasterLayer(4, 4, 0xff102030U)});
    document.assets.emplace_back(RasterAsset{"raster-frame-2", makeRasterLayer(4, 4, 0xff304050U)});
    document.assets.emplace_back(vectorAsset("vector-static", 0x80ffcc00U));
    document.assets.emplace_back(vectorAsset("vector-frame-0", 0xff112233U));
    document.assets.emplace_back(vectorAsset("vector-frame-1", 0xffaabbccU));

    AffineTransform transformed;
    transformed.m11 = 0.75;
    transformed.m12 = 0.25;
    transformed.m21 = -0.5;
    transformed.m22 = 1.25;
    transformed.translationX = 1.5;
    transformed.translationY = -0.25;
    document.layers.emplace_back(BitmapLayer{
        {"static-raster", "Static raster / 한글", false, 0.375, transformed,
         RasterBlendMode::Screen},
        StaticSource{"raster/static"},
    });
    document.layers.emplace_back(VectorLayer{
        {"static-vector", "Static vector", true, 0.875, {}, RasterBlendMode::Multiply},
        StaticSource{"vector-static"},
    });
    document.layers.emplace_back(BitmapLayer{
        {"z-animated-raster", "Animated raster", true, 1.0, {},
         RasterBlendMode::SourceOver},
        KeyframedSource{{0, 2}},
    });
    document.layers.emplace_back(VectorLayer{
        {"a-animated-vector", "Animated vector", true, 0.5, {}, RasterBlendMode::Overlay},
        KeyframedSource{{0, 1}},
    });
    layerProperties(document.layers[0]).frameRange = LayerFrameRange{0, 1};
    layerProperties(document.layers[1]).frameRange = LayerFrameRange{1, 2};
    layerProperties(document.layers[2]).frameRange = LayerFrameRange{0, 2};
    layerProperties(document.layers[3]).frameRange = LayerFrameRange{0, 1};
    document.frames = {
        {0,
         {
             {"a-animated-vector", "vector-frame-0"},
             {"z-animated-raster", "raster-frame-0"},
         }},
        {1, {{"a-animated-vector", "vector-frame-1"}}},
        {2, {{"z-animated-raster", "raster-frame-2"}}},
    };

    StableDiffusionMetadata generation;
    generation.positivePrompt = "editorial portrait, soft rim light";
    generation.negativePrompt = "watermark, malformed hands";
    generation.outputExtent = StableDiffusionImageExtent{1024, 1024};
    generation.batchSize = 1;
    generation.clipSkip = 2;
    generation.samplingPasses.push_back({
        "3", 998877665544ULL, 32, 6.25, "dpmpp_2m_sde", "karras", 1.0,
        std::nullopt, std::nullopt,
    });
    generation.models.push_back({
        "checkpoint", "portraitXL.safetensors", "12ab34cd", "sha256", {},
    });
    generation.loras.push_back({"skin-detail.safetensors", "55aa", 0.7, 0.65});
    generation.software = "ComfyUI";
    generation.softwareVersion = "0.3.x";
    generation.createdAt = "2026-09-02T14:00:00Z";
    generation.generationParametersText =
        "Steps: 32, Sampler: DPM++ 2M SDE, CFG scale: 6.25, Seed: 998877665544";
    generation.comfyUi.promptJson =
        R"json({"3":{"class_type":"KSampler","inputs":{"seed":998877665544,"steps":32,"cfg":6.25,"sampler_name":"dpmpp_2m_sde","scheduler":"karras","denoise":1.0}}})json";
    generation.comfyUi.workflowJson =
        R"json({"version":1,"state":{"lastNodeId":9},"nodes":[],"links":[]})json";
    generation.comfyUi.extraPngInfo.push_back(
        {"custom", R"json({"extension":"value"})json"});
    generation.extraParameters.push_back({"controlNet", "depth"});
    document.stableDiffusionMetadata = std::move(generation);
    return document;
}

iiSharedCanvas::Document simpleRangeDocument()
{
    using namespace iiSharedCanvas;
    Document document;
    document.extent = {1, 1};
    document.timeline = {{24, 1}, 4};
    document.assets.emplace_back(
        RasterAsset{"pixel", makeRasterLayer(1, 1, 0xffffffffU)});
    document.layers.emplace_back(BitmapLayer{
        {"range", "Range", true, 1.0, {}, RasterBlendMode::SourceOver},
        StaticSource{"pixel"},
    });
    layerProperties(document.layers.front()).frameRange = LayerFrameRange{1, 2};
    return document;
}

void clearFrameRanges(iiSharedCanvas::Document &document)
{
    for (iiSharedCanvas::Layer &layer : document.layers) {
        iiSharedCanvas::layerProperties(layer).frameRange.reset();
    }
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    // These fixed containers were emitted by the separately installed 0.2.0
    // package, so this compatibility proof does not generate its own oracle.
    struct LegacyGolden {
        std::uint16_t minor;
        std::string_view hex;
    };
    const std::vector<LegacyGolden> legacyGoldens{
        {0,
         "494953430d0a1a0a010000000000000089000000000000000614f5e20000000001000000010000001800000001000000020000000100000000010000007001000000010000000100000000000000000400000000000000030201ff01000000010000006c010000004c01000000000000f03f000000000000f03f00000000000000000000000000000000000000000000f03f0000000000000000000000000000000000000100000070"},
        {1,
         "494953430d0a1a0a01000100000000008a0000000000000080829638000000000100000001000000001800000001000000020000000100000000010000007001000000010000000100000000000000000400000000000000030201ff01000000010000006c010000004c01000000000000f03f000000000000f03f00000000000000000000000000000000000000000000f03f0000000000000000000000000000000000000100000070"},
        {2,
         "494953430d0a1a0a0100020000000000e4000000000000005618fe6a000000000100000001000000001800000001000000020000000100000000010000007001000000010000000100000000000000000400000000000000030201ff01000000010000006c010000004c01000000000000f03f000000000000f03f00000000000000000000000000000000000000000000f03f0000000000000000000000000000000000000100000070010e0000006c656761637920666978747572650000000000000000000000000000000000000014000000696953686172656443616e76617320302e322e3000000000000000000000000000000000000000000000000000000000"},
    };
    for (const LegacyGolden &golden : legacyGoldens) {
        const std::vector<std::uint8_t> oldBytes = bytesFromHex(golden.hex);
        const IiscDecodeResult oldDecoded = decodeIisc(oldBytes);
        const IiscEncodeResult authored = encodeIisc(
            legacyGoldenDocument(golden.minor));
        const IiscEncodeResult oldReencoded = oldDecoded.ok()
            ? encodeIisc(oldDecoded.document)
            : IiscEncodeResult{};
        const RasterAsset *oldRaster = oldDecoded.ok()
            ? findRasterAsset(oldDecoded.document, "p")
            : nullptr;
        const std::string fixtureName = "fixed 0.2.0 format 1."
            + std::to_string(golden.minor) + " fixture";
        expect(oldDecoded.ok(),
               fixtureName + " must decode: " + oldDecoded.error.message);
        expect(authored.ok(),
               fixtureName + " must remain authorable: " + authored.error.message);
        expect(oldDecoded.ok()
                   && authored.ok()
                   && oldDecoded.document.formatVersion.minor == golden.minor
                   && oldRaster
                   && rasterLayerPixelAt(oldRaster->pixels, {0, 0}) == 0xff010203U
                   && static_cast<bool>(
                          oldDecoded.document.stableDiffusionMetadata)
                       == (golden.minor >= 2)
                   && authored.bytes == oldBytes
                   && oldReencoded.ok()
                   && oldReencoded.bytes == oldBytes,
               fixtureName
                   + " must decode semantically and re-encode byte-identically under 1.3");
    }

    const Document original = completeDocument();
    const IiscEncodeResult encoded = encodeIisc(original);
    expect(encoded.ok() && encoded.bytes.size() > IiscHeaderSize,
           "a complete raster/vector/keyframe document must encode into an .iisc container");

    const IiscDecodeResult decoded = decodeIisc(encoded.bytes);
    expect(decoded.ok() && validate(decoded.document).ok(),
           "the canonical .iisc container must decode to a valid document");
    expect(decoded.document.layers.size() == original.layers.size()
               && decoded.document.assets.size() == original.assets.size(),
           "round-trip must preserve every layer and asset");
    expect(decoded.document.formatVersion.minor == 3
               && decoded.document.stableDiffusionMetadata
               && original.stableDiffusionMetadata
               && decoded.document.stableDiffusionMetadata
                   == original.stableDiffusionMetadata,
           "format 1.3 must round-trip typed generation settings and raw ComfyUI metadata exactly");
    if (decoded.document.stableDiffusionMetadata) {
        const StableDiffusionMetadata &generation =
            *decoded.document.stableDiffusionMetadata;
        expect(generation.samplingPasses.front().seed == 998877665544ULL
                   && generation.samplingPasses.front().steps == 32
                   && generation.samplingPasses.front().cfgScale == 6.25
                   && generation.comfyUi.promptJson.find("KSampler")
                       != std::string::npos
                   && generation.comfyUi.workflowJson.find("\"nodes\"")
                       != std::string::npos,
               "decoded metadata must expose reproducible parameters and both ComfyUI JSON graphs");
    }
    expect(layerProperties(decoded.document.layers.front()).name == "Static raster / 한글"
               && decoded.document.timeline.frameRate.numerator == 30000
               && decoded.document.timeline.frameRate.denominator == 1001,
           "round-trip must preserve UTF-8 names and rational frame rates exactly");

    const RasterAsset *decodedImage = findRasterAsset(decoded.document, "raster/static");
    expect(decodedImage
               && decodedImage->pixels.width == 2
               && decodedImage->pixels.height == 2
               && decodedImage->pixels.pixels
                   == std::vector<std::uint32_t>{
                       0x00000000U, 0x80ff0000U, 0xff00ff00U, 0xff0000ffU},
           "decoded image assets must expose their dimensions and exact ARGB pixels");

    const VectorAsset *decodedShape = findVectorAsset(decoded.document, "vector-static");
    expect(decodedShape
               && decodedShape->viewport.width == 4
               && decodedShape->viewport.height == 4
               && decodedShape->paths.size() == 1,
           "decoded shape assets must expose their viewport and ordered paths");
    if (decodedShape && decodedShape->paths.size() == 1) {
        const VectorPath &path = decodedShape->paths.front();
        expect(path.commands.size() == 5
                   && std::holds_alternative<MoveTo>(path.commands[0])
                   && std::holds_alternative<LineTo>(path.commands[1])
                   && std::holds_alternative<QuadraticTo>(path.commands[2])
                   && std::holds_alternative<CubicTo>(path.commands[3])
                   && std::holds_alternative<ClosePath>(path.commands[4]),
               "decoded shapes must expose every native path command in paint order");
        const auto *quadratic = std::get_if<QuadraticTo>(&path.commands[2]);
        const auto *cubic = std::get_if<CubicTo>(&path.commands[3]);
        expect(quadratic
                   && quadratic->control.x == 4.0
                   && quadratic->control.y == 2.0
                   && quadratic->end.x == 4.0
                   && quadratic->end.y == 4.0
                   && cubic
                   && cubic->control1.x == 3.0
                   && cubic->control2.x == 1.0
                   && cubic->end.y == 4.0,
               "decoded shapes must expose control points and endpoints without flattening");
        expect(path.fill
                   && path.fill->argb == 0x80ffcc00U
                   && path.stroke
                   && path.stroke->paint.argb == 0xff010203U
                   && path.stroke->width == 1.25,
               "decoded shapes must expose fill and stroke details");
    }

    const Layer *decodedLayer = findLayer(decoded.document, "static-raster");
    const LayerProperties *decodedProperties = decodedLayer
        ? &layerProperties(*decodedLayer)
        : nullptr;
    expect(decodedLayer
               && std::holds_alternative<BitmapLayer>(*decodedLayer)
               && decodedProperties
               && decodedProperties->name == "Static raster / 한글"
               && !decodedProperties->visible
               && decodedProperties->opacity == 0.375
               && decodedProperties->transform.m11 == 0.75
               && decodedProperties->transform.m12 == 0.25
               && decodedProperties->transform.m21 == -0.5
               && decodedProperties->transform.m22 == 1.25
               && decodedProperties->transform.translationX == 1.5
               && decodedProperties->transform.translationY == -0.25
               && decodedProperties->blendMode == RasterBlendMode::Screen,
           "decoded layers must expose identity, visibility, opacity, transform, and blend mode");
    const StaticSource *decodedStaticSource = decodedLayer
        ? std::get_if<StaticSource>(&layerSource(*decodedLayer))
        : nullptr;
    expect(decodedStaticSource && decodedStaticSource->assetId == "raster/static",
           "decoded layers must expose their stable asset reference");
    expect(decodedProperties
               && decodedProperties->frameRange
               && decodedProperties->frameRange->firstFrame == 0
               && decodedProperties->frameRange->lastFrame == 1
               && layerProperties(decoded.document.layers[1]).frameRange
               && layerProperties(decoded.document.layers[1])
                       .frameRange->firstFrame == 1
               && layerProperties(decoded.document.layers[1])
                       .frameRange->lastFrame == 2,
           "format 1.3 must preserve inclusive frame ranges for bitmap and vector layers");

    const Layer *decodedAnimatedLayer = findLayer(decoded.document, "a-animated-vector");
    const KeyframedSource *decodedTrack = decodedAnimatedLayer
        ? std::get_if<KeyframedSource>(&layerSource(*decodedAnimatedLayer))
        : nullptr;
    expect(decodedTrack
               && decodedAnimatedLayer
               && std::holds_alternative<VectorLayer>(*decodedAnimatedLayer)
               && contentKind(*decodedAnimatedLayer) == ContentKind::Vector
               && decoded.document.frames.size() == 3
               && decoded.document.frames[0].index == 0
               && decoded.document.frames[0].keyframes.size() == 2
               && decoded.document.frames[0].keyframes[0].layerId
                   == "a-animated-vector"
               && decoded.document.frames[0].keyframes[0].assetId
                   == "vector-frame-0"
               && decoded.document.frames[0].keyframes[1].layerId
                   == "z-animated-raster"
               && decoded.document.frames[0].keyframes[1].assetId
                   == "raster-frame-0"
               && decoded.document.frames[1].index == 1
               && decoded.document.frames[1].keyframes.size() == 1
               && decoded.document.frames[1].keyframes[0].layerId
                   == "a-animated-vector"
               && decoded.document.frames[1].keyframes[0].assetId
                   == "vector-frame-1"
               && decoded.document.frames[2].index == 2
               && decoded.document.frames[2].keyframes.size() == 1
               && decoded.document.frames[2].keyframes[0].layerId
                   == "z-animated-raster"
               && decoded.document.frames[2].keyframes[0].assetId
                   == "raster-frame-2",
           "the decoder must transpose non-lexicographic layer-major records into lexicographically ordered direct frame ownership");
    const auto *decodedVectorSource = decodedAnimatedLayer
        ? std::get_if<KeyframedSource>(&layerSource(*decodedAnimatedLayer))
        : nullptr;
    const Layer *decodedRasterLayer = findLayer(decoded.document,
                                                 "z-animated-raster");
    const auto *decodedRasterSource = decodedRasterLayer
        ? std::get_if<KeyframedSource>(&layerSource(*decodedRasterLayer))
        : nullptr;
    expect(decodedVectorSource
               && decodedVectorSource->frameIndices
                   == std::vector<FrameIndex>{0, 1}
               && decodedRasterSource
               && decodedRasterSource->frameIndices
                   == std::vector<FrameIndex>{0, 2},
           "each decoded keyframed source must retain its validated derived frame index");

    const IiscEncodeResult reencoded = encodeIisc(decoded.document);
    expect(reencoded.ok() && reencoded.bytes == encoded.bytes,
           "encoding must be canonical and byte-identical after round-trip");
    Document unbounded = original;
    layerProperties(unbounded.layers.front()).frameRange.reset();
    const IiscEncodeResult encodedUnbounded = encodeIisc(unbounded);
    const IiscDecodeResult decodedUnbounded = decodeIisc(encodedUnbounded.bytes);
    expect(encodedUnbounded.ok()
               && decodedUnbounded.ok()
               && !layerProperties(decodedUnbounded.document.layers.front())
                       .frameRange
               && encodeIisc(decodedUnbounded.document).bytes
                   == encodedUnbounded.bytes,
           "an absent format 1.3 frame range must round-trip canonically as an unbounded layer");
    Document documentWithoutGenerationMetadata = original;
    documentWithoutGenerationMetadata.stableDiffusionMetadata.reset();
    for (FrameIndex frame = 0; frame < original.timeline.frameCount; ++frame) {
        const FrameRenderResult originalFrame = renderFrame(original, frame);
        const FrameRenderResult decodedFrame = renderFrame(decoded.document, frame);
        const FrameRenderResult metadataFreeFrame =
            renderFrame(documentWithoutGenerationMetadata, frame);
        expect(originalFrame.ok() && decodedFrame.ok()
                   && metadataFreeFrame.ok()
                   && originalFrame.pixels.width == decodedFrame.pixels.width
                   && originalFrame.pixels.height == decodedFrame.pixels.height
                   && originalFrame.pixels.pixels == decodedFrame.pixels.pixels
                   && originalFrame.pixels.pixels == metadataFreeFrame.pixels.pixels,
               "generation metadata and serialization must not change rendered pixels at any timeline frame");
    }

    Document legacyVersion12 = original;
    legacyVersion12.formatVersion = {1, 2};
    clearFrameRanges(legacyVersion12);
    const IiscEncodeResult encodedVersion12 = encodeIisc(legacyVersion12);
    const IiscDecodeResult decodedVersion12 = decodeIisc(encodedVersion12.bytes);
    const IiscEncodeResult reencodedVersion12 =
        encodeIisc(decodedVersion12.document);
    expect(encodedVersion12.ok() && decodedVersion12.ok()
               && decodedVersion12.document.formatVersion.major == 1
               && decodedVersion12.document.formatVersion.minor == 2
               && decodedVersion12.document.stableDiffusionMetadata
                   == legacyVersion12.stableDiffusionMetadata
               && !layerProperties(decodedVersion12.document.layers.front())
                       .frameRange
               && reencodedVersion12.ok()
               && reencodedVersion12.bytes == encodedVersion12.bytes,
           "the 1.3 reader must preserve canonical range-free 1.2 containers byte-for-byte");

    Document nonRepresentableVersion12 = original;
    nonRepresentableVersion12.formatVersion = {1, 2};
    expect(encodeIisc(nonRepresentableVersion12).error.code
               == IiscErrorCode::InvalidDocument,
           "a writer must not silently discard layer ranges requested in format 1.2");

    Document legacyVersion11 = original;
    legacyVersion11.formatVersion = {1, 1};
    legacyVersion11.stableDiffusionMetadata.reset();
    clearFrameRanges(legacyVersion11);
    const IiscEncodeResult encodedVersion11 = encodeIisc(legacyVersion11);
    const IiscDecodeResult decodedVersion11 = decodeIisc(encodedVersion11.bytes);
    const IiscEncodeResult reencodedVersion11 = encodeIisc(decodedVersion11.document);
    expect(encodedVersion11.ok() && decodedVersion11.ok()
               && decodedVersion11.document.formatVersion.major == 1
               && decodedVersion11.document.formatVersion.minor == 1
               && !decodedVersion11.document.stableDiffusionMetadata
               && keyframedSource(decodedVersion11.document,
                                  "a-animated-vector")
               && keyframedSource(decodedVersion11.document,
                                  "a-animated-vector")->frameIndices
                   == std::vector<FrameIndex>{0, 1}
               && reencodedVersion11.ok()
               && reencodedVersion11.bytes == encodedVersion11.bytes,
           "the 1.3 reader must preserve canonical metadata-free 1.1 containers byte-for-byte");

    Document legacyVersion10 = original;
    legacyVersion10.formatVersion = {1, 0};
    legacyVersion10.stableDiffusionMetadata.reset();
    clearFrameRanges(legacyVersion10);
    const IiscEncodeResult encodedVersion10 = encodeIisc(legacyVersion10);
    const IiscDecodeResult decodedVersion10 = decodeIisc(encodedVersion10.bytes);
    const IiscEncodeResult reencodedVersion10 =
        encodeIisc(decodedVersion10.document);
    expect(encodedVersion10.ok() && decodedVersion10.ok()
               && decodedVersion10.document.formatVersion.major == 1
               && decodedVersion10.document.formatVersion.minor == 0
               && decodedVersion10.document.frames.size() == 3
               && decodedVersion10.document.frames.front().keyframes.size() == 2
               && keyframedSource(decodedVersion10.document,
                                  "z-animated-raster")
               && keyframedSource(decodedVersion10.document,
                                  "z-animated-raster")->frameIndices
                   == std::vector<FrameIndex>{0, 2}
               && reencodedVersion10.ok()
               && reencodedVersion10.bytes == encodedVersion10.bytes,
           "the current reader must transpose and preserve canonical 1.0 keyframes byte-for-byte");

    Document invalid = original;
    invalid.extent.width = 0;
    expect(encodeIisc(invalid).error.code == IiscErrorCode::InvalidDocument,
           "the writer must reject invalid documents before producing bytes");

    Document invalidUtf8 = original;
    layerProperties(invalidUtf8.layers.front()).name = std::string{"\xc0\xaf", 2};
    expect(encodeIisc(invalidUtf8).error.code == IiscErrorCode::InvalidDocument,
           "the writer must reject non-canonical UTF-8 document strings");

    std::vector<std::uint8_t> badMagic = encoded.bytes;
    badMagic[0] ^= 0xffU;
    expect(decodeIisc(badMagic).error.code == IiscErrorCode::InvalidHeader,
           "a container with the wrong magic must fail closed");

    std::vector<std::uint8_t> futureVersion = encoded.bytes;
    futureVersion[8] = static_cast<std::uint8_t>(CurrentFormatMajor + 1);
    futureVersion[9] = 0;
    expect(decodeIisc(futureVersion).error.code == IiscErrorCode::UnsupportedVersion,
           "a future .iisc major version must fail closed before payload parsing");

    std::vector<std::uint8_t> futureMinor = encoded.bytes;
    futureMinor[10] = static_cast<std::uint8_t>(CurrentFormatMinor + 1);
    futureMinor[11] = 0;
    expect(decodeIisc(futureMinor).error.code == IiscErrorCode::UnsupportedVersion,
           "an unknown future minor version must require an explicit reader migration");

    std::vector<std::uint8_t> truncated = encoded.bytes;
    truncated.pop_back();
    expect(decodeIisc(truncated).error.code == IiscErrorCode::TruncatedData,
           "a truncated payload must fail before allocation or document exposure");

    const Document rangeDocument = simpleRangeDocument();
    const IiscEncodeResult encodedRange = encodeIisc(rangeDocument);
    expect(encodedRange.ok() && encodedRange.bytes.size() >= IiscHeaderSize + 10,
           "the format 1.3 range fixture must encode a layer range before the metadata presence byte");
    Document invalidRangeDocument = rangeDocument;
    layerProperties(invalidRangeDocument.layers.front()).frameRange->lastFrame =
        invalidRangeDocument.timeline.frameCount;
    expect(encodeIisc(invalidRangeDocument).error.code
               == IiscErrorCode::InvalidDocument,
           "the writer must enforce timeline bounds before serializing a layer range");
    if (encodedRange.ok() && encodedRange.bytes.size() >= IiscHeaderSize + 10) {
        const std::size_t rangeOffset = encodedRange.bytes.size() - 10;

        std::vector<std::uint8_t> invalidRangePresence = encodedRange.bytes;
        invalidRangePresence[rangeOffset] = 2U;
        refreshPayloadChecksum(invalidRangePresence);
        expect(decodeIisc(invalidRangePresence).error.code
                   == IiscErrorCode::InvalidData,
               "a non-canonical frame-range presence byte must fail closed");

        std::vector<std::uint8_t> descendingRange = encodedRange.bytes;
        overwriteU32(descendingRange, rangeOffset + 1, 3);
        overwriteU32(descendingRange, rangeOffset + 5, 2);
        refreshPayloadChecksum(descendingRange);
        expect(decodeIisc(descendingRange).error.code
                   == IiscErrorCode::InvalidData,
               "a serialized layer range with firstFrame after lastFrame must fail closed");

        std::vector<std::uint8_t> outOfTimelineRange = encodedRange.bytes;
        overwriteU32(outOfTimelineRange, rangeOffset + 5, 4);
        refreshPayloadChecksum(outOfTimelineRange);
        expect(decodeIisc(outOfTimelineRange).error.code
                   == IiscErrorCode::InvalidData,
               "serialized layer-range bounds must remain within the timeline frame limit");

        std::vector<std::uint8_t> truncatedRange = encodedRange.bytes;
        truncatePayload(truncatedRange, 2);
        expect(decodeIisc(truncatedRange).error.code
                   == IiscErrorCode::TruncatedData,
               "a truncated format 1.3 layer range must fail before document exposure");
    }

    std::vector<std::uint8_t> corrupt = encoded.bytes;
    corrupt[IiscHeaderSize] ^= 0x01U;
    expect(decodeIisc(corrupt).error.code == IiscErrorCode::ChecksumMismatch,
           "payload corruption must be detected by the container checksum");

    std::vector<std::uint8_t> unknownAssetKind = encoded.bytes;
    constexpr std::size_t firstVersion11AssetKindOffset = IiscHeaderSize + 25;
    unknownAssetKind[firstVersion11AssetKindOffset] = 0xffU;
    refreshPayloadChecksum(unknownAssetKind);
    expect(decodeIisc(unknownAssetKind).error.code == IiscErrorCode::InvalidData,
           "unknown canonical payload tags must fail closed after checksum verification");

    std::vector<std::uint8_t> invalidUtf8Payload = encoded.bytes;
    const std::string knownId = "raster/static";
    const auto idPosition = std::search(invalidUtf8Payload.begin() + IiscHeaderSize,
                                        invalidUtf8Payload.end(),
                                        knownId.begin(),
                                        knownId.end());
    expect(idPosition != invalidUtf8Payload.end(),
           "test fixture must contain its canonical UTF-8 asset id");
    if (idPosition != invalidUtf8Payload.end()) {
        *idPosition = 0xc0U;
        *(idPosition + 1) = 0xafU;
        refreshPayloadChecksum(invalidUtf8Payload);
        expect(decodeIisc(invalidUtf8Payload).error.code == IiscErrorCode::InvalidData,
               "the reader must reject non-canonical UTF-8 before exposing a document");
    }

    std::vector<std::uint8_t> descendingLayerKeyframes = encoded.bytes;
    const std::string rasterFrameZero = "raster-frame-0";
    const std::string rasterFrameTwo = "raster-frame-2";
    const auto rasterFrameZeroPosition = std::find_end(
        descendingLayerKeyframes.begin() + IiscHeaderSize,
        descendingLayerKeyframes.end(),
        rasterFrameZero.begin(), rasterFrameZero.end());
    const auto rasterFrameTwoPosition = std::find_end(
        descendingLayerKeyframes.begin() + IiscHeaderSize,
        descendingLayerKeyframes.end(),
        rasterFrameTwo.begin(), rasterFrameTwo.end());
    expect(rasterFrameZeroPosition != descendingLayerKeyframes.end()
               && rasterFrameTwoPosition != descendingLayerKeyframes.end()
               && std::distance(descendingLayerKeyframes.begin(),
                                rasterFrameZeroPosition) >= 8
               && std::distance(descendingLayerKeyframes.begin(),
                                rasterFrameTwoPosition) >= 8,
           "test fixture must expose both layer-major raster keyframe records");
    if (rasterFrameZeroPosition != descendingLayerKeyframes.end()
        && rasterFrameTwoPosition != descendingLayerKeyframes.end()) {
        overwriteU32(descendingLayerKeyframes,
                     static_cast<std::size_t>(std::distance(
                         descendingLayerKeyframes.begin(),
                         rasterFrameZeroPosition)) - 8,
                     2);
        overwriteU32(descendingLayerKeyframes,
                     static_cast<std::size_t>(std::distance(
                         descendingLayerKeyframes.begin(),
                         rasterFrameTwoPosition)) - 8,
                     0);
        refreshPayloadChecksum(descendingLayerKeyframes);
        expect(decodeIisc(descendingLayerKeyframes).error.code
                   == IiscErrorCode::InvalidData,
               "the reader must reject descending layer-major keyframes instead of normalizing non-canonical bytes into frames");
    }

    std::vector<std::uint8_t> trailing = encoded.bytes;
    trailing.push_back(0U);
    expect(decodeIisc(trailing).error.code == IiscErrorCode::TrailingData,
           "canonical containers must reject unaccounted trailing bytes");

    SerializationLimits tinyLimits;
    tinyLimits.maximumContainerBytes = encoded.bytes.size() - 1;
    expect(decodeIisc(encoded.bytes, tinyLimits).error.code == IiscErrorCode::LimitExceeded,
           "container size limits must be enforced before parsing");

    SerializationLimits assetLimits;
    assetLimits.maximumAssets = 1;
    expect(decodeIisc(encoded.bytes, assetLimits).error.code == IiscErrorCode::LimitExceeded,
           "declared collection limits must be enforced before collection allocation");

    SerializationLimits frameLimits;
    expect(frameLimits.maximumFrames == 262144U,
           "the default sparse owned-frame limit must remain explicit and bounded");
    frameLimits.maximumFrames = 2;
    expect(encodeIisc(original, frameLimits).error.code
               == IiscErrorCode::LimitExceeded
               && decodeIisc(encoded.bytes, frameLimits).error.code
                   == IiscErrorCode::LimitExceeded,
           "sparse owned-frame limits must be enforced symmetrically before materialization");

    SerializationLimits keyframeLimits;
    keyframeLimits.maximumTotalKeyframes = 3;
    expect(encodeIisc(original, keyframeLimits).error.code
               == IiscErrorCode::LimitExceeded
               && decodeIisc(encoded.bytes, keyframeLimits).error.code
                   == IiscErrorCode::LimitExceeded,
           "frame-owned keyframe limits must be enforced symmetrically by writer and reader");

    Document keyframeOwnershipDocument;
    keyframeOwnershipDocument.extent = {1, 1};
    keyframeOwnershipDocument.timeline = {{24, 1}, 2};
    keyframeOwnershipDocument.assets.emplace_back(
        RasterAsset{"a", makeRasterLayer(1, 1, 0xff000000U)});
    keyframeOwnershipDocument.assets.emplace_back(
        RasterAsset{"b", makeRasterLayer(1, 1, 0xffffffffU)});
    const std::string longLayerId(64, 'l');
    keyframeOwnershipDocument.layers.emplace_back(BitmapLayer{
        {longLayerId, "", true, 1.0, {}, RasterBlendMode::SourceOver},
        KeyframedSource{{0, 1}},
    });
    keyframeOwnershipDocument.frames = {
        {0, {{longLayerId, "a"}}},
        {1, {{longLayerId, "b"}}},
    };
    const IiscEncodeResult encodedOwnershipDocument =
        encodeIisc(keyframeOwnershipDocument);
    SerializationLimits ownedStringLimits;
    ownedStringLimits.maximumTotalStringBytes = 100;
    expect(encodedOwnershipDocument.ok()
               && encodeIisc(keyframeOwnershipDocument, ownedStringLimits)
                       .error.code == IiscErrorCode::LimitExceeded
               && decodeIisc(encodedOwnershipDocument.bytes, ownedStringLimits)
                       .error.code == IiscErrorCode::LimitExceeded,
           "materialized frame-owned layer ids must share the symmetric string allocation limit");

    SerializationLimits metadataLimits;
    metadataLimits.maximumMetadataEntries = 1;
    expect(encodeIisc(original, metadataLimits).error.code
               == IiscErrorCode::LimitExceeded
               && decodeIisc(encoded.bytes, metadataLimits).error.code
                   == IiscErrorCode::LimitExceeded,
           "generation metadata collection limits must be enforced by writer and reader");

    SerializationLimits metadataStringLimits;
    metadataStringLimits.maximumMetadataStringBytes = 8;
    expect(encodeIisc(original, metadataStringLimits).error.code
               == IiscErrorCode::LimitExceeded
               && decodeIisc(encoded.bytes, metadataStringLimits).error.code
                   == IiscErrorCode::LimitExceeded,
           "generation metadata string limits must be enforced by writer and reader");

    metadataStringLimits.maximumMetadataStringBytes = 0;
    expect(encodeIisc(original, metadataStringLimits).error.code
               == IiscErrorCode::LimitExceeded
               && decodeIisc(encoded.bytes, metadataStringLimits).error.code
                   == IiscErrorCode::LimitExceeded,
           "a zero generation metadata string limit must reject non-empty values symmetrically");

    return failures == 0 ? 0 : 1;
}
