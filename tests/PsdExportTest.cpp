#include <iiSharedCanvas.h>

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool value, const std::string &message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
bool warns(const MediaIoResult &result, const std::string &part)
{
    return std::any_of(result.warnings.begin(), result.warnings.end(), [&](const auto &value) {
        return value.find(part) != std::string::npos;
    });
}
std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size()) { expect(false, "fixture parser bounds"); return 0; }
    return (std::uint16_t(bytes[offset]) << 8) | bytes[offset + 1];
}
std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return (std::uint32_t(u16(bytes, offset)) << 16) | u16(bytes, offset + 2);
}
std::string key(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size()) { expect(false, "fixture key bounds"); return {}; }
    return {reinterpret_cast<const char *>(bytes.data() + offset), 4};
}
struct Record {
    std::int32_t top, left, bottom, right;
    std::string blend;
    std::uint8_t opacity, flags;
    std::vector<std::string> tags;
    std::span<const std::uint8_t> placed;
    std::span<const std::uint8_t> smart;
};
struct Structure {
    std::vector<Record> records;
    std::vector<std::string> globalTags;
    std::span<const std::uint8_t> linkedData;
};
Structure inspect(std::span<const std::uint8_t> bytes)
{
    Structure result;
    expect(bytes.size() >= 38 && key(bytes, 0) == "8BPS" && u16(bytes, 4) == 1
           && u16(bytes, 12) == 4 && u16(bytes, 22) == 8 && u16(bytes, 24) == 3,
           "PSD v1 RGB8 header has merged transparency");
    if (bytes.size() < 38) { return result; }
    std::size_t section = 30 + u32(bytes, 26);
    section += 4 + u32(bytes, section);
    const auto sectionEnd = section + 4 + u32(bytes, section);
    auto info = section + 4;
    const auto infoEnd = info + 4 + u32(bytes, info);
    const auto signedCount = std::int16_t(u16(bytes, info + 4));
    expect(signedCount < 0, "negative PSD layer count identifies merged transparency");
    const auto count = unsigned(-int(signedCount));
    std::size_t cursor = info + 6;
    std::uint64_t channelBytes = 0;
    for (unsigned index = 0; index < count && cursor < infoEnd; ++index) {
        Record record{std::int32_t(u32(bytes, cursor)), std::int32_t(u32(bytes, cursor + 4)),
                      std::int32_t(u32(bytes, cursor + 8)), std::int32_t(u32(bytes, cursor + 12)), {}, 0, 0, {}};
        cursor += 16;
        const auto channels = u16(bytes, cursor); cursor += 2;
        expect(channels == 4, "each exported PSD layer has explicit RGB plus alpha");
        for (unsigned channel = 0; channel < channels; ++channel) {
            channelBytes += u32(bytes, cursor + 2); cursor += 6;
        }
        expect(key(bytes, cursor) == "8BIM", "PSD blend signature");
        record.blend = key(bytes, cursor + 4); record.opacity = bytes[cursor + 8]; record.flags = bytes[cursor + 10];
        cursor += 12;
        const auto extraEnd = cursor + 4 + u32(bytes, cursor); cursor += 4;
        cursor += 4 + u32(bytes, cursor); cursor += 4 + u32(bytes, cursor);
        const auto nameBytes = std::size_t(bytes[cursor]) + 1; cursor += (nameBytes + 3) & ~std::size_t(3);
        while (cursor + 12 <= extraEnd) {
            expect(key(bytes, cursor) == "8BIM", "PSD additional-info signature");
            const auto tag = key(bytes, cursor + 4); const auto size = u32(bytes, cursor + 8);
            expect(size % 4 == 0, "PSD layer tag length includes its alignment padding, with no bytes before the next signature");
            record.tags.push_back(tag);
            if (cursor + 12 + size <= bytes.size()) {
                if (tag == "PlLd") { record.placed = bytes.subspan(cursor + 12, size); }
                if (tag == "SoLd") { record.smart = bytes.subspan(cursor + 12, size); }
            }
            cursor += 12 + size;
        }
        expect(cursor == extraEnd, "PSD layer extra sections are length-delimited");
        result.records.push_back(std::move(record));
    }
    expect(cursor + channelBytes <= infoEnd && infoEnd - cursor - channelBytes <= 1,
           "PSD declared channel lengths exactly fit the layer-info section");
    cursor = infoEnd;
    cursor += 4 + u32(bytes, cursor);
    while (cursor + 12 <= sectionEnd) {
        expect(key(bytes, cursor) == "8BIM", "PSD global additional-info signature");
        const auto name = key(bytes, cursor + 4); const auto size = u32(bytes, cursor + 8);
        result.globalTags.push_back(name);
        if (name == "lnk2" && cursor + 12 + size <= bytes.size()) { result.linkedData = bytes.subspan(cursor + 12, size); }
        cursor += 12 + ((size + 3) & ~std::size_t(3));
    }
    expect(cursor == sectionEnd, "PSD global layer data is bounded and padded");
    return result;
}

Document bitmapDocument()
{
    Document document; document.extent = {4, 2};
    document.assets.emplace_back(RasterAsset{"base", makeRasterLayer(4, 2, 0xff223344U)});
    RasterLayer pixels = makeRasterLayer(2, 1); pixels.pixels = {0xffee4422U, 0x00123456U};
    document.assets.emplace_back(RasterAsset{"top", std::move(pixels)});
    BitmapLayer base; base.properties.id = "base-layer"; base.properties.name = "Background";
    base.source = StaticSource{"base"}; document.layers.emplace_back(std::move(base));
    BitmapLayer top; top.properties.id = "top-layer"; top.properties.name = "한글🌈";
    top.properties.transform.translationX = 1; top.source = StaticSource{"top"};
    document.layers.emplace_back(std::move(top));
    return document;
}

void rejects(const Document &document, const PsdExportOptions &options, MediaIoCode code, const std::string &message)
{
    const auto encoded = encodePsd(document, options);
    expect(encoded.result.code == code && encoded.bytes.empty(), message + ": " + encoded.result.message);
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    auto document = bitmapDocument();
    const auto original = encodeIisc(document).bytes;
    auto exported = encodePsd(document);
    expect(exported.ok(), "encode layered bitmap PSD: " + exported.result.message);
    if (exported.ok()) {
        const auto binary = inspect(exported.bytes);
        expect(binary.records.size() == 2 && binary.records[0].left == 0 && binary.records[1].left == 1,
               "binary records preserve bottom-to-top order and bitmap integer offsets");
        const auto decoded = decodeLayeredDocument(exported.bytes);
        expect(decoded.ok() && decoded.document.layers.size() == 2, "exported plain bitmap PSD reimports as layers");
        if (decoded.ok()) {
            expect(layerProperties(decoded.document.layers[1]).name == "한글🌈", "Unicode PSD layer names round-trip");
            expect(std::get<RasterAsset>(decoded.document.assets[1]).pixels.pixels
                   == std::get<RasterAsset>(document.assets[1]).pixels.pixels, "simple bitmap PSD preserves transparent RGB");
            const auto expected = renderFrame(document, 0), actual = renderFrame(decoded.document, 0);
            expect(expected.ok() && actual.ok() && expected.pixels.pixels == actual.pixels.pixels,
                   "bitmap PSD native renderer golden remains unchanged");
        }
    }
    expect(encodeIisc(document).bytes == original, "PSD encoding does not mutate source document");

    auto hidden = document; auto &hiddenProperties = layerProperties(hidden.layers[1]);
    hiddenProperties.visible = false; hiddenProperties.opacity = 128.0 / 255; hiddenProperties.blendMode = RasterBlendMode::Multiply;
    auto hiddenPsd = encodePsd(hidden); auto hiddenDecoded = decodeLayeredDocument(hiddenPsd.bytes);
    expect(hiddenPsd.ok() && hiddenDecoded.ok(), "hidden PSD layer export round-trip");
    if (hiddenDecoded.ok()) {
        const auto &properties = layerProperties(hiddenDecoded.document.layers[1]);
        expect(!properties.visible && std::abs(properties.opacity - 128.0 / 255) < 1e-12
               && properties.blendMode == RasterBlendMode::Multiply
               && std::get<RasterAsset>(hiddenDecoded.document.assets[1]).pixels.pixels
                    == std::get<RasterAsset>(hidden.assets[1]).pixels.pixels,
               "hidden layer retains unbaked pixels, opacity and blend metadata");
    }
    auto animated = document; animated.timeline.frameCount = 2;
    animated.assets.emplace_back(RasterAsset{"later", makeRasterLayer(2, 1, 0xff00ff00U)});
    layerSource(animated.layers[1]) = KeyframedSource{{0, 1}};
    animated.frames = {{0, {{"top-layer", "top"}}}, {1, {{"top-layer", "later"}}}};
    auto animatedPsd = encodePsd(animated);
    auto animatedDecoded = decodeLayeredDocument(animatedPsd.bytes);
    expect(animatedPsd.ok() && warns(animatedPsd.result, "frame 0") && animatedDecoded.ok(),
           "timeline export explicitly samples only frame 0");
    if (animatedDecoded.ok()) {
        expect(std::get<RasterAsset>(animatedDecoded.document.assets[1]).pixels.pixels
               == std::get<RasterAsset>(document.assets[1]).pixels.pixels, "later keyframe pixels do not leak into PSD");
    }
    layerProperties(animated.layers[1]).frameRange = LayerFrameRange{1, 1};
    const auto ranged = encodePsd(animated); const auto rangedDecoded = decodeLayeredDocument(ranged.bytes);
    expect(ranged.ok() && warns(ranged.result, "frame 0") && rangedDecoded.ok() && rangedDecoded.document.layers.size() == 1,
           "layers outside frame 0 are excluded and disclosed");
    layerProperties(animated.layers[0]).frameRange = LayerFrameRange{1, 1};
    const auto emptyFrame = encodePsd(animated); const auto emptyDecoded = decodeLayeredDocument(emptyFrame.bytes);
    expect(emptyFrame.ok() && warns(emptyFrame.result, "placeholder") && emptyDecoded.ok()
           && emptyDecoded.document.layers.size() == 1 && renderFrame(emptyDecoded.document, 0).pixels.pixels
                == std::vector<std::uint32_t>(8, 0),
           "a valid empty frame 0 exports a disclosed transparent placeholder without later layers");

    auto transformed = document; auto &transform = layerProperties(transformed.layers[1]).transform;
    transform.m11 = 1.5; transform.translationX = 0.25;
    const auto baked = encodePsd(transformed); const auto bakedDecoded = decodeLayeredDocument(baked.bytes);
    expect(baked.ok() && warns(baked.result, "baked") && bakedDecoded.ok(), "arbitrary bitmap transforms use disclosed viewport caches");
    if (bakedDecoded.ok()) {
        expect(renderFrame(transformed, 0).pixels.pixels == renderFrame(bakedDecoded.document, 0).pixels.pixels,
               "baked bitmap transform golden preserves visible rendering");
    }

    Document vectorDocument; vectorDocument.extent = {12, 10};
    VectorAsset vector; vector.id = "vector"; vector.viewport = {2, 2};
    VectorPath vectorPath; vectorPath.commands = {MoveTo{{-1, -2}}, LineTo{{5, -2}}, LineTo{{5, 3}}, LineTo{{-1, 3}}, ClosePath{}};
    vectorPath.fill = SolidPaint{0xffcc6622}; vectorPath.stroke = StrokeStyle{SolidPaint{0xff2233cc}, 2}; vector.paths.push_back(vectorPath);
    vectorDocument.assets.emplace_back(vector);
    VectorLayer vectorLayer; vectorLayer.properties.id = "native-vector-layer"; vectorLayer.properties.name = "Editable vector";
    vectorLayer.properties.opacity = 128.0 / 255; vectorLayer.source = StaticSource{"vector"};
    vectorLayer.properties.transform = {1.5, 0.25, -0.5, 1, 3, 4}; vectorDocument.layers.emplace_back(vectorLayer);
    const auto vectorOriginal = encodeIisc(vectorDocument).bytes;
    const auto smartPsd = encodePsd(vectorDocument);
    expect(smartPsd.ok() && warns(smartPsd.result, "PDF Smart Objects"), "native vectors export as embedded PDF Smart Objects");
    if (smartPsd.ok()) {
        const auto structure = inspect(smartPsd.bytes);
        expect(structure.records.size() == 1 && !structure.records[0].placed.empty()
               && !structure.records[0].smart.empty() && !structure.linkedData.empty(),
               "vector PSD has linked payload and both placed/smart descriptors, not only a raster cache");
        if (structure.records.size() == 1 && structure.records[0].placed.size() > 125 && structure.linkedData.size() > 30) {
            const auto &placed = structure.records[0].placed;
            expect(key(placed, 0) == "plcL" && u32(placed, 4) == 3
                   && key(structure.records[0].smart, 0) == "soLD" && u32(structure.records[0].smart, 4) == 4,
                   "placed/smart object envelopes use documented signatures and versions");
            const std::array<std::uint8_t, 16> mediaBoxCrop{0, 0, 0, 0, 'C', 'r', 'o', 'p', 'l', 'o', 'n', 'g', 0, 0, 0, 3};
            const auto smart = structure.records[0].smart;
            expect(std::search(smart.begin(), smart.end(), mediaBoxCrop.begin(), mediaBoxCrop.end()) != smart.end(),
                   "PDF Smart Object explicitly uses MediaBox cropping to retain full source-page geometry");
            const auto uuidSize = placed[8];
            const std::string placedUuid(reinterpret_cast<const char *>(placed.data() + 9), uuidSize);
            const auto quadOffset = 9 + uuidSize + 16;
            const std::array<double, 8> expectedQuad{1.5, 0.5, 13.5, 2.5, 10, 9.5, -2, 7.5};
            for (std::size_t index = 0; index < expectedQuad.size(); ++index) {
                const auto offset = quadOffset + index * 8;
                const auto bits = (std::uint64_t(u32(placed, offset)) << 32) | u32(placed, offset + 4);
                expect(std::abs(std::bit_cast<double>(bits) - expectedQuad[index]) < 1e-12,
                       "placed quad preserves full off-viewport path/stroke bounds and original affine transform");
            }
            const auto linked = structure.linkedData;
            expect(key(linked, 8) == "liFD" && u32(linked, 12) == 2, "Smart Object data is embedded, not an external filename reference");
            const auto linkedUuidLength = linked[16];
            const std::string linkedUuid(reinterpret_cast<const char *>(linked.data() + 17), linkedUuidLength);
            expect(linkedUuid == placedUuid, "placed and embedded payload UUIDs match");
            std::size_t payload = 17 + linkedUuidLength;
            const auto filenameUnits = u32(linked, payload);
            QString filename;
            for (std::uint32_t index = 0; index < filenameUnits; ++index) { filename.append(QChar(u16(linked, payload + 4 + index * 2))); }
            expect(filename == QStringLiteral("vector-0.pdf") + QChar(0),
                   "linked PDF filename includes its terminal UTF-16 null so Photoshop retains the full .pdf extension");
            payload += 4 + 2 * filenameUnits;
            expect(key(linked, payload) == "PDF ", "embedded payload type is PDF");
            const auto payloadSize = (std::uint64_t(u32(linked, payload + 8)) << 32) | u32(linked, payload + 12);
            payload += 17;
            expect(payload + payloadSize <= linked.size(), "embedded PDF length fits its linked-data envelope");
            if (payload + payloadSize <= linked.size()) {
                const QByteArray pdf(reinterpret_cast<const char *>(linked.data() + payload), qsizetype(payloadSize));
                expect(pdf.startsWith("%PDF-") && !pdf.contains("/Subtype /Image"),
                       "embedded source is a vector PDF, not a raster wrapped in a PDF");
                const auto mediaBox = QRegularExpression(R"(/MediaBox\s*\[\s*0\s+0\s+([\d.]+)\s+([\d.]+)\s*\])").match(QString::fromLatin1(pdf));
                expect(mediaBox.hasMatch() && mediaBox.captured(1).toDouble() == 8 && mediaBox.captured(2).toDouble() == 7,
                       "PDF page includes complete path bounds plus stroke outside native viewport");
            }
        }
        expect(decodeLayeredDocument(smartPsd.bytes).result.code == MediaIoCode::UnsupportedFeature,
               "strict PSD importer does not pretend editable PDF Smart Objects became native raster-only layers");
    }
    expect(encodeIisc(vectorDocument).bytes == vectorOriginal, "Smart Object export leaves source vector paths and timeline unchanged");
    auto commandLimit = PsdExportOptions{}; commandLimit.limits.maxVectorCommands = 1;
    rejects(vectorDocument, commandLimit, MediaIoCode::LimitExceeded, "Smart Object vector command preflight");
    auto pdfMemoryLimit = PsdExportOptions{};
    // Cached render scratch plus one small record/ID reserve fits; the PDF
    // must share the same cap rather than receiving another full budget.
    pdfMemoryLimit.limits.maxDecodedBytes = 12 * 10 * 4 * 8 + 5 * sizeof(PathCommand) + 5 * 128
        + 4 * std::string("Editable vector").size() + 1024;
    rejects(vectorDocument, pdfMemoryLimit, MediaIoCode::LimitExceeded,
            "embedded PDFs and cached rendering share one aggregate decoded-memory budget");
    auto hugePdf = vectorDocument; std::get<VectorAsset>(hugePdf.assets[0]).viewport.width = 14401;
    rejects(hugePdf, {}, MediaIoCode::UnsupportedFeature, "unsupported PDF source dimensions fail without silently clipping paths");
    auto hiddenVector = vectorDocument; layerProperties(hiddenVector.layers[0]).visible = false;
    const auto hiddenSmart = encodePsd(hiddenVector);
    expect(hiddenSmart.ok() && inspect(hiddenSmart.bytes).records[0].flags == 2,
           "hidden vectors still retain embedded editable Smart Objects and cached pixels");

    auto invalidName = document; layerProperties(invalidName.layers[0]).name = std::string("bad\0name", 8);
    rejects(invalidName, {}, MediaIoCode::InvalidArgument, "embedded null name is rejected");
    layerProperties(invalidName.layers[0]).name = std::string("\xff", 1);
    rejects(invalidName, {}, MediaIoCode::InvalidArgument, "invalid UTF-8 name is rejected");
    auto erase = document; layerProperties(erase.layers[0]).blendMode = RasterBlendMode::DestinationOut;
    rejects(erase, {}, MediaIoCode::InvalidArgument, "unsupported native/PSD blend fails instead of changing its appearance");
    auto limited = PsdExportOptions{}; limited.maxLayers = 1;
    rejects(document, limited, MediaIoCode::LimitExceeded, "PSD layer-count preflight");
    limited = {}; limited.limits.maxPixelsPerFrame = 1;
    rejects(document, limited, MediaIoCode::LimitExceeded, "PSD pixel preflight");
    limited = {}; limited.limits.maxDecodedBytes = 16;
    rejects(document, limited, MediaIoCode::LimitExceeded, "PSD decoded working memory preflight");
    limited = {}; limited.limits.maxOutputBytes = 32;
    rejects(document, limited, MediaIoCode::LimitExceeded, "PSD output is bounded and atomic in memory");
    auto huge = document; huge.extent = {30001, 1};
    rejects(huge, {}, MediaIoCode::UnsupportedFeature, "PSD v1 canvas dimensions fail without creating PSB");
    Document manyLayers; manyLayers.extent = {1, 1}; manyLayers.assets.emplace_back(RasterAsset{"shared", makeRasterLayer(1, 1)});
    for (int index = 0; index < 1024; ++index) {
        BitmapLayer layer; layer.properties.id = "layer-" + std::to_string(index); layer.source = StaticSource{"shared"};
        manyLayers.layers.emplace_back(std::move(layer));
    }
    limited = {}; limited.limits.maxDecodedBytes = 64;
    rejects(manyLayers, limited, MediaIoCode::LimitExceeded, "layer records are bounded before allocating a large tiny-pixel layer list");
    Document tiled; tiled.extent = {256, 256}; tiled.canvasMode = CanvasMode::Infinite; tiled.infiniteCanvas.chunkSize = 32;
    ChunkedRasterAsset chunks; chunks.id = "chunks";
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 8; ++column) { chunks.chunks.push_back({column, row, makeRasterLayer(32, 32, 0xffff0000)}); }
    }
    tiled.assets.emplace_back(std::move(chunks)); BitmapLayer tilesLayer;
    tilesLayer.properties.id = "tile-layer"; tilesLayer.source = StaticSource{"chunks"}; tiled.layers.emplace_back(tilesLayer);
    limited = {}; limited.limits.maxDecodedBytes = 3 * 1024 * 1024;
    rejects(tiled, limited, MediaIoCode::LimitExceeded, "native per-chunk full-canvas render pieces are included in the memory preflight");

    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/psd-export-XXXXXX"));
    const auto path = directory.filePath("native layers 한글.psd").toStdString();
    const auto written = exportPsd(document, path);
    expect(written.ok() && importLayeredDocument(path).ok(), "atomic path export writes a readable PSD");
    expect(exportPsd(hidden, path).code == MediaIoCode::AlreadyExists, "default PSD export does not overwrite");
    expect(importLayeredDocument(path).ok() && layerProperties(importLayeredDocument(path).document.layers[1]).visible,
           "failed overwrite preserves previous bytes");
    PsdExportOptions overwrite; overwrite.overwrite = true;
    expect(exportPsd(hidden, path, overwrite).ok() && !layerProperties(importLayeredDocument(path).document.layers[1]).visible,
           "explicit PSD replacement is atomic");
    expect(exportPsd(document, directory.filePath("working.iisc").toStdString(), overwrite).code == MediaIoCode::InvalidArgument,
           "PSD output refuses to replace native iisc working files");
    expect(exportPsd(vectorDocument, std::string(IISHAREDCANVAS_TEST_OUTPUT_DIR) + "/export-smartobject.psd", overwrite).ok(),
           "write persistent Smart Object fixture for independent producer/consumer validation");
    return failures ? 1 : 0;
}
