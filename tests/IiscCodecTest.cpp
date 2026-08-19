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
    document.layers.push_back({"static-raster", "Static raster / 한글", false, 0.375,
                               transformed, RasterBlendMode::Screen,
                               StaticSource{"raster/static"}});
    document.layers.push_back({"static-vector", "Static vector", true, 0.875, {},
                               RasterBlendMode::Multiply, StaticSource{"vector-static"}});
    document.layers.push_back({"animated-raster", "Animated raster", true, 1.0, {},
                               RasterBlendMode::SourceOver,
                               KeyframedSource{ContentKind::Raster,
                                               {{0, "raster-frame-0"}, {2, "raster-frame-2"}}}});
    document.layers.push_back({"animated-vector", "Animated vector", true, 0.5, {},
                               RasterBlendMode::Overlay,
                               KeyframedSource{ContentKind::Vector,
                                               {{0, "vector-frame-0"}, {1, "vector-frame-1"}}}});
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
    expect(decoded.document.layers.front().name == "Static raster / 한글"
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
    expect(decodedLayer
               && decodedLayer->name == "Static raster / 한글"
               && !decodedLayer->visible
               && decodedLayer->opacity == 0.375
               && decodedLayer->transform.m11 == 0.75
               && decodedLayer->transform.m12 == 0.25
               && decodedLayer->transform.m21 == -0.5
               && decodedLayer->transform.m22 == 1.25
               && decodedLayer->transform.translationX == 1.5
               && decodedLayer->transform.translationY == -0.25
               && decodedLayer->blendMode == RasterBlendMode::Screen,
           "decoded layers must expose identity, visibility, opacity, transform, and blend mode");
    const StaticSource *decodedStaticSource = decodedLayer
        ? std::get_if<StaticSource>(&decodedLayer->source)
        : nullptr;
    expect(decodedStaticSource && decodedStaticSource->assetId == "raster/static",
           "decoded layers must expose their stable asset reference");

    const Layer *decodedAnimatedLayer = findLayer(decoded.document, "animated-vector");
    const KeyframedSource *decodedTrack = decodedAnimatedLayer
        ? std::get_if<KeyframedSource>(&decodedAnimatedLayer->source)
        : nullptr;
    expect(decodedTrack
               && decodedTrack->kind == ContentKind::Vector
               && decodedTrack->keyframes.size() == 2
               && decodedTrack->keyframes[0].frame == 0
               && decodedTrack->keyframes[0].assetId == "vector-frame-0"
               && decodedTrack->keyframes[1].frame == 1
               && decodedTrack->keyframes[1].assetId == "vector-frame-1",
           "decoded animated layers must expose content kind and every keyframe reference");

    const IiscEncodeResult reencoded = encodeIisc(decoded.document);
    expect(reencoded.ok() && reencoded.bytes == encoded.bytes,
           "encoding must be canonical and byte-identical after round-trip");
    for (FrameIndex frame = 0; frame < original.timeline.frameCount; ++frame) {
        const FrameRenderResult originalFrame = renderFrame(original, frame);
        const FrameRenderResult decodedFrame = renderFrame(decoded.document, frame);
        expect(originalFrame.ok() && decodedFrame.ok()
                   && originalFrame.pixels.width == decodedFrame.pixels.width
                   && originalFrame.pixels.height == decodedFrame.pixels.height
                   && originalFrame.pixels.pixels == decodedFrame.pixels.pixels,
               "serialization must preserve exact rendered pixels at every timeline frame");
    }

    Document invalid = original;
    invalid.extent.width = 0;
    expect(encodeIisc(invalid).error.code == IiscErrorCode::InvalidDocument,
           "the writer must reject invalid documents before producing bytes");

    Document invalidUtf8 = original;
    invalidUtf8.layers.front().name = std::string{"\xc0\xaf", 2};
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
    unknownAssetKind[IiscHeaderSize + 24] = 0xffU;
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

    return failures == 0 ? 0 : 1;
}
