#include <iiSharedCanvas.h>

#include <QBuffer>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const std::string &message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    using namespace iiSharedCanvas;
    QDir().mkpath(IISHAREDCANVAS_TEST_OUTPUT_DIR);
    QTemporaryDir directory(QStringLiteral(IISHAREDCANVAS_TEST_OUTPUT_DIR "/bitmap-codec-XXXXXX"));
    expect(directory.isValid(), "create bitmap test directory");

    const auto formats = bitmapFormats();
    const auto supports = [&](const std::string &name, bool write) {
        return std::any_of(formats.begin(), formats.end(), [&](const auto &format) {
            return format.name == name && (write ? format.canWrite : format.canRead);
        });
    };
    expect(supports("png", false) && supports("png", true), "PNG read/write capability");
    expect(supports("jpeg", false) && supports("jpeg", true), "JPEG read/write capability");
    expect(!supports("svg", false) && !supports("pdf", false),
           "bitmap discovery must not misrepresent rasterization as native bitmap input");

    RasterLayer pixels = makeRasterLayer(16, 12, 0xff2386caU);
    pixels.pixels[0] = 0x80662211U;
    pixels.pixels[1] = 0x00123456U;
    BitmapExportOptions options;
    options.text = {{"parameters", "a mountain\nSteps: 20, Sampler: Euler, Seed: 42"},
                    {"description", "Unicode: 한글"}};
    auto encoded = encodeBitmap(pixels, options);
    expect(encoded.ok() && !encoded.bytes.empty(), "encode bitmap PNG");
    BitmapImportOptions importOptions;
    importOptions.assetId = "imported";
    auto decoded = decodeBitmap(encoded.bytes, importOptions);
    expect(decoded.ok() && decoded.asset.id == "imported"
           && decoded.asset.pixels.pixels == pixels.pixels,
           "PNG must preserve straight ARGB including transparent RGB");
    const auto sortedText = [](auto entries) {
        std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
        return entries;
    };
    expect(decoded.format == "png" && sortedText(decoded.text) == sortedText(options.text),
           "PNG text carriers and UTF-8 must round-trip");
    auto recipe = std::find_if(decoded.text.begin(), decoded.text.end(), [](const auto &entry) { return entry.key == "parameters"; });
    expect(recipe != decoded.text.end() && parseStableDiffusionGenerationParameters(recipe->value).ok(),
           "multiline generation recipe must still parse after PNG carrier extraction");

    const auto path = directory.filePath("image with spaces 한글.png").toStdString();
    expect(exportBitmap(pixels, path, options).ok(), "export real bitmap file");
    expect(importBitmap(path, importOptions).asset.pixels.pixels == pixels.pixels,
           "import real bitmap file without a save round-trip");
    expect(exportBitmap(makeRasterLayer(1, 1), path).code == MediaIoCode::AlreadyExists,
           "exports must not overwrite without explicit replacement");
    expect(importBitmap(path).asset.pixels.pixels == pixels.pixels, "collision preserves file");

    for (const std::string format : {"png", "jpeg", "bmp", "tiff", "webp", "ppm", "pgm", "pbm",
                                     "xbm", "xpm", "ico", "wbmp", "jp2", "heic", "tga", "qoi", "exr", "dpx", "hdr", "pcx", "sgi"}) {
        if (!supports(format, true) || !supports(format, false)) { continue; }
        BitmapExportOptions formatOptions;
        formatOptions.format = format;
        formatOptions.quality = 100;
        const auto solid = makeRasterLayer(32, 32, 0xff88aa44U);
        auto bytes = encodeBitmap(solid, formatOptions);
        expect(bytes.ok(), "encode available " + format + ": " + bytes.result.message);
        if (!bytes.ok()) { continue; }
        BitmapImportOptions explicitFormat;
        explicitFormat.format = format;
        auto roundTrip = decodeBitmap(bytes.bytes, explicitFormat);
        if (!roundTrip.ok()) {
            std::cerr << "encoded header " << format << ": "
                      << QByteArray(reinterpret_cast<const char *>(bytes.bytes.data()), int(std::min<std::size_t>(bytes.bytes.size(), 96))).toHex().toStdString() << '\n';
        }
        expect(roundTrip.ok() && roundTrip.asset.pixels.width == 32
               && roundTrip.asset.pixels.height == 32,
               "round-trip available " + format + ": " + roundTrip.result.message);
        BitmapImportOptions bounded;
        bounded.format = format;
        bounded.limits.maxPixelsPerFrame = 1;
        expect(decodeBitmap(bytes.bytes, bounded).result.code == MediaIoCode::LimitExceeded,
               "preflight pixel budget for " + format);
        std::cout << "bitmap round-trip: " << format << '\n';
        if (roundTrip.ok() && (format == "qoi" || format == "tga" || format == "sgi" || format == "dpx" || format == "pcx")) {
            expect(roundTrip.asset.pixels.pixels == solid.pixels, "lossless extended pixel fidelity for " + format);
        }
        if (roundTrip.ok() && (format == "exr" || format == "hdr")) {
            const auto color = roundTrip.asset.pixels.pixels[0];
            expect(std::abs(int((color >> 16) & 255) - 0x88) <= 2
                   && std::abs(int((color >> 8) & 255) - 0xaa) <= 2
                   && std::abs(int(color & 255) - 0x44) <= 2,
                   "linear-light extended formats must round-trip display color for " + format);
            auto limitedOutput = formatOptions;
            limitedOutput.limits.maxDecodedBytes = 32 * 32 * 4;
            expect(encodeBitmap(solid, limitedOutput).result.code == MediaIoCode::LimitExceeded,
                   "linear export intermediate pixels obey the decoded-byte budget for " + format);
            auto limitedInput = explicitFormat;
            limitedInput.limits.maxDecodedBytes = 32 * 32 * 4;
            expect(decodeBitmap(bytes.bytes, limitedInput).result.code == MediaIoCode::LimitExceeded,
                   "linear import intermediate pixels obey the decoded-byte budget for " + format);
        }
    }
    for (const std::string format : {"qoi", "tga", "sgi", "exr"}) {
        if (!supports(format, true) || !supports(format, false)) { continue; }
        const auto alpha = makeRasterLayer(4, 4, 0x80602040U);
        BitmapExportOptions output;
        output.format = format;
        const auto bytes = encodeBitmap(alpha, output);
        BitmapImportOptions input;
        input.format = format;
        const auto restored = decodeBitmap(bytes.bytes, input);
        expect(bytes.ok() && restored.ok(), "alpha round-trip for " + format);
        if (restored.ok()) {
            for (const auto shift : {0, 8, 16, 24}) {
                expect(std::abs(int((restored.asset.pixels.pixels[0] >> shift) & 255)
                                - int((alpha.pixels[0] >> shift) & 255)) <= 1,
                       "straight/premultiplied alpha conversion for " + format);
            }
        }
    }
    if (supports("psd", false)) {
        // PSD v1, 2x1 RGB, raw planar composite and empty optional sections.
        auto psd = QByteArray::fromHex("3842505300010000000000000003000000010000000200080003"
                                       "0000000000000000000000000000f0f078781e1e");
        auto flattened = decodeBitmap({reinterpret_cast<const std::uint8_t *>(psd.constData()), std::size_t(psd.size())});
        expect(flattened.ok() && flattened.asset.pixels.pixels == std::vector<std::uint32_t>(2, 0xfff0781eU)
               && flattened.result.warnings.size() >= 2, "PSD composite import must disclose loss of editable Photoshop layers");
    }

    // Producer-independent JPEG EXIF orientation carrier, inserted after SOI.
    BitmapExportOptions plainJpeg;
    plainJpeg.format = "jpeg";
    auto orientedBytes = encodeBitmap(makeRasterLayer(12, 8, 0xffff0000U), plainJpeg).bytes;
    const auto exif = QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000");
    orientedBytes.insert(orientedBytes.begin() + 2, exif.begin(), exif.end());
    auto oriented = decodeBitmap(orientedBytes);
    expect(oriented.ok() && oriented.asset.pixels.width == 8 && oriented.asset.pixels.height == 12,
           "JPEG EXIF orientation is applied by default");
    BitmapImportOptions unrotated;
    unrotated.applyOrientation = false;
    auto noOrientation = decodeBitmap(orientedBytes, unrotated);
    expect(noOrientation.ok() && noOrientation.asset.pixels.width == 12 && noOrientation.asset.pixels.height == 8,
           "orientation application is configurable");

    QImage wideGamut(2, 2, QImage::Format_RGBA64);
    wideGamut.fill(QColor(190, 100, 65));
    wideGamut.setColorSpace(QColorSpace::DisplayP3);
    QByteArray highDepthBytes;
    QBuffer highDepthBuffer(&highDepthBytes);
    highDepthBuffer.open(QIODevice::WriteOnly);
    expect(wideGamut.save(&highDepthBuffer, "PNG"), "create independent high-depth color fixture");
    auto highDepth = decodeBitmap({reinterpret_cast<const std::uint8_t *>(highDepthBytes.constData()), std::size_t(highDepthBytes.size())});
    const auto expectedSrgb = wideGamut.convertedToColorSpace(QColorSpace::SRgb).convertToFormat(QImage::Format_ARGB32);
    expect(highDepth.ok() && highDepth.asset.pixels.pixels[0] == expectedSrgb.pixel(0, 0)
           && highDepth.result.warnings.size() >= 2, "16-bit/profile conversion is correct and disclosed");

    if (supports("tga", true)) {
        // Uncompressed 2x1 top-origin, 24-bit TGA; no optional identifying footer.
        const auto tga = QByteArray::fromHex("0000020000000000000000000200010018200000ff00ff00");
        BitmapImportOptions hint;
        hint.format = "tga";
        auto targa = decodeBitmap({reinterpret_cast<const std::uint8_t *>(tga.constData()), std::size_t(tga.size())}, hint);
        expect(targa.ok() && targa.asset.pixels.pixels == std::vector<std::uint32_t>{0xffff0000U, 0xff00ff00U},
               "explicit format hints support bitmap formats without strong magic bytes");
    }
    MediaBackendOptions noExtendedBackend;
    noExtendedBackend.ffmpegPath = directory.filePath("missing-ffmpeg").toStdString();
    noExtendedBackend.ffprobePath = directory.filePath("missing-ffprobe").toStdString();
    const auto noExtendedFormats = bitmapFormats(noExtendedBackend);
    expect(std::none_of(noExtendedFormats.begin(), noExtendedFormats.end(), [](const auto &entry) { return entry.name == "qoi"; }),
           "absent extended runtime is not advertised as an available bitmap codec");

    options = {};
    options.format = "JPG";
    options.quality = 100;
    options.matteArgb = 0xff0000ffU;
    auto jpeg = encodeBitmap(makeRasterLayer(16, 16, 0x80ff0000U), options);
    auto jpegDecoded = decodeBitmap(jpeg.bytes);
    expect(jpeg.ok() && jpegDecoded.ok() && !jpeg.result.warnings.empty(),
           "alpha loss must be reported for JPEG");
    if (jpegDecoded.ok()) {
        const auto color = jpegDecoded.asset.pixels.pixels[0];
        expect(std::abs(int((color >> 16) & 255) - 128) < 5
               && std::abs(int(color & 255) - 127) < 5 && (color >> 24) == 255,
               "JPEG must composite transparency onto the explicit matte");
    }

    options.format = "not-a-codec";
    expect(encodeBitmap(pixels, options).result.code == MediaIoCode::UnsupportedFormat,
           "unknown writers fail closed");
    options.format = "png";
    options.quality = 101;
    expect(!encodeBitmap(pixels, options).ok(), "reject invalid quality");
    expect(!encodeBitmap(RasterLayer{}).ok(), "reject malformed raster");
    BitmapExportOptions invalidText;
    invalidText.text = {{"한글 키", "valid value"}};
    expect(!encodeBitmap(pixels, invalidText).ok(), "PNG keywords must not silently become lossy Latin-1 question marks");
    expect(!decodeBitmap(std::vector<std::uint8_t>{1, 2, 3}).ok(), "reject invalid bitmap bytes");
    auto badPng = encoded.bytes;
    badPng.pop_back();
    expect(!decodeBitmap(badPng).ok(), "truncated PNG IEND CRC must not be repaired silently by a lenient reader");
    badPng = encoded.bytes;
    badPng[29] ^= 1;
    expect(!decodeBitmap(badPng).ok(), "reject corrupted PNG chunk CRC");
    importOptions.limits.maxPixelsPerFrame = 1;
    expect(decodeBitmap(encoded.bytes, importOptions).result.code == MediaIoCode::LimitExceeded,
           "reject decoded dimensions before allocating pixels");
    importOptions = {};
    importOptions.limits.maxInputBytes = 2;
    expect(importBitmap(path, importOptions).result.code == MediaIoCode::LimitExceeded,
           "bound encoded input size");
    options = {};
    options.limits.maxOutputBytes = 8;
    expect(encodeBitmap(pixels, options).result.code == MediaIoCode::LimitExceeded,
           "bound encoded output size");

    const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\"><rect width=\"4\" height=\"4\"/></svg>";
    const std::vector<std::uint8_t> svgBytes(svg.begin(), svg.end());
    expect(!decodeBitmap(svgBytes).ok(), "native bitmap decoder must reject SVG");

    Document document;
    document.extent = {16, 12};
    DocumentFile file;
    const auto workingPath = directory.filePath("working.iisc").toStdString();
    expect(file.create(workingPath, document).ok(), "create live document for import");
    expect(file.edit([&](Document &draft) {
        draft.assets.emplace_back(decoded.asset);
        draft.layers.emplace_back(BitmapLayer{{"bitmap", "Imported bitmap"}, StaticSource{"imported"}});
        return true;
    }).ok(), "import asset and layer through one durable edit");
    DocumentFile reopened;
    expect(reopened.open(workingPath).ok() && reopened.document()->assets.size() == 1,
           "import is already on disk before close");
    options = {};
    options.overwrite = true;
    expect(!exportBitmap(pixels, workingPath, options).ok(),
           "even explicit export replacement must reject a working canvas file");
    expect(reopened.document()->assets.size() == 1, "working document remains intact");
    return failures == 0 ? 0 : 1;
}
