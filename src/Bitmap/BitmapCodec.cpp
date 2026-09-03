#include "BitmapCodec.h"
#include "ExtendedBitmapCodec_p.hpp"

#include "Media/MediaIo_p.hpp"
#include "Render/FrameRenderer.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QMap>
#include <QPainter>
#include <QSet>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <limits>

namespace iiSharedCanvas {
namespace {
using namespace media_detail;

bool vectorFormat(const QString &format)
{
    return QSet<QString>{"svg", "svgz", "pdf", "ps", "eps"}.contains(format);
}

MediaIoResult validatePng(std::span<const std::uint8_t> bytes)
{
    constexpr std::array<std::uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    const auto invalid = [] { return error(MediaIoCode::InvalidData, "PNG chunks are truncated, corrupt or incomplete"); };
    if (bytes.size() < signature.size() || !std::equal(signature.begin(), signature.end(), bytes.begin())) { return invalid(); }
    const auto be32 = [](std::span<const std::uint8_t> data, std::size_t offset) {
        return (std::uint32_t(data[offset]) << 24) | (std::uint32_t(data[offset + 1]) << 16)
            | (std::uint32_t(data[offset + 2]) << 8) | data[offset + 3];
    };
    bool header = false, pixels = false;
    bytes = bytes.subspan(signature.size());
    while (!bytes.empty()) {
        if (bytes.size() < 12) { return invalid(); }
        const std::size_t size = be32(bytes, 0);
        const auto type = be32(bytes, 4);
        if (size > bytes.size() - 12 || (!header && (type != 0x49484452 || size != 13))) { return invalid(); }
        if (type == 0x49484452) {
            if (header) { return invalid(); }
            header = true;
        }
        auto checked = bytes.subspan(4, size + 4);
        auto crc = crc32(0L, Z_NULL, 0);
        while (!checked.empty()) {
            const auto count = std::min<std::size_t>(checked.size(), 65536);
            crc = crc32(crc, checked.data(), uInt(count));
            checked = checked.subspan(count);
        }
        if (crc != be32(bytes, size + 8)) { return invalid(); }
        if (type == 0x49444154) { pixels = true; }
        if (type == 0x49454e44) {
            return size == 0 && pixels && bytes.size() == 12 ? MediaIoResult{} : invalid();
        }
        bytes = bytes.subspan(size + 12);
    }
    return invalid();
}

// Some ImageIO-based Qt JPEG 2000 plugins do not implement Size. Inspect only
// the standard JP2 boxes/SIZ marker; Qt still performs all pixel decoding.
QSize jpeg2000Size(std::span<const std::uint8_t> bytes)
{
    const auto be32 = [](std::span<const std::uint8_t> data, std::size_t offset) {
        return (std::uint32_t(data[offset]) << 24) | (std::uint32_t(data[offset + 1]) << 16)
            | (std::uint32_t(data[offset + 2]) << 8) | data[offset + 3];
    };
    const auto streamSize = [&](std::span<const std::uint8_t> stream) -> QSize {
        if (stream.size() < 42 || stream[0] != 0xff || stream[1] != 0x4f
            || stream[2] != 0xff || stream[3] != 0x51) { return {}; }
        const auto markerSize = (std::uint32_t(stream[4]) << 8) | stream[5];
        const auto components = (std::uint32_t(stream[40]) << 8) | stream[41];
        if (!components || markerSize != 38 + 3 * components || markerSize > stream.size() - 4) { return {}; }
        const auto right = be32(stream, 8), bottom = be32(stream, 12);
        const auto left = be32(stream, 16), top = be32(stream, 20);
        if (right <= left || bottom <= top || right - left > std::uint32_t(std::numeric_limits<int>::max())
            || bottom - top > std::uint32_t(std::numeric_limits<int>::max())) { return {}; }
        return {int(right - left), int(bottom - top)};
    };
    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0x4f) { return streamSize(bytes); }
    if (bytes.size() < 12 || be32(bytes, 0) != 12 || be32(bytes, 4) != 0x6a502020 || be32(bytes, 8) != 0x0d0a870a) { return {}; }
    QSize header, codestream;
    const auto scan = [&](auto &&self, std::span<const std::uint8_t> boxes, bool nested) -> bool {
        while (!boxes.empty()) {
            if (boxes.size() < 8) { return false; }
            std::uint64_t size = be32(boxes, 0);
            const auto type = be32(boxes, 4);
            std::size_t prefix = 8;
            if (size == 1) {
                if (boxes.size() < 16) { return false; }
                size = (std::uint64_t(be32(boxes, 8)) << 32) | be32(boxes, 12);
                prefix = 16;
            } else if (size == 0) { size = boxes.size(); }
            if (size < prefix || size > boxes.size()) { return false; }
            const auto payload = boxes.subspan(prefix, std::size_t(size) - prefix);
            if (!nested && type == 0x6a703268) {
                if (!self(self, payload, true)) { return false; }
            } else if (nested && type == 0x69686472) {
                if (header.isValid() || payload.size() != 14) { return false; }
                const auto height = be32(payload, 0), width = be32(payload, 4);
                if (!width || !height || width > std::uint32_t(std::numeric_limits<int>::max())
                    || height > std::uint32_t(std::numeric_limits<int>::max())) { return false; }
                header = QSize(int(width), int(height));
            } else if (!nested && type == 0x6a703263) {
                if (codestream.isValid()) { return false; }
                codestream = streamSize(payload);
                if (!codestream.isValid()) { return false; }
            }
            boxes = boxes.subspan(std::size_t(size));
        }
        return true;
    };
    return scan(scan, bytes, false) && header.isValid() && header == codestream ? header : QSize{};
}

}

std::vector<MediaFormatCapability> bitmapFormats(const MediaBackendOptions &backend, bool includeExtended)
{
    QMap<QString, MediaFormatCapability> formats;
    for (const auto &format : QImageReader::supportedImageFormats()) {
        const auto name = normalizedFormat(QString::fromLatin1(format));
        if (!vectorFormat(name)) { formats[name].name = name.toStdString(); formats[name].canRead = true; }
    }
    for (const auto &format : QImageWriter::supportedImageFormats()) {
        const auto name = normalizedFormat(QString::fromLatin1(format));
        if (!vectorFormat(name)) { formats[name].name = name.toStdString(); formats[name].canWrite = true; }
    }
    if (includeExtended) {
        for (const auto &format : bitmap_detail::extendedFormats(backend)) {
            auto &entry = formats[QString::fromStdString(format.name)];
            entry.name = format.name;
            entry.canRead |= format.canRead;
            entry.canWrite |= format.canWrite;
        }
    }
    std::vector<MediaFormatCapability> result;
    for (const auto &format : formats) { result.push_back(format); }
    return result;
}

BitmapImportResult decodeBitmap(std::span<const std::uint8_t> bytes, const BitmapImportOptions &options)
{
    BitmapImportResult result;
    if (bytes.size() > options.limits.maxInputBytes || bytes.size() > std::uint64_t(std::numeric_limits<qsizetype>::max())) {
        result.result = error(MediaIoCode::LimitExceeded, "encoded bitmap exceeds the byte limit");
        return result;
    }
    if (bytes.empty() || options.assetId.empty() || options.assetId.find('\0') != std::string::npos
        || QString::fromUtf8(options.assetId).toUtf8().toStdString() != options.assetId
        || options.imageIndex >= options.limits.maxFrames || options.imageIndex > std::uint32_t(std::numeric_limits<int>::max())) {
        result.result = error(MediaIoCode::InvalidArgument, "bitmap bytes, asset id and bounded image index are required");
        return result;
    }
    const auto extendedFormat = options.format.empty() ? bitmap_detail::detectExtendedFormat(bytes)
                                                      : normalizedFormat(QString::fromStdString(options.format));
    if (options.extendedCodecs && bitmap_detail::isExtendedFormat(extendedFormat)) {
        auto extended = bitmap_detail::decodeExtended(bytes, extendedFormat, options);
        if (extended.result.code != MediaIoCode::DependencyUnavailable || extendedFormat != "tga") { return extended; }
        // Without FFmpeg, a deployed Qt plugin can still read its supported TGA subset.
    }
    QByteArray data = QByteArray::fromRawData(reinterpret_cast<const char *>(bytes.data()), qsizetype(bytes.size()));
    QBuffer buffer(&data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    if (options.format.empty()) { reader.setDecideFormatFromContent(true); }
    else {
        const auto hint = normalizedFormat(QString::fromStdString(options.format));
        QByteArray native;
        for (const auto &candidate : QImageReader::supportedImageFormats()) {
            if (!vectorFormat(hint) && normalizedFormat(QString::fromLatin1(candidate)) == hint) { native = candidate; break; }
        }
        if (native.isEmpty()) {
            result.result = error(MediaIoCode::UnsupportedFormat, "requested bitmap reader is not available");
            return result;
        }
        reader.setFormat(native);
        reader.setAutoDetectImageFormat(false);
    }
    reader.setAutoTransform(options.applyOrientation);
    const auto format = normalizedFormat(QString::fromLatin1(reader.format()));
    if (format.isEmpty() || vectorFormat(format)) {
        result.result = error(MediaIoCode::UnsupportedFormat, "no native bitmap reader for this content");
        return result;
    }
    if (format == "png") {
        result.result = validatePng(bytes);
        if (!result.ok()) { return result; }
    }
    if (options.imageIndex != 0 && !reader.jumpToImage(int(options.imageIndex))) {
        result.result = error(MediaIoCode::InvalidArgument, "requested image index is unavailable");
        return result;
    }
    auto size = reader.size();
    if (!size.isValid() && format == "jp2") { size = jpeg2000Size(bytes); }
    if (!size.isValid()) {
        result.result = error(MediaIoCode::UnsupportedFeature, "reader cannot report bounded image dimensions before decoding");
        return result;
    }
    result.result = checkExtent({size.width(), size.height()}, options.limits);
    if (!result.ok()) { return result; }
    const auto image = reader.read();
    if (image.isNull()) {
        result.result = error(MediaIoCode::InvalidData, reader.errorString());
        return result;
    }
    result.result = checkExtent({image.width(), image.height()}, options.limits);
    if (!result.ok()) { return result; }
    result.asset = {options.assetId, rasterFromImage(image, result.result)};
    if (!result.ok()) { result.asset = {}; return result; }
    result.format = format.toStdString();
    for (const auto &key : image.textKeys()) { result.text.push_back({key.toStdString(), image.text(key).toStdString()}); }
    if (reader.supportsAnimation() || reader.imageCount() > 1) {
        result.result.warnings.emplace_back("only the selected image/frame was imported; use importVideo for animation");
    }
    return result;
}

BitmapImportResult importBitmap(const std::string &path, const BitmapImportOptions &options)
{
    auto input = readFile(path, options.limits);
    if (!input.ok()) { return {{}, {}, {}, std::move(input.result)}; }
    auto result = decodeBitmap(input.bytes, options);
    if (!result.ok() && result.result.code == MediaIoCode::UnsupportedFormat && options.format.empty()) {
        const auto suffix = normalizedFormat(QFileInfo(QString::fromStdString(path)).suffix());
        if (!suffix.isEmpty() && !vectorFormat(suffix)) {
            auto hinted = options;
            hinted.format = suffix.toStdString();
            return decodeBitmap(input.bytes, hinted);
        }
    }
    return result;
}

MediaBytesResult encodeBitmap(const RasterLayer &pixels, const BitmapExportOptions &options)
{
    MediaBytesResult result;
    result.result = checkRaster(pixels, options.limits);
    if (!result.ok()) { return result; }
    if (options.quality < -1 || options.quality > 100 || (options.matteArgb >> 24) != 255) {
        result.result = error(MediaIoCode::InvalidArgument, "quality must be -1 or 0..100 and matte must be opaque");
        return result;
    }
    const auto format = normalizedFormat(QString::fromStdString(options.format));
    QByteArray nativeFormat;
    for (const auto &candidate : QImageWriter::supportedImageFormats()) {
        if (!vectorFormat(format) && normalizedFormat(QString::fromLatin1(candidate)) == format) {
            nativeFormat = candidate; break;
        }
    }
    if (nativeFormat.isEmpty()) {
        if (options.extendedCodecs && bitmap_detail::isExtendedFormat(format)) {
            return bitmap_detail::encodeExtended(pixels, format, options);
        }
        result.result = error(MediaIoCode::UnsupportedFormat, "requested bitmap writer is not available");
        return result;
    }
    QByteArray encoded;
    BoundedBuffer buffer(&encoded, options.limits.maxOutputBytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, nativeFormat);
    writer.setQuality(options.quality);
    QSet<QString> keys;
    for (const auto &entry : options.text) {
        const auto key = QString::fromStdString(entry.key);
        if (key.isEmpty() || keys.contains(key) || key.contains(QChar::Null)
            || QString::fromUtf8(entry.key).toUtf8().toStdString() != entry.key
            || QString::fromUtf8(entry.value).toUtf8().toStdString() != entry.value
            || entry.value.find('\0') != std::string::npos) {
            result.result = error(MediaIoCode::InvalidArgument, "metadata requires unique nonempty UTF-8 text keys");
            return result;
        }
        keys.insert(key);
        if (format == "png") {
            const auto keyword = key.toLatin1();
            const bool badCharacter = std::any_of(keyword.begin(), keyword.end(), [](char ch) {
                const auto value = static_cast<unsigned char>(ch);
                return value < 32 || (value >= 127 && value <= 160);
            });
            if (QString::fromLatin1(keyword) != key || keyword.size() > 79 || badCharacter
                || key.startsWith(' ') || key.endsWith(' ') || key.contains("  ")) {
                result.result = error(MediaIoCode::InvalidArgument, "PNG text keywords must be 1..79 printable Latin-1 bytes without excess spaces");
                return result;
            }
        }
    }
    if (!options.text.empty() && format != "png") {
        result.result.warnings.emplace_back("text metadata retention is only guaranteed for PNG");
    }
    QImage image = imageFromRaster(pixels);
    const bool keepsAlpha = QSet<QString>{"png", "tiff", "webp", "ico", "icns", "xpm"}.contains(format);
    const bool transparent = std::any_of(pixels.pixels.begin(), pixels.pixels.end(), [](auto pixel) { return (pixel >> 24) != 255; });
    if (transparent && !keepsAlpha) {
        QImage opaque(image.size(), QImage::Format_RGB32);
        opaque.fill(options.matteArgb);
        QPainter painter(&opaque);
        painter.drawImage(0, 0, image);
        painter.end();
        image = opaque;
        result.result.warnings.emplace_back("alpha composited onto matte for the selected bitmap format");
    }
    if (QSet<QString>{"pbm", "pgm", "wbmp", "xbm"}.contains(format)) {
        result.result.warnings.emplace_back("color reduced to grayscale or monochrome for the selected bitmap format");
    }
    // QImageWriter::setText normalizes whitespace. Image text preserves exact
    // multiline generation parameters through the PNG handler instead.
    for (const auto &entry : options.text) {
        image.setText(QString::fromStdString(entry.key), QString::fromStdString(entry.value));
    }
    if (!writer.write(image)) {
        result.result = error(buffer.exceeded ? MediaIoCode::LimitExceeded : MediaIoCode::IoError, writer.errorString());
        return result;
    }
    result.bytes.assign(encoded.begin(), encoded.end());
    return result;
}

MediaIoResult exportBitmap(const RasterLayer &pixels, const std::string &path, const BitmapExportOptions &options)
{
    QString absolute;
    auto checked = checkDestination(path, options.overwrite, absolute);
    if (!checked.ok()) { return checked; }
    auto encoded = encodeBitmap(pixels, options);
    if (!encoded.ok()) { return encoded.result; }
    auto result = writeFile(path, encoded.bytes, options.overwrite, options.limits);
    result.warnings = std::move(encoded.result.warnings);
    return result;
}

MediaIoResult exportBitmapFrame(const Document &document, FrameIndex frame, const std::string &path,
                                const BitmapExportOptions &options)
{
    auto result = checkExtent(document.extent, options.limits);
    if (!result.ok()) { return result; }
    auto rendered = renderFrame(document, frame);
    if (!rendered.ok()) { return {MediaIoCode::InvalidArgument, rendered.message, {}}; }
    return exportBitmap(rendered.pixels, path, options);
}

} // namespace iiSharedCanvas
