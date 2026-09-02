#include <iiSharedCanvas.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
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
        {"animated-raster", "Animated raster", true, 1.0, {},
         RasterBlendMode::SourceOver},
        KeyframedSource{{{0, "raster-frame-0"}, {2, "raster-frame-2"}}},
    });
    document.layers.emplace_back(VectorLayer{
        {"animated-vector", "Animated vector", true, 0.5, {}, RasterBlendMode::Overlay},
        KeyframedSource{{{0, "vector-frame-0"}, {1, "vector-frame-1"}}},
    });

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
    generation.automatic1111Parameters =
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

} // namespace

int main()
{
    using namespace iiSharedCanvas;

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
    expect(decoded.document.formatVersion.minor == 2
               && decoded.document.stableDiffusionMetadata
               && original.stableDiffusionMetadata
               && decoded.document.stableDiffusionMetadata
                   == original.stableDiffusionMetadata,
           "format 1.2 must round-trip typed generation settings and raw ComfyUI metadata exactly");
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

    const Layer *decodedAnimatedLayer = findLayer(decoded.document, "animated-vector");
    const KeyframedSource *decodedTrack = decodedAnimatedLayer
        ? std::get_if<KeyframedSource>(&layerSource(*decodedAnimatedLayer))
        : nullptr;
    expect(decodedTrack
               && decodedAnimatedLayer
               && std::holds_alternative<VectorLayer>(*decodedAnimatedLayer)
               && contentKind(*decodedAnimatedLayer) == ContentKind::Vector
               && decodedTrack->keyframes.size() == 2
               && decodedTrack->keyframes[0].frame == 0
               && decodedTrack->keyframes[0].assetId == "vector-frame-0"
               && decodedTrack->keyframes[1].frame == 1
               && decodedTrack->keyframes[1].assetId == "vector-frame-1",
           "decoded animated layers must preserve concrete layer type and every keyframe reference");

    const IiscEncodeResult reencoded = encodeIisc(decoded.document);
    expect(reencoded.ok() && reencoded.bytes == encoded.bytes,
           "encoding must be canonical and byte-identical after round-trip");
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

    Document legacyVersion11 = original;
    legacyVersion11.formatVersion = {1, 1};
    legacyVersion11.stableDiffusionMetadata.reset();
    const IiscEncodeResult encodedVersion11 = encodeIisc(legacyVersion11);
    const IiscDecodeResult decodedVersion11 = decodeIisc(encodedVersion11.bytes);
    const IiscEncodeResult reencodedVersion11 = encodeIisc(decodedVersion11.document);
    expect(encodedVersion11.ok() && decodedVersion11.ok()
               && decodedVersion11.document.formatVersion.major == 1
               && decodedVersion11.document.formatVersion.minor == 1
               && !decodedVersion11.document.stableDiffusionMetadata
               && reencodedVersion11.ok()
               && reencodedVersion11.bytes == encodedVersion11.bytes,
           "the 1.2 reader must preserve canonical metadata-free 1.1 containers byte-for-byte");

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
