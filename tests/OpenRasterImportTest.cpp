#include <iiSharedCanvas.h>
#include <Layered/LayeredDocumentCodec.h>

#include <QByteArray>
#include <QGuiApplication>

#include <zip.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, const std::string &message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}

struct Entry {
    std::string name;
    std::vector<std::uint8_t> data;
    bool symlink = false;
    bool encrypted = false;
    zip_int32_t compression = ZIP_CM_STORE;
};

std::vector<std::uint8_t> bytes(const std::string &text)
{
    return {text.begin(), text.end()};
}

std::vector<std::uint8_t> archive(const std::vector<Entry> &entries)
{
    zip_error_t error;
    zip_error_init(&error);
    auto *source = zip_source_buffer_create(nullptr, 0, 0, &error);
    auto *zip = source ? zip_open_from_source(source, ZIP_TRUNCATE, &error) : nullptr;
    if (!zip) {
        expect(false, "create independent libzip fixture");
        if (source) { zip_source_free(source); }
        zip_error_fini(&error);
        return {};
    }
    zip_source_keep(source);
    bool good = true;
    for (const auto &entry : entries) {
        auto *item = zip_source_buffer(zip, entry.data.data(), entry.data.size(), 0);
        const auto index = item ? zip_file_add(zip, entry.name.c_str(), item, ZIP_FL_ENC_UTF_8) : -1;
        if (index < 0) {
            if (item) { zip_source_free(item); }
            good = false;
            break;
        }
        good = zip_set_file_compression(zip, zip_uint64_t(index), entry.compression, 0) == 0 && good;
        if (entry.symlink) {
            good = zip_file_set_external_attributes(zip, zip_uint64_t(index), 0,
                                                    ZIP_OPSYS_UNIX, 0120777U << 16) == 0 && good;
        }
        if (entry.encrypted) {
            good = zip_file_set_encryption(zip, zip_uint64_t(index), ZIP_EM_AES_256, "fixture") == 0 && good;
        }
    }
    if (!good || zip_close(zip) != 0) {
        expect(false, "finish independent libzip fixture");
        zip_discard(zip);
        zip_source_free(source);
        zip_error_fini(&error);
        return {};
    }
    zip_stat_t stat;
    zip_stat_init(&stat);
    good = zip_source_stat(source, &stat) == 0 && zip_source_open(source) == 0;
    std::vector<std::uint8_t> result(good ? std::size_t(stat.size) : 0);
    good = good && zip_source_read(source, result.data(), result.size()) == zip_int64_t(result.size());
    expect(good, "read in-memory archive fixture");
    zip_source_close(source);
    zip_source_free(source);
    zip_error_fini(&error);
    return result;
}

std::vector<Entry> fixture(const std::string &xml)
{
    using namespace iiSharedCanvas;
    BitmapExportOptions png;
    png.extendedCodecs = false;
    return {{"mimetype", bytes("image/openraster")}, {"stack.xml", bytes(xml)},
            {"data/top.png", encodeBitmap(makeRasterLayer(2, 2, 0xffff0000U), png).bytes},
            {"data/bot.png", encodeBitmap(makeRasterLayer(3, 2, 0xff0000ffU), png).bytes}};
}

std::string image(const std::string &children)
{
    return "<image version='0.0.6' w='3' h='2'><stack>" + children + "</stack></image>";
}

std::uint32_t little32(const std::vector<std::uint8_t> &data, std::size_t offset)
{
    return std::uint32_t(data[offset]) | (std::uint32_t(data[offset + 1]) << 8)
        | (std::uint32_t(data[offset + 2]) << 16) | (std::uint32_t(data[offset + 3]) << 24);
}

std::size_t centralRecordSize(const std::vector<std::uint8_t> &data, std::size_t offset)
{
    const auto little16 = [&](std::size_t field) {
        return std::size_t(data[offset + field]) | (std::size_t(data[offset + field + 1]) << 8);
    };
    return 46 + little16(28) + little16(30) + little16(32);
}

void expectRejected(const std::vector<std::uint8_t> &data,
                    const std::string &message,
                    const iiSharedCanvas::LayeredDocumentImportOptions &options = {})
{
    const auto result = iiSharedCanvas::decodeLayeredDocument(data, options);
    expect(!result.ok() && result.document.layers.empty() && result.document.assets.empty(), message);
}
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    using namespace iiSharedCanvas;
    const auto xml = image("<layer name='Top 한글' src='data/top.png' x='1' y='0'/>"
                           "<layer name='Bottom' src='data/bot.png'/>");
    const auto input = archive(fixture(xml));
    LayeredDocumentImportOptions options;
    options.idPrefix = "ora";
    const auto imported = decodeLayeredDocument(input, options);
    expect(imported.ok(), "decode flat OpenRaster: " + imported.result.message);
    if (imported.ok()) {
        expect(imported.format == "ora" && imported.document.layers.size() == 2
               && imported.document.assets.size() == 2 && validate(imported.document).ok(),
               "OpenRaster returns a valid detached two-layer document");
        const auto &bottom = layerProperties(imported.document.layers[0]);
        const auto &top = layerProperties(imported.document.layers[1]);
        expect(bottom.id == "ora-layer-0" && bottom.name == "Bottom"
               && top.name == "Top 한글" && top.transform.translationX == 1,
               "OpenRaster top-first order becomes bottom-first with layer names and offsets");
        expect(std::get<StaticSource>(layerSource(imported.document.layers[0])).assetId == "ora-asset-0",
               "layer source IDs match deterministic imported asset IDs");
        const auto rendered = renderFrame(imported.document, 0);
        expect(rendered.ok() && rendered.pixels.pixels
               == std::vector<std::uint32_t>{0xff0000ffU, 0xffff0000U, 0xffff0000U,
                                            0xff0000ffU, 0xffff0000U, 0xffff0000U},
               "mixed layer ordering and offset render to exact independent golden pixels");
        const auto encoded = encodeIisc(imported.document);
        const auto restored = decodeIisc(encoded.bytes);
        expect(encoded.ok() && restored.ok() && restored.document.layers.size() == 2
               && renderFrame(restored.document, 0).pixels.pixels == rendered.pixels.pixels,
               "foreign editable layers survive the native iisc serialization round trip");
    }

    const auto properties = decodeLayeredDocument(archive(fixture(image(
        "<layer name='Hidden' src='data/top.png' x='-1' y='-2' opacity='0.25' visibility='hidden' composite-op='svg:multiply'/>"))));
    expect(properties.ok() && !layerProperties(properties.document.layers[0]).visible
           && layerProperties(properties.document.layers[0]).opacity == 0.25
           && layerProperties(properties.document.layers[0]).transform.translationX == -1
           && layerProperties(properties.document.layers[0]).transform.translationY == -2
           && layerProperties(properties.document.layers[0]).blendMode == RasterBlendMode::Multiply,
           "visibility, fractional opacity, signed offsets, and multiply mode are preserved");
    for (const auto &[name, mode] : std::vector<std::pair<std::string, RasterBlendMode>>{
             {"svg:src-over", RasterBlendMode::SourceOver}, {"svg:screen", RasterBlendMode::Screen},
             {"svg:overlay", RasterBlendMode::Overlay}}) {
        const auto result = decodeLayeredDocument(archive(fixture(image(
            "<layer src='data/top.png' composite-op='" + name + "'/>"))));
        expect(result.ok() && layerProperties(result.document.layers[0]).blendMode == mode,
               "preserve supported OpenRaster blend mode " + name);
    }

    const auto group = decodeLayeredDocument(archive(fixture(image(
        "<stack isolation='auto' name='Folder'><layer src='data/top.png'/></stack><layer src='data/bot.png'/>"))));
    expect(group.ok() && group.document.layers.size() == 2 && !group.result.warnings.empty(),
           "neutral non-isolated groups flatten only with an explicit hierarchy-loss warning");
    const auto legacyGroup = decodeLayeredDocument(archive(fixture(
        "<image version='0.0.5' w='3' h='2'><stack><stack isolation='auto' x='100' y='-100'>"
        "<layer src='data/top.png' x='1' y='2'/></stack></stack></image>")));
    expect(legacyGroup.ok() && layerProperties(legacyGroup.document.layers[0]).transform.translationX == 1
           && layerProperties(legacyGroup.document.layers[0]).transform.translationY == 2,
           "legacy OpenRaster group coordinates are ignored, never added to child offsets");
    const auto attributedRoot = decodeLayeredDocument(archive(fixture(
        "<image version='0.0.6' w='3' h='2'><stack name='Root' isolation='auto' "
        "composite-op='svg:multiply' opacity='0.1' visibility='hidden'>"
        "<layer src='data/bot.png'/></stack></image>")));
    expect(attributedRoot.ok() && !attributedRoot.result.warnings.empty()
           && layerProperties(attributedRoot.document.layers[0]).opacity == 1.0
           && layerProperties(attributedRoot.document.layers[0]).visible
           && renderFrame(attributedRoot.document, 0).pixels.pixels == std::vector<std::uint32_t>(6, 0xff0000ffU),
           "known redundant root stack attributes are ignored with warning; root rendering is fixed");
    for (const std::string groupAttributes : {"", "isolation='isolate'", "isolation='auto' opacity='0.5'",
                                              "isolation='auto' x='10'"}) {
        expectRejected(archive(fixture(image("<stack " + groupAttributes
            + "><layer src='data/top.png'/></stack>"))), "unsupported group compositing is never silently flattened");
    }
    for (const std::string malformedLayer : {
             "<layer src='../outside.png'/>", "<layer src='https://example.com/layer.png'/>",
             "<layer src='missing.png'/>", "<layer src='data/top.png' opacity='nan'/>",
             "<layer src='data/top.png' opacity='1.1'/>", "<layer src='data/top.png' x='2147483648'/>",
             "<layer src='data/top.png' visibility='maybe'/>", "<layer src='data/top.png' composite-op='svg:darken'/>",
             "<layer src='data/top.png' composite-op='svg:dst-out'/>",
             "<layer src='data/top.png' clipping='true'/>", "<text>unsupported</text>",
             "<layer src='data/top.png'><filter/></layer>"}) {
        expectRejected(archive(fixture(image(malformedLayer))), "reject invalid/unsupported OpenRaster drawing semantics");
    }
    for (const std::string badXml : {
             "<!DOCTYPE image [<!ENTITY external SYSTEM 'file:///etc/passwd'>]><image version='0.0.6' w='3' h='2'><stack/></image>",
             "<image version='99.0.0' w='3' h='2'><stack/></image>",
             "<image version='0.0.6' w='0' h='2'><stack/></image>",
             "<image version='0.0.6' w='3' h='2'><stack/><stack/></image>",
             "<image version='0.0.6' w='3' h='2'><stack>text</stack></image>",
             "<image version='0.0.6' w='3' h='2'><stack>"}) {
        expectRejected(archive(fixture(badXml)), "reject invalid XML, dimensions, unknown versions, and entities");
    }

    for (const std::string path : {"../outside", "/absolute", "data/../outside", "C:/outside", "data\\outside"}) {
        auto entries = fixture(xml);
        entries.push_back({path, bytes("ignored")});
        expectRejected(archive(entries), "reject unsafe archive entry even when unreferenced: " + path);
    }
    auto linked = fixture(xml);
    linked.push_back({"data/link", bytes("target"), true});
    expectRejected(archive(linked), "reject unreferenced archive symlink");
    auto encrypted = fixture(xml);
    encrypted.back().encrypted = true;
    expectRejected(archive(encrypted), "reject encrypted OpenRaster entries");
    auto wrongMime = fixture(xml);
    wrongMime[0].data = bytes("application/zip");
    expectRejected(archive(wrongMime), "reject ZIP with wrong OpenRaster mimetype");
    auto compressedMime = fixture(xml);
    compressedMime[0].compression = ZIP_CM_DEFLATE;
    expectRejected(archive(compressedMime), "mimetype must be stored without compression");
    auto wrongPhysicalOrderEntries = fixture(xml);
    std::swap(wrongPhysicalOrderEntries[0], wrongPhysicalOrderEntries[1]);
    auto wrongPhysicalOrder = archive(wrongPhysicalOrderEntries);
    // Reorder only the first two central records: mimetype is logically first,
    // but stack.xml is still the first local file. This is a valid ZIP, not ORA.
    const auto directoryOffset = std::size_t(little32(wrongPhysicalOrder, wrongPhysicalOrder.size() - 22 + 16));
    const auto firstLength = centralRecordSize(wrongPhysicalOrder, directoryOffset);
    const auto secondLength = centralRecordSize(wrongPhysicalOrder, directoryOffset + firstLength);
    std::rotate(wrongPhysicalOrder.begin() + std::ptrdiff_t(directoryOffset),
                wrongPhysicalOrder.begin() + std::ptrdiff_t(directoryOffset + firstLength),
                wrongPhysicalOrder.begin() + std::ptrdiff_t(directoryOffset + firstLength + secondLength));
    expectRejected(wrongPhysicalOrder, "mimetype must be the first physical local file, not only the first central entry");
    auto inconsistentName = input;
    inconsistentName[30 + 7] = 'x';
    expectRejected(inconsistentName, "reject inconsistent ZIP local and central names");
    auto deflated = fixture(xml);
    for (std::size_t i = 1; i < deflated.size(); ++i) { deflated[i].compression = ZIP_CM_DEFLATE; }
    deflated.push_back({"mergedimage.png", deflated.back().data});
    deflated.push_back({"Thumbnails/thumbnail.png", deflated.back().data});
    const auto complete = decodeLayeredDocument(archive(deflated));
    expect(complete.ok() && complete.result.warnings.empty(),
           "stored mimetype and deflated editable payloads with standard previews import without loss warnings");
    auto corruptPng = fixture(xml);
    corruptPng.back().data.back() ^= 1U;
    expectRejected(archive(corruptPng), "reject PNG corruption even when ZIP checksum is valid");
    auto disguisedPng = fixture(xml);
    BitmapExportOptions jpeg;
    jpeg.extendedCodecs = false;
    jpeg.format = "jpeg";
    disguisedPng.back().data = encodeBitmap(makeRasterLayer(3, 2, 0xffaabbccU), jpeg).bytes;
    expectRejected(archive(disguisedPng), "PNG layer extension cannot bypass explicit native PNG signature validation");
    auto invalidXmlUtf8 = fixture(xml);
    invalidXmlUtf8[1].data.insert(invalidXmlUtf8[1].data.begin(), 0xffU);
    expectRejected(archive(invalidXmlUtf8), "reject invalid UTF-8 XML before parsing");
    auto duplicates = QByteArray(reinterpret_cast<const char *>(input.data()), qsizetype(input.size()));
    duplicates.replace("data/bot.png", "data/top.png");
    expectRejected({duplicates.begin(), duplicates.end()}, "reject duplicate ZIP names");
    auto crc = input;
    const auto png = fixture(xml).back().data;
    const auto position = std::search(crc.begin(), crc.end(), png.begin(), png.end());
    expect(position != crc.end(), "locate stored PNG for independent archive CRC corruption");
    if (position != crc.end()) { *position ^= 0xffU; }
    expectRejected(crc, "reject corrupt archive CRC before decoding PNG");
    auto invalidUtf8 = QByteArray(reinterpret_cast<const char *>(input.data()), qsizetype(input.size()));
    invalidUtf8.replace("data/bot.png", QByteArray("data/\xffot.png", 12));
    expectRejected({invalidUtf8.begin(), invalidUtf8.end()}, "reject invalid UTF-8 archive filenames");
    auto nulEntries = fixture(xml);
    nulEntries.push_back({"data/unusedx.png", bytes("unused")});
    const auto nulArchive = archive(nulEntries);
    auto nulName = QByteArray(reinterpret_cast<const char *>(nulArchive.data()), qsizetype(nulArchive.size()));
    nulName.replace("data/unusedx.png", QByteArray("data/unused\0.png", 16));
    expectRejected({nulName.begin(), nulName.end()}, "reject embedded NUL in an unreferenced raw archive filename");
    auto spacedEntries = fixture(xml);
    spacedEntries.push_back({"data/unused name.png", bytes("unused")});
    expect(decodeLayeredDocument(archive(spacedEntries)).ok(),
           "valid spaces in archive filenames remain supported without NUL normalization");
    auto zeroEntryLimit = options;
    zeroEntryLimit.maxArchiveEntries = 0;
    expectRejected(input, "entry count is bounded before the ZIP dependency allocates its directory", zeroEntryLimit);
    auto zip64 = input;
    zip64[zip64.size() - 22 + 10] = 0xff;
    zip64[zip64.size() - 22 + 11] = 0xff;
    expect(decodeLayeredDocument(zip64).result.code == MediaIoCode::UnsupportedFeature,
           "ZIP64 directory sentinels fail closed as an explicitly unsupported profile");
    auto trailing = input;
    trailing.push_back(0);
    expectRejected(trailing, "reject bytes trailing the ZIP end record");
    auto commented = input;
    commented[commented.size() - 2] = 3;
    commented.insert(commented.end(), {'o', 'r', 'a'});
    expect(decodeLayeredDocument(commented).ok(), "valid bounded ZIP end-record comments remain supported");
    auto badDirectory = input;
    badDirectory[badDirectory.size() - 22 + 16] = 0xff;
    badDirectory[badDirectory.size() - 22 + 17] = 0xff;
    expectRejected(badDirectory, "reject inconsistent central-directory bounds before raw-name access");
    auto badNameLength = input;
    const auto inputDirectory = std::size_t(little32(input, input.size() - 22 + 16));
    badNameLength[inputDirectory + 28] = 0xff;
    badNameLength[inputDirectory + 29] = 0xff;
    expectRejected(badNameLength, "reject oversized raw-name field before bounded access");
    auto split = input;
    split[split.size() - 22 + 4] = 1;
    expect(decodeLayeredDocument(split).result.code == MediaIoCode::UnsupportedFeature,
           "split ZIP archives are explicitly unsupported");
    expectRejected({input.begin(), input.end() - 1}, "reject truncated archive");

    auto bounded = options;
    bounded.maxLayers = 1;
    expectRejected(input, "enforce total imported layer limit", bounded);
    bounded = options;
    bounded.maxArchiveEntries = 3;
    expectRejected(input, "enforce archive entry limit", bounded);
    bounded = options;
    bounded.limits.maxPixelsPerFrame = 5;
    expectRejected(input, "enforce canvas dimensions before decoding", bounded);
    bounded = options;
    bounded.limits.maxXmlDepth = 2;
    expectRejected(input, "enforce XML nesting limit", bounded);
    bounded = options;
    bounded.limits.maxInputBytes = input.size() - 1;
    expectRejected(input, "enforce archive input byte limit", bounded);
    bounded = options;
    std::uint64_t expanded = 0;
    for (const auto &entry : fixture(xml)) { expanded += entry.data.size(); }
    bounded.limits.maxDecodedBytes = expanded + 30;
    expectRejected(input, "enforce cumulative inflation and raster storage budget", bounded);

    return failures == 0 ? 0 : 1;
}
