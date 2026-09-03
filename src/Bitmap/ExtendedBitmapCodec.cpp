#include "ExtendedBitmapCodec_p.hpp"

#include "Media/MediaIo_p.hpp"

#include <QJsonArray>
#include <QColorSpace>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>
#include <limits>

namespace iiSharedCanvas::bitmap_detail {
namespace {
using namespace media_detail;
struct Format { const char *name; const char *decoder; const char *encoder; const char *pixelFormat; bool alpha; };
constexpr Format formats[] = {
    {"tga", "targa", "targa", "bgra", true},
    {"qoi", "qoi", "qoi", "rgba", true},
    {"exr", "exr", "exr", "gbrapf32le", true},
    {"dpx", "dpx", "dpx", "rgb24", false},
    {"hdr", "hdr", "hdr", "gbrpf32le", false},
    {"pcx", "pcx", "pcx", "rgb24", false},
    {"sgi", "sgi", "sgi", "rgba", true},
    {"psd", "psd", "", "", true},
    {"dds", "dds", "", "", true},
};

const Format *descriptor(const QString &name)
{
    const auto found = std::find_if(std::begin(formats), std::end(formats), [&](const auto &value) { return name == value.name; });
    return found == std::end(formats) ? nullptr : found;
}

QSet<QString> codecNames(const QByteArray &output)
{
    static const QRegularExpression pattern("^\\s*V[^\\s]{5}\\s+([A-Za-z0-9_-]+)\\s");
    QSet<QString> names;
    for (const auto &line : QString::fromUtf8(output).split('\n')) {
        const auto match = pattern.match(line);
        if (match.hasMatch()) { names.insert(match.captured(1)); }
    }
    return names;
}

MediaIoResult transformBytes(const std::string &program, const QStringList &args,
                             const MediaBackendOptions &backend,
                             std::span<const std::uint8_t> input, QByteArray &output, std::uint64_t limit)
{
    std::size_t offset = 0;
    output.clear();
    return runProcess(program, args, backend, [&](const QByteArray &block) {
        if (std::uint64_t(block.size()) > limit - std::uint64_t(output.size())) {
            return error(MediaIoCode::LimitExceeded, "extended bitmap process output exceeds byte limit");
        }
        output += block;
        return MediaIoResult{};
    }, [&] {
        MediaBytesResult block;
        const auto count = std::min<std::size_t>(64 * 1024, input.size() - offset);
        block.bytes.assign(input.begin() + std::ptrdiff_t(offset), input.begin() + std::ptrdiff_t(offset + count));
        offset += count;
        return block;
    });
}
}

bool isExtendedFormat(const QString &name) { return descriptor(name) != nullptr; }

QString detectExtendedFormat(std::span<const std::uint8_t> bytes)
{
    const auto prefix = QByteArray::fromRawData(reinterpret_cast<const char *>(bytes.data()), qsizetype(std::min<std::size_t>(bytes.size(), 32)));
    if (prefix.startsWith("qoif")) { return "qoi"; }
    if (prefix.startsWith(QByteArray::fromHex("762f3101"))) { return "exr"; }
    if (prefix.startsWith("SDPX") || prefix.startsWith("XPDS")) { return "dpx"; }
    if (prefix.startsWith("#?RADIANCE") || prefix.startsWith("#?RGBE")) { return "hdr"; }
    if (prefix.startsWith("8BPS")) { return "psd"; }
    if (prefix.startsWith("DDS ")) { return "dds"; }
    if (prefix.startsWith(QByteArray::fromHex("01da"))) { return "sgi"; }
    return {};
}

std::vector<MediaFormatCapability> extendedFormats(const MediaBackendOptions &backend)
{
    std::vector<MediaFormatCapability> result;
    QByteArray decoders, encoders;
    const bool canProbe = !QStandardPaths::findExecutable(QString::fromStdString(backend.ffprobePath)).isEmpty();
    const auto decodeResult = captureProcess(backend.ffmpegPath, {"-hide_banner", "-decoders"}, backend, decoders);
    const auto encodeResult = captureProcess(backend.ffmpegPath, {"-hide_banner", "-encoders"}, backend, encoders);
    const auto readNames = codecNames(decoders), writeNames = codecNames(encoders);
    for (const auto &format : formats) {
        const bool read = decodeResult.ok() && canProbe && readNames.contains(format.decoder);
        const bool write = encodeResult.ok() && *format.encoder && writeNames.contains(format.encoder);
        if (read || write) { result.push_back({format.name, read, write}); }
    }
    return result;
}

BitmapImportResult decodeExtended(std::span<const std::uint8_t> bytes, const QString &format,
                                  const BitmapImportOptions &options)
{
    BitmapImportResult result;
    const auto *entry = descriptor(format);
    if (!entry) { result.result = error(MediaIoCode::UnsupportedFormat, "no extended bitmap decoder for this format"); return result; }
    if (options.imageIndex != 0) {
        result.result = error(MediaIoCode::UnsupportedFeature, "extended bitmap import reads one composite image, not pages or layers");
        return result;
    }
    if (bytes.size() > std::uint64_t(std::numeric_limits<int>::max())) {
        result.result = error(MediaIoCode::LimitExceeded, "extended bitmap packet exceeds the decoder input limit");
        return result;
    }
    // Image2pipe plus an explicit codec cannot follow paths or fetch resources.
    QStringList args{"-v", "error", "-protocol_whitelist", "pipe", "-f", "image2pipe", "-c:v", entry->decoder,
                     "-frame_size", QString::number(bytes.size()), "-read_intervals", "%+#1",
                     "-show_entries", "stream=width,height,pix_fmt:frame=width,height", "-of", "json", "-i", "pipe:0"};
    QByteArray probe;
    result.result = transformBytes(options.backend.ffprobePath, args, options.backend, bytes, probe, 64 * 1024);
    if (!result.ok()) { return result; }
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(probe, &parseError);
    const auto streams = json.object().value("streams").toArray();
    if (parseError.error != QJsonParseError::NoError || streams.size() != 1) {
        result.result = error(MediaIoCode::InvalidData, "extended bitmap probe did not identify one image"); return result;
    }
    auto stream = streams[0].toObject();
    if (stream.value("width").toInt() <= 0) {
        const auto frames = json.object().value("frames").toArray();
        if (!frames.isEmpty()) { stream = frames[0].toObject(); }
    }
    const CanvasExtent extent{stream.value("width").toInt(), stream.value("height").toInt()};
    if (extent.width <= 0 || extent.height <= 0) {
        result.result = error(MediaIoCode::InvalidData, "extended bitmap probe could not decode image dimensions"); return result;
    }
    result.result = checkExtent(extent, options.limits);
    if (!result.ok()) { return result; }
    const bool linear = format == "exr" || format == "hdr";
    const auto expectedBytes = std::uint64_t(extent.width) * extent.height * (linear ? 8 : 4);
    if (expectedBytes > options.limits.maxDecodedBytes
        || expectedBytes > std::uint64_t(std::numeric_limits<qsizetype>::max())) {
        result.result = error(MediaIoCode::LimitExceeded, "extended bitmap intermediate pixels exceed the decoded-byte limit");
        return result;
    }
    args = {"-v", "error", "-nostdin", "-threads", "1", "-filter_threads", "1", "-protocol_whitelist", "pipe",
            "-f", "image2pipe", "-c:v", entry->decoder, "-frame_size", QString::number(bytes.size()),
            "-i", "pipe:0", "-map", "0:v:0", "-an", "-sn", "-dn",
            "-frames:v", "1"};
    if (format == "exr") { args += {"-vf", "unpremultiply=inplace=1"}; }
    args += {"-pix_fmt", linear ? "rgba64le" : "rgba", "-c:v", "rawvideo", "-threads", "1", "-f", "rawvideo", "pipe:1"};
    QByteArray pixels;
    result.result = transformBytes(options.backend.ffmpegPath, args, options.backend, bytes, pixels, expectedBytes);
    if (!result.ok()) { return result; }
    if (std::uint64_t(pixels.size()) != expectedBytes) {
        result.result = error(MediaIoCode::InvalidData, "extended bitmap decoder returned incomplete pixel data"); return result;
    }
    if (linear) {
        QImage image(extent.width, extent.height, QImage::Format_RGBA64);
        if (image.isNull()) { result.result = error(MediaIoCode::LimitExceeded, "cannot allocate linear bitmap pixels"); return result; }
        for (int y = 0; y < extent.height; ++y) {
            auto *row = reinterpret_cast<QRgba64 *>(image.scanLine(y));
            for (int x = 0; x < extent.width; ++x) {
                const auto *data = pixels.constData() + (qsizetype(y) * extent.width + x) * 8;
                row[x] = QRgba64::fromRgba64(qFromLittleEndian<quint16>(data), qFromLittleEndian<quint16>(data + 2),
                                            qFromLittleEndian<quint16>(data + 4), qFromLittleEndian<quint16>(data + 6));
            }
        }
        image.setColorSpace(QColorSpace::SRgbLinear);
        result.asset = {options.assetId, rasterFromImage(image, result.result)};
    } else { result.asset = {options.assetId, rasterFromRgba(pixels, extent)}; }
    if (!result.ok()) { result.asset = {}; return result; }
    result.format = format.toStdString();
    result.result.warnings.emplace_back("extended image decoded to 8-bit RGB without source ICC conversion; metadata, HDR precision and auxiliary channels are not retained");
    if (format == "psd") { result.result.warnings.emplace_back("PSD composite image imported; Photoshop layers and editable objects are not retained"); }
    if (linear) { result.result.warnings.emplace_back("scene-linear RGB interpreted with sRGB primaries and clamped to SDR; no HDR tone mapping"); }
    return result;
}

MediaBytesResult encodeExtended(const RasterLayer &pixels, const QString &format, const BitmapExportOptions &options)
{
    MediaBytesResult result;
    const auto *entry = descriptor(format);
    if (!entry || !*entry->encoder) { result.result = error(MediaIoCode::UnsupportedFormat, "no extended bitmap encoder for this format"); return result; }
    const bool linear = format == "exr" || format == "hdr";
    if (linear && (pixels.pixels.size() > options.limits.maxDecodedBytes / 8
                   || pixels.pixels.size() > std::uint64_t(std::numeric_limits<qsizetype>::max()) / 8)) {
        result.result = error(MediaIoCode::LimitExceeded, "linear bitmap intermediate pixels exceed the decoded-byte limit");
        return result;
    }
    auto rgba = rgbaBytes(pixels);
    bool matte = false;
    if (!entry->alpha) {
        auto *data = reinterpret_cast<unsigned char *>(rgba.data());
        for (qsizetype i = 0; i < rgba.size(); i += 4) {
            const auto alpha = data[i + 3];
            if (alpha == 255) { continue; }
            matte = true;
            for (int channel = 0; channel < 3; ++channel) {
                const auto background = (options.matteArgb >> (16 - channel * 8)) & 255;
                data[i + channel] = static_cast<unsigned char>((data[i + channel] * alpha + background * (255 - alpha) + 127) / 255);
            }
            data[i + 3] = 255;
        }
    }
    const QString pixelFormat = QString::fromLatin1(entry->pixelFormat);
    if (linear) {
        QImage srgb(reinterpret_cast<const uchar *>(rgba.constData()), pixels.width, pixels.height,
                    qsizetype(pixels.width) * 4, QImage::Format_RGBA8888);
        srgb.setColorSpace(QColorSpace::SRgb);
        const auto image = srgb.convertedToColorSpace(QColorSpace::SRgbLinear, QImage::Format_RGBA64);
        if (image.isNull()) { result.result = error(MediaIoCode::LimitExceeded, "cannot convert linear bitmap pixels"); return result; }
        QByteArray linearBytes(qsizetype(pixels.pixels.size()) * 8, Qt::Uninitialized);
        for (int y = 0; y < image.height(); ++y) {
            const auto *row = reinterpret_cast<const QRgba64 *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                auto *data = linearBytes.data() + (qsizetype(y) * image.width() + x) * 8;
                qToLittleEndian<quint16>(row[x].red(), data);
                qToLittleEndian<quint16>(row[x].green(), data + 2);
                qToLittleEndian<quint16>(row[x].blue(), data + 4);
                qToLittleEndian<quint16>(row[x].alpha(), data + 6);
            }
        }
        rgba = std::move(linearBytes);
    }
    QString filter = "scale=iw:ih,setsar=1,format=" + pixelFormat;
    if (format == "exr") { filter += ",premultiply=inplace=1"; }
    QStringList args{"-v", "error", "-nostdin", "-filter_threads", "1", "-f", "rawvideo", "-pixel_format", linear ? "rgba64le" : "rgba",
                     "-video_size", QString::number(pixels.width) + 'x' + QString::number(pixels.height), "-i", "pipe:0",
                     "-frames:v", "1", "-vf", filter, "-c:v", entry->encoder,
                     "-pix_fmt", format == "exr" ? pixelFormat : '+' + pixelFormat,
                     "-threads", "1", "-f", "image2pipe", "pipe:1"};
    QByteArray output;
    result.result = transformBytes(options.backend.ffmpegPath, args, options.backend,
                                   {reinterpret_cast<const std::uint8_t *>(rgba.constData()), std::size_t(rgba.size())},
                                   output, options.limits.maxOutputBytes);
    if (!result.ok()) { return result; }
    if (output.isEmpty()) { result.result = error(MediaIoCode::IoError, "extended encoder produced no image"); return result; }
    result.bytes.assign(output.begin(), output.end());
    if (matte) { result.result.warnings.emplace_back("alpha composited onto matte for the selected bitmap format"); }
    if (!options.text.empty()) { result.result.warnings.emplace_back("extended bitmap codecs do not preserve text metadata"); }
    if (format == "exr" || format == "hdr") {
        result.result.warnings.emplace_back("8-bit canvas pixels expanded to floating-point storage; no original HDR values are recovered");
    }
    return result;
}

} // namespace iiSharedCanvas::bitmap_detail
