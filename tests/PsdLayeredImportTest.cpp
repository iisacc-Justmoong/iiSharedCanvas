#include <iiSharedCanvas.h>
#include <Layered/LayeredDocumentCodec.h>

#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QTemporaryDir>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool condition, const std::string &message)
{
    if (!condition) { std::cerr << message << '\n'; ++failures; }
}
void u16(Bytes &out, std::uint16_t value)
{
    out.push_back(std::uint8_t(value >> 8)); out.push_back(std::uint8_t(value));
}
void u32(Bytes &out, std::uint32_t value)
{
    u16(out, std::uint16_t(value >> 16)); u16(out, std::uint16_t(value));
}
void text(Bytes &out, const std::string &value) { out.insert(out.end(), value.begin(), value.end()); }
void append(Bytes &out, const Bytes &value) { out.insert(out.end(), value.begin(), value.end()); }
void block(Bytes &out, const Bytes &value) { u32(out, std::uint32_t(value.size())); append(out, value); }
std::uint32_t read32(const Bytes &bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16)
        | (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}
std::size_t firstChannelOffset(const Bytes &bytes)
{
    // These fixtures use empty image resources. One layer record begins at 44:
    // rectangle + four descriptors + blend/flags = 54 bytes before extra size.
    constexpr std::size_t extraSizeOffset = 44 + 16 + 2 + 24 + 12;
    return extraSizeOffset + 4 + read32(bytes, extraSizeOffset);
}
Bytes tagged(const std::string &key, Bytes payload)
{
    Bytes out; text(out, "8BIM"); text(out, key); block(out, payload);
    if (payload.size() & 1U) { out.push_back(0); }
    return out;
}
Bytes imageResource(std::uint16_t id, const Bytes &payload)
{
    Bytes out; text(out, "8BIM"); u16(out, id); u16(out, 0); block(out, payload);
    if (payload.size() & 1U) { out.push_back(0); }
    return out;
}

struct LayerFixture {
    int x = 0, y = 0, width = 2, height = 1;
    std::string name = "layer";
    std::vector<std::uint32_t> pixels = {0x80112233U, 0x00445566U};
    std::string blend = "norm";
    std::uint8_t opacity = 255, flags = 0, clipping = 0;
    std::uint16_t compression = 0;
    Bytes extra;
    Bytes mask;
    Bytes ranges;
    bool alpha = true;
};

Bytes channel(Bytes plane, int width, int height, std::uint16_t compression)
{
    Bytes out; u16(out, compression);
    if (compression == 0) { append(out, plane); }
    else if (compression == 1) {
        for (int row = 0; row < height; ++row) { u16(out, std::uint16_t(width + 1)); }
        for (int row = 0; row < height; ++row) {
            out.push_back(std::uint8_t(width - 1));
            out.insert(out.end(), plane.begin() + row * width, plane.begin() + (row + 1) * width);
        }
    } else {
        if (compression == 3) {
            for (int row = 0; row < height; ++row) {
                for (int col = width - 1; col > 0; --col) {
                    const auto index = std::size_t(row * width + col);
                    plane[index] = std::uint8_t(plane[index] - plane[index - 1]);
                }
            }
        }
        uLongf size = compressBound(uLong(plane.size()));
        Bytes compressed(size);
        expect(compress2(compressed.data(), &size, plane.data(), uLong(plane.size()), Z_BEST_SPEED) == Z_OK,
               "fixture ZIP compression");
        compressed.resize(size); append(out, compressed);
    }
    return out;
}

Bytes psd(const std::vector<LayerFixture> &layers, std::uint16_t version = 1,
          std::uint16_t depth = 8, std::uint16_t mode = 3, Bytes resources = {})
{
    Bytes records, pixels;
    for (const auto &layer : layers) {
        u32(records, std::uint32_t(layer.y)); u32(records, std::uint32_t(layer.x));
        u32(records, std::uint32_t(layer.y + layer.height)); u32(records, std::uint32_t(layer.x + layer.width));
        u16(records, layer.alpha ? 4 : 3);
        for (const int id : {0, 1, 2, -1}) {
            if (id == -1 && !layer.alpha) { continue; }
            Bytes plane;
            for (const auto pixel : layer.pixels) {
                const int shift = id == -1 ? 24 : 16 - 8 * id;
                plane.push_back(std::uint8_t(pixel >> shift));
            }
            const auto encoded = channel(std::move(plane), layer.width, layer.height, layer.compression);
            u16(records, std::uint16_t(id)); u32(records, std::uint32_t(encoded.size())); append(pixels, encoded);
        }
        text(records, "8BIM"); text(records, layer.blend);
        records.push_back(layer.opacity); records.push_back(layer.clipping);
        records.push_back(layer.flags); records.push_back(0);
        Bytes extra; block(extra, layer.mask); block(extra, layer.ranges);
        extra.push_back(std::uint8_t(layer.name.size())); text(extra, layer.name);
        while (extra.size() % 4) { extra.push_back(0); }
        append(extra, layer.extra); block(records, extra);
    }
    Bytes info; u16(info, std::uint16_t(layers.size())); append(info, records); append(info, pixels);
    if (info.size() & 1U) { info.push_back(0); }
    Bytes layerAndMask; block(layerAndMask, info); u32(layerAndMask, 0);
    Bytes out; text(out, "8BPS"); u16(out, version);
    out.insert(out.end(), 6, 0); u16(out, 3); u32(out, 2); u32(out, 4); u16(out, depth); u16(out, mode);
    u32(out, 0); block(out, resources); block(out, layerAndMask);
    u16(out, 0); out.insert(out.end(), 24, 0); // Independent, intentionally different merged preview.
    return out;
}

void rejects(const Bytes &bytes, MediaIoCode code, const std::string &message,
             const LayeredDocumentImportOptions &options = {})
{
    const auto result = decodeLayeredDocument(bytes, options);
    expect(!result.ok() && result.result.code == code && result.document.layers.empty()
           && result.document.assets.empty(), message + ": " + result.result.message);
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    LayerFixture top; top.name = "legacy"; top.x = 1; top.y = -1; top.opacity = 128; top.flags = 2;
    Bytes unicode; u32(unicode, 4); u16(unicode, 0xd55c); u16(unicode, 0xae00); u16(unicode, 0xd83c); u16(unicode, 0xdf08);
    top.extra = tagged("luni", unicode);
    LayerFixture bottom; bottom.name = "bottom"; bottom.pixels = {0xff0000ffU, 0xff00ff00U};
    LayeredDocumentImportOptions named; named.idPrefix = "psd-test";
    // PSD binary records, unlike an OpenRaster stack, are bottom-to-top.
    const auto encoded = psd({bottom, top});
    const auto imported = decodeLayeredDocument(encoded, named);
    expect(imported.ok(), "two editable PSD layers import: " + imported.result.message);
    if (imported.ok()) {
        const auto &document = imported.document;
        expect(imported.format == "psd" && document.layers.size() == 2 && document.assets.size() == 2
               && document.extent.width == 4 && document.extent.height == 2 && validate(document).ok(),
               "PSD produces valid detached finite iisc document");
        const auto &first = layerProperties(document.layers[0]);
        const auto &last = layerProperties(document.layers[1]);
        expect(first.name == "bottom" && last.name == "한글🌈", "PSD bottom-to-top order and UTF-16 surrogate names");
        expect(!last.visible && std::abs(last.opacity - 128.0 / 255.0) < 1e-12
               && last.transform.translationX == 1 && last.transform.translationY == -1,
               "PSD visibility, opacity and signed offsets remain layer properties");
        expect(first.id.starts_with("psd-test") && last.id != first.id,
               "generated stable IDs use caller prefix and remain unique");
        const auto *asset = findRasterAsset(document, std::get<StaticSource>(layerSource(document.layers[1])).assetId);
        expect(asset && asset->pixels.pixels == top.pixels, "straight ARGB includes fully transparent RGB values");
        const auto stored = encodeIisc(document);
        const auto restored = decodeIisc(stored.bytes);
        expect(stored.ok() && restored.ok() && encodeIisc(restored.document).bytes == stored.bytes,
               "PSD editable document is deterministic through native iisc serialization");
        auto rendered = renderFrame(document, 0);
        expect(rendered.ok() && rendered.pixels.pixels[0] == 0xff0000ffU
               && rendered.pixels.pixels[1] == 0xff00ff00U,
               "render uses real layers and visibility, never deliberately black PSD composite");
    }

    for (std::uint16_t compression = 0; compression <= 3; ++compression) {
        LayerFixture fixture; fixture.width = 3; fixture.height = 2; fixture.compression = compression;
        fixture.pixels = {0x80112233U, 0x00445566U, 0xff778899U, 0xffaabbccU, 0x00ddeeffU, 0x7ffefdfcU};
        const auto result = decodeLayeredDocument(psd({fixture}));
        expect(result.ok(), "PSD compression " + std::to_string(compression) + ": " + result.result.message);
        if (result.ok()) {
            expect(std::get<RasterAsset>(result.document.assets[0]).pixels.pixels == fixture.pixels,
                   "PSD compression preserves channel bytes and predictor row resets");
        }
    }
    const std::array<std::pair<std::string, RasterBlendMode>, 4> modes{{
        {"norm", RasterBlendMode::SourceOver}, {"mul ", RasterBlendMode::Multiply},
        {"scrn", RasterBlendMode::Screen}, {"over", RasterBlendMode::Overlay},
    }};
    for (const auto &[key, mode] : modes) {
        auto fixture = bottom; fixture.blend = key; fixture.alpha = false;
        const auto result = decodeLayeredDocument(psd({fixture}));
        expect(result.ok() && layerProperties(result.document.layers[0]).blendMode == mode,
               "PSD blend mode " + key);
    }

    for (const std::string key : {"lsct", "lsdk", "TySh", "vmsk", "SoLd", "lrFX", "zzzz"}) {
        auto fixture = bottom; fixture.extra = tagged(key, Bytes(4, 0));
        rejects(psd({fixture}), MediaIoCode::UnsupportedFeature, "unsupported PSD semantics " + key);
    }
    auto unsupported = bottom; unsupported.mask = Bytes(20, 0);
    rejects(psd({unsupported}), MediaIoCode::UnsupportedFeature, "layer mask is not silently discarded");
    unsupported = bottom; unsupported.clipping = 1;
    rejects(psd({unsupported}), MediaIoCode::UnsupportedFeature, "clipping layer is not silently discarded");
    unsupported = bottom; unsupported.blend = "diff";
    rejects(psd({unsupported}), MediaIoCode::UnsupportedFeature, "unknown blend is not changed to source-over");
    unsupported = bottom; unsupported.ranges = Bytes(8, 0);
    rejects(psd({unsupported}), MediaIoCode::UnsupportedFeature, "non-default blending ranges are not ignored");
    unsupported = bottom; unsupported.flags = 24;
    rejects(psd({unsupported}), MediaIoCode::UnsupportedFeature, "non-pixel semantic layer is not rendered as cached pixels");
    rejects(psd({bottom}, 2), MediaIoCode::UnsupportedFeature, "PSB does not enter PSD v1 parser");
    rejects(psd({bottom}, 1, 16), MediaIoCode::UnsupportedFeature, "16-bit PSD fails closed");
    rejects(psd({bottom}, 1, 8, 4), MediaIoCode::UnsupportedFeature, "CMYK PSD fails closed");
    rejects(psd({}), MediaIoCode::UnsupportedFeature, "composite-only PSD has no fake editable layers");

    auto auxiliaryChannel = psd({bottom}); auxiliaryChannel[13] = 4;
    auxiliaryChannel.insert(auxiliaryChannel.end(), 8, 0x7f);
    rejects(auxiliaryChannel, MediaIoCode::UnsupportedFeature,
            "positive layer count cannot silently discard a fourth auxiliary document channel");
    auto missingMergedAlpha = psd({bottom}); missingMergedAlpha[42] = 0xff; missingMergedAlpha[43] = 0xff;
    rejects(missingMergedAlpha, MediaIoCode::InvalidData,
            "negative layer count requires a merged transparency channel in the header");
    auto mergedTransparency = auxiliaryChannel; mergedTransparency[42] = 0xff; mergedTransparency[43] = 0xff;
    const auto transparentMerged = decodeLayeredDocument(mergedTransparency);
    expect(transparentMerged.ok() && transparentMerged.document.layers.size() == 1
           && std::get<RasterAsset>(transparentMerged.document.assets[0]).pixels.pixels == bottom.pixels,
           "negative layer count and four document channels preserve layers without baking merged transparency");

    rejects(psd({bottom}, 1, 8, 3, imageResource(1041, {1})), MediaIoCode::UnsupportedFeature,
            "intentionally untagged PSD must not silently assume sRGB");
    expect(decodeLayeredDocument(psd({bottom}, 1, 8, 3, imageResource(1041, {0}))).ok(),
           "disabled untagged-profile flag permits the documented default sRGB interpretation");
    for (const Bytes malformed : {Bytes{}, Bytes{2}, Bytes{0, 0}}) {
        rejects(psd({bottom}, 1, 8, 3, imageResource(1041, malformed)), MediaIoCode::InvalidData,
                "ICC untagged flag requires one canonical Boolean byte");
    }
    const auto srgbProfile = QColorSpace(QColorSpace::SRgb).iccProfile();
    Bytes taggedSrgb = imageResource(1039, Bytes(srgbProfile.begin(), srgbProfile.end()));
    expect(decodeLayeredDocument(psd({bottom}, 1, 8, 3, taggedSrgb)).ok(), "explicit sRGB PSD is supported");
    append(taggedSrgb, imageResource(1041, {1}));
    rejects(psd({bottom}, 1, 8, 3, taggedSrgb), MediaIoCode::UnsupportedFeature,
            "explicit sRGB profile cannot override the intentionally-untagged flag");

    auto limited = named; limited.maxLayers = 1;
    rejects(encoded, MediaIoCode::LimitExceeded, "layer count bound", limited);
    limited = named; limited.limits.maxPixelsPerFrame = 1;
    rejects(encoded, MediaIoCode::LimitExceeded, "pixel bound before allocation", limited);
    limited = named; limited.limits.maxDecodedBytes = 15;
    rejects(encoded, MediaIoCode::LimitExceeded, "aggregate raster and channel scratch bound", limited);
    limited = named; limited.limits.maxInputBytes = encoded.size() - 1;
    rejects(encoded, MediaIoCode::LimitExceeded, "encoded input bound", limited);

    for (std::size_t size = 4; size < encoded.size() - 26; ++size) {
        const auto result = decodeLayeredDocument(std::span(encoded).first(size));
        expect(!result.ok() && result.document.layers.empty() && result.document.assets.empty(),
               "every truncated layer/resource prefix fails atomically at " + std::to_string(size));
    }
    auto corrupt = encoded; corrupt[42] = 0x7f; corrupt[43] = 0xff;
    rejects(corrupt, MediaIoCode::LimitExceeded, "malicious layer count is bounded");
    corrupt = psd({bottom}); corrupt[68] = 0; corrupt[69] = 0;
    rejects(corrupt, MediaIoCode::InvalidData, "duplicate channel IDs fail before pixel allocation");
    corrupt = psd({bottom}); corrupt[46] = 0x7f; corrupt[47] = 0xff;
    rejects(corrupt, MediaIoCode::InvalidData, "inverted signed layer rectangle is rejected");
    auto badName = bottom; Bytes unmatched; u32(unmatched, 1); u16(unmatched, 0xd800);
    badName.extra = tagged("luni", unmatched);
    rejects(psd({badName}), MediaIoCode::InvalidData, "unmatched Unicode surrogate is rejected");
    auto metadata = bottom; Bytes identifier; u32(identifier, 42); metadata.extra = tagged("lyid", identifier);
    const auto withMetadata = decodeLayeredDocument(psd({metadata}));
    expect(withMetadata.ok() && !withMetadata.result.warnings.empty(), "discarded editor metadata is disclosed");
    auto packed = bottom; packed.compression = 1;
    corrupt = psd({packed}); corrupt[firstChannelOffset(corrupt) + 4] = 127;
    rejects(corrupt, MediaIoCode::InvalidData, "RLE literal cannot write beyond one row");
    corrupt = psd({packed}); corrupt[firstChannelOffset(corrupt) + 4] = 255;
    rejects(corrupt, MediaIoCode::InvalidData, "RLE trailing literal cannot extend a complete row");
    auto zipped = bottom; zipped.compression = 2;
    corrupt = psd({zipped}); corrupt[firstChannelOffset(corrupt) + 2] ^= 0xff;
    rejects(corrupt, MediaIoCode::InvalidData, "ZIP checksum/header corruption fails closed");
    auto noPreview = encoded; noPreview.resize(noPreview.size() - 26);
    expect(decodeLayeredDocument(noPreview).ok(), "PSD layers work when optional merged preview is omitted");
    corrupt = encoded; corrupt.pop_back();
    rejects(corrupt, MediaIoCode::InvalidData, "a present but truncated merged preview is invalid");
    for (std::uint16_t compression = 1; compression <= 3; ++compression) {
        auto compressedPreview = noPreview;
        if (compression == 1) {
            u16(compressedPreview, 1);
            for (int row = 0; row < 6; ++row) { u16(compressedPreview, 2); }
            for (int row = 0; row < 6; ++row) { compressedPreview.push_back(253); compressedPreview.push_back(0); }
        } else { append(compressedPreview, channel(Bytes(24, 0), 4, 6, compression)); }
        expect(decodeLayeredDocument(compressedPreview).ok(), "bounded merged compression validation " + std::to_string(compression));
        compressedPreview.pop_back();
        rejects(compressedPreview, MediaIoCode::InvalidData, "truncated compressed merged preview is invalid");
    }

    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/psd-layer-import-XXXXXX"));
    const auto path = directory.filePath("레이어 with spaces.dat");
    QFile file(path);
    expect(file.open(QIODevice::WriteOnly)
           && file.write(reinterpret_cast<const char *>(encoded.data()), qint64(encoded.size())) == qint64(encoded.size()),
           "create independent PSD file fixture");
    file.close();
    const auto fromFile = importLayeredDocument(path.toStdString(), named);
    expect(fromFile.ok() && fromFile.format == "psd" && fromFile.document.layers.size() == 2,
           "content detection imports a layered file argument despite extension");
    return failures ? 1 : 0;
}
