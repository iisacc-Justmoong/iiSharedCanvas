#include "VideoCodec.h"

#include "Media/MediaIo_p.hpp"
#include "Render/FrameRenderer.h"
#include "Validation/Validation.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace iiSharedCanvas {
namespace {
using namespace media_detail;

const QSet<QString> &containers()
{
    static const QSet<QString> values{"matroska", "webm", "mov", "mp4", "avi", "mpeg", "mpegts",
                                      "asf", "flv", "ogg", "mxf", "gif", "apng", "nut", "ivf"};
    return values;
}

QStringList localInputArguments()
{
    // Deliberately exclude playlists, concat, URL inputs and device demuxers.
    return {"-protocol_whitelist", "file,pipe", "-format_whitelist",
            "matroska,webm,mov,avi,mpeg,mpegts,asf,flv,ogg,mxf,gif,apng,nut,ivf"};
}

bool validRate(FrameRate rate)
{
    return rate.numerator > 0 && rate.denominator > 0;
}

std::optional<FrameRate> parseRate(QString value, QChar separator = '/')
{
    const auto pair = value.split(separator);
    if (pair.size() != 2) { return {}; }
    bool okNumerator = false, okDenominator = false;
    auto numerator = pair[0].toULongLong(&okNumerator);
    auto denominator = pair[1].toULongLong(&okDenominator);
    if (!okNumerator || !okDenominator || !numerator || !denominator) { return {}; }
    const auto divisor = std::gcd(numerator, denominator);
    numerator /= divisor;
    denominator /= divisor;
    if (numerator > std::numeric_limits<std::uint32_t>::max()
        || denominator > std::numeric_limits<std::uint32_t>::max()) { return {}; }
    return FrameRate{std::uint32_t(numerator), std::uint32_t(denominator)};
}

QString rateText(FrameRate rate)
{
    return QString::number(rate.numerator) + '/' + QString::number(rate.denominator);
}

void sortUnique(std::vector<std::string> &values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

std::vector<std::string> videoCodecs(const QByteArray &output)
{
    static const QRegularExpression pattern("^\\s*V[^\\s]{5}\\s+([A-Za-z0-9_-]+)\\s");
    std::vector<std::string> values;
    for (const auto &line : QString::fromUtf8(output).split('\n')) {
        const auto match = pattern.match(line);
        if (match.hasMatch()) { values.push_back(match.captured(1).toStdString()); }
    }
    sortUnique(values);
    return values;
}

bool validId(const std::string &value)
{
    return !value.empty() && value.find('\0') == std::string::npos
        && QString::fromUtf8(value).toUtf8().toStdString() == value;
}

bool alphaPixelFormat(const QString &format)
{
    return format.startsWith("rgba") || format.startsWith("bgra") || format == "argb" || format == "abgr"
        || format.startsWith("yuva") || format.startsWith("gbrap");
}
}

VideoCapabilities videoCapabilities(const MediaBackendOptions &backend)
{
    VideoCapabilities result;
    QByteArray output;
    result.result = captureProcess(backend.ffmpegPath, {"-hide_banner", "-version"}, backend, output);
    if (!result.ok()) { return result; }
    result.version = output.split('\n').value(0).toStdString();
    result.result = captureProcess(backend.ffprobePath, {"-hide_banner", "-version"}, backend, output);
    if (!result.ok()) { return result; }
    result.result = captureProcess(backend.ffmpegPath, {"-hide_banner", "-formats"}, backend, output);
    if (!result.ok()) { return result; }
    static const QRegularExpression pattern("^ ([D ])([E ])\\s+([A-Za-z0-9_,]+)\\s");
    for (const auto &line : QString::fromUtf8(output).split('\n')) {
        const auto match = pattern.match(line);
        if (!match.hasMatch()) { continue; }
        for (const auto &name : match.captured(3).split(',')) {
            if (!containers().contains(name)) { continue; }
            if (match.captured(1) == "D") { result.demuxers.push_back(name.toStdString()); }
            if (match.captured(2) == "E") { result.muxers.push_back(name.toStdString()); }
        }
    }
    sortUnique(result.demuxers);
    sortUnique(result.muxers);
    result.result = captureProcess(backend.ffmpegPath, {"-hide_banner", "-decoders"}, backend, output);
    if (!result.ok()) { return result; }
    result.videoDecoders = videoCodecs(output);
    result.result = captureProcess(backend.ffmpegPath, {"-hide_banner", "-encoders"}, backend, output);
    if (!result.ok()) { return result; }
    result.videoEncoders = videoCodecs(output);
    return result;
}

VideoProbeResult probeVideo(const std::string &path, const MediaBackendOptions &backend, const MediaLimits &limits)
{
    VideoProbeResult result;
    QString absolute;
    result.result = checkInput(path, limits, absolute);
    if (!result.ok()) { return result; }
    QStringList arguments{"-v", "error"};
    arguments += localInputArguments();
    arguments += {"-show_streams", "-show_format", "-of", "json", "-i", absolute};
    QByteArray output;
    result.result = captureProcess(backend.ffprobePath, arguments, backend, output);
    if (!result.ok()) { return result; }
    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        result.result = error(MediaIoCode::InvalidData, "media probe returned invalid JSON");
        return result;
    }
    const auto root = json.object();
    QJsonObject video;
    for (const auto &entry : root.value("streams").toArray()) {
        const auto stream = entry.toObject();
        if (stream.value("codec_type").toString() == "audio") { ++result.info.audioStreamCount; }
        if (video.isEmpty() && stream.value("codec_type").toString() == "video"
            && stream.value("disposition").toObject().value("attached_pic").toInt() == 0) { video = stream; }
    }
    if (video.isEmpty()) {
        result.result = error(MediaIoCode::UnsupportedFormat, "container has no video stream");
        return result;
    }
    auto width = video.value("width").toInt();
    auto height = video.value("height").toInt();
    const auto sourceExtent = checkExtent({width, height}, limits);
    if (!sourceExtent.ok()) { result.result = sourceExtent; return result; }
    if (const auto aspect = parseRate(video.value("sample_aspect_ratio").toString(), ':');
        aspect && aspect->numerator != aspect->denominator) {
        const double displayWidth = double(width) * aspect->numerator / aspect->denominator;
        if (!std::isfinite(displayWidth) || displayWidth < 1 || displayWidth > std::numeric_limits<int>::max()) {
            result.result = error(MediaIoCode::LimitExceeded, "display aspect ratio exceeds pixel limits");
            return result;
        }
        width = int(std::round(displayWidth));
        result.result.warnings.emplace_back("non-square pixels normalized to square display pixels");
    }
    double rotation = video.value("tags").toObject().value("rotate").toString().toDouble();
    for (const auto &entry : video.value("side_data_list").toArray()) {
        if (entry.toObject().contains("rotation")) { rotation = entry.toObject().value("rotation").toDouble(); }
    }
    if (!std::isfinite(rotation) || std::abs(rotation / 90 - std::round(rotation / 90)) > 0.000001) {
        result.result = error(MediaIoCode::UnsupportedFeature, "only right-angle video display rotation is supported");
        return result;
    }
    if (std::fmod(std::abs(rotation), 180.0) == 90.0) { std::swap(width, height); }
    auto extentResult = checkExtent({width, height}, limits);
    if (!extentResult.ok()) { result.result = std::move(extentResult); return result; }
    result.info.extent = {width, height};
    auto rate = parseRate(video.value("avg_frame_rate").toString());
    if (!rate) { rate = parseRate(video.value("r_frame_rate").toString()); }
    if (!rate) {
        result.result = error(MediaIoCode::UnsupportedFeature, "video has no usable rational frame rate");
        return result;
    }
    result.info.frameRate = *rate;
    result.info.container = root.value("format").toObject().value("format_name").toString().toStdString();
    result.info.codec = video.value("codec_name").toString().toStdString();
    result.info.pixelFormat = video.value("pix_fmt").toString().toStdString();
    return result;
}

MediaDocumentResult importVideo(const std::string &path, const VideoImportOptions &options)
{
    MediaDocumentResult result;
    if (!validId(options.layerId) || !validId(options.assetIdPrefix) || options.limits.maxFrames == 0
        || (options.frameRate && !validRate(*options.frameRate))
        || (options.frameCount && (*options.frameCount == 0 || *options.frameCount > options.limits.maxFrames))) {
        result.result = error(MediaIoCode::InvalidArgument, "invalid video ids, frame rate or requested frame count");
        return result;
    }
    auto probe = probeVideo(path, options.backend, options.limits);
    if (!probe.ok()) { result.result = std::move(probe.result); return result; }
    QString absolute;
    result.result = checkInput(path, options.limits, absolute);
    if (!result.ok()) { return result; }
    const auto rate = options.frameRate.value_or(probe.info.frameRate);
    const auto extent = probe.info.extent;
    const auto frameBytes = std::uint64_t(extent.width) * extent.height * 4;
    QString filter = "fps=" + rateText(rate) + ",scale=" + QString::number(extent.width) + ':'
        + QString::number(extent.height) + ",setsar=1,trim=start_frame=" + QString::number(options.firstFrame);
    if (options.frameCount) {
        filter += ":end_frame=" + QString::number(std::uint64_t(options.firstFrame) + *options.frameCount);
    }
    QStringList arguments{"-v", "error", "-nostdin", "-threads", "1", "-filter_threads", "1"};
    arguments += localInputArguments();
    arguments += {"-i", absolute, "-map", "0:V:0", "-an", "-sn", "-dn", "-vf", filter,
                  "-frames:v", QString::number(options.frameCount ? *options.frameCount : std::uint64_t(options.limits.maxFrames) + 1),
                  "-fps_mode", "passthrough", "-pix_fmt", "rgba", "-c:v", "rawvideo", "-threads", "1",
                  "-f", "rawvideo", "pipe:1"};
    Document draft;
    draft.extent = extent;
    draft.timeline.frameRate = rate;
    KeyframedSource keys;
    QByteArray pending;
    std::uint64_t decodedBytes = 0;
    result.result = runProcess(options.backend.ffmpegPath, arguments, options.backend, [&](const QByteArray &block) {
        pending += block;
        while (std::uint64_t(pending.size()) >= frameBytes) {
            if (draft.assets.size() >= options.limits.maxFrames
                || frameBytes > options.limits.maxDecodedBytes - decodedBytes) {
                return error(MediaIoCode::LimitExceeded, "decoded video exceeds frame count or total pixel storage limits");
            }
            const auto index = FrameIndex(draft.assets.size());
            const auto id = options.assetIdPrefix + "." + std::to_string(index);
            auto raster = rasterFromRgba(pending, extent);
            pending.remove(0, qsizetype(frameBytes));
            draft.assets.emplace_back(RasterAsset{id, std::move(raster)});
            draft.frames.push_back({index, {{options.layerId, id}}});
            keys.frameIndices.push_back(index);
            decodedBytes += frameBytes;
        }
        return MediaIoResult{};
    });
    if (!result.ok()) { return result; }
    if (!pending.isEmpty() || draft.assets.empty()
        || (options.frameCount && draft.assets.size() != *options.frameCount)) {
        result.result = error(MediaIoCode::InvalidData, "video did not produce the complete requested frame range");
        return result;
    }
    draft.timeline.frameCount = FrameIndex(draft.assets.size());
    draft.layers.emplace_back(BitmapLayer{{options.layerId, "Imported video"}, std::move(keys)});
    const auto validation = validate(draft);
    if (!validation.ok()) {
        result.result = {MediaIoCode::InvalidData, validation.issues.front().message, {}};
        return result;
    }
    result.document = std::move(draft);
    result.result.warnings = std::move(probe.result.warnings);
    result.result.warnings.emplace_back("video timestamps resampled to constant rate " + rateText(rate).toStdString());
    result.result.warnings.emplace_back("video decoded to 8-bit display RGB; source codec, HDR precision and packet metadata are not retained");
    if (probe.info.audioStreamCount > 0) { result.result.warnings.emplace_back("audio tracks are not imported into the canvas document"); }
    return result;
}

MediaIoResult exportVideo(const Document &document, const std::string &path, const VideoExportOptions &options)
{
    QString absolute;
    auto result = checkDestination(path, options.overwrite, absolute);
    if (!result.ok()) { return result; }
    result = checkExtent(document.extent, options.limits);
    if (!result.ok()) { return result; }
    const auto validation = validate(document);
    if (!validation.ok()) { return {MediaIoCode::InvalidArgument, validation.issues.front().message, {}}; }
    const auto last = options.lastFrame.value_or(document.timeline.frameCount - 1);
    const std::uint64_t count = std::uint64_t(last) + 1 - options.firstFrame;
    if (options.firstFrame > last || last >= document.timeline.frameCount || count > options.limits.maxFrames
        || (options.matteArgb >> 24) != 255) {
        return error(MediaIoCode::InvalidArgument, "invalid export frame range, frame limit or matte");
    }
    const auto container = QString::fromStdString(options.container);
    const auto pixelFormat = QString::fromStdString(options.pixelFormat);
    static const QRegularExpression token("^[a-zA-Z0-9_-]+$");
    if (!containers().contains(container) || !token.match(pixelFormat).hasMatch()
        || !token.match(QString::fromStdString(options.codec)).hasMatch()) {
        return error(MediaIoCode::UnsupportedFormat, "unsupported container, encoder or pixel format identifier");
    }
    QByteArray encoders;
    result = captureProcess(options.backend.ffmpegPath, {"-hide_banner", "-encoders"}, options.backend, encoders);
    if (!result.ok()) { return result; }
    const auto available = videoCodecs(encoders);
    if (std::find(available.begin(), available.end(), options.codec) == available.end()) {
        return error(MediaIoCode::UnsupportedFormat, "requested video encoder is not installed");
    }
    QTemporaryFile temporary(QFileInfo(absolute).dir().filePath(".iisc-video-XXXXXX"));
    if (!temporary.open()) { return error(MediaIoCode::IoError, temporary.errorString()); }
    const auto temporaryPath = temporary.fileName();
    temporary.close();
    const auto rate = rateText(document.timeline.frameRate);
    QStringList arguments{"-v", "error", "-nostdin", "-y", "-filter_threads", "1",
                          "-f", "rawvideo", "-pixel_format", "rgba", "-video_size",
                          QString::number(document.extent.width) + 'x' + QString::number(document.extent.height),
                          "-framerate", rate, "-i", "pipe:0", "-an", "-sn", "-dn", "-map", "0:v:0",
                          "-vf", "scale=iw:ih,format=" + pixelFormat, "-c:v", QString::fromStdString(options.codec),
                          "-pix_fmt", '+' + pixelFormat, "-threads", "1", "-fps_mode", "passthrough",
                          "-frames:v", QString::number(count), "-fs", QString::number(std::min<std::uint64_t>(
                              options.limits.maxOutputBytes, std::uint64_t(std::numeric_limits<qint64>::max() - 1)) + 1)};
    if (container == "mp4" || container == "mov") { arguments += {"-movflags", "+faststart"}; }
    if (container == "gif") { arguments += {"-loop", "0"}; }
    if (container == "apng") { arguments += {"-plays", "0"}; }
    arguments += {"-f", container, temporaryPath};
    std::uint64_t next = options.firstFrame;
    bool alphaFlattened = false;
    result = runProcess(options.backend.ffmpegPath, arguments, options.backend,
                        [](const QByteArray &) { return MediaIoResult{}; }, [&]() {
        MediaBytesResult input;
        if (next > last) { return input; }
        if (std::uint64_t(std::max<qint64>(0, QFileInfo(temporaryPath).size())) > options.limits.maxOutputBytes) {
            input.result = error(MediaIoCode::LimitExceeded, "encoded video exceeds output byte limit");
            return input;
        }
        auto rendered = renderFrame(document, FrameIndex(next));
        if (!rendered.ok()) { input.result = {MediaIoCode::InvalidData, rendered.message, {}}; return input; }
        if (!alphaPixelFormat(pixelFormat)) {
            for (auto &pixel : rendered.pixels.pixels) {
                const auto alpha = pixel >> 24;
                if (alpha == 255) { continue; }
                alphaFlattened = true;
                std::uint32_t rgb = 0xff000000U;
                for (const auto shift : {0, 8, 16}) {
                    const auto channel = (((pixel >> shift) & 255) * alpha
                        + ((options.matteArgb >> shift) & 255) * (255 - alpha) + 127) / 255;
                    rgb |= channel << shift;
                }
                pixel = rgb;
            }
        }
        const auto bytes = rgbaBytes(rendered.pixels);
        input.bytes.assign(bytes.begin(), bytes.end());
        ++next;
        return input;
    });
    if (std::uint64_t(std::max<qint64>(0, QFileInfo(temporaryPath).size())) > options.limits.maxOutputBytes) {
        return error(MediaIoCode::LimitExceeded, "encoded video exceeds output byte limit");
    }
    if (!result.ok()) { return result; }
    if (next != std::uint64_t(last) + 1) { return error(MediaIoCode::IoError, "encoder stopped before receiving every frame"); }
    result = publishFile(temporaryPath, path, options.overwrite, options.limits);
    if (alphaFlattened) { result.warnings.emplace_back("video alpha composited onto the requested matte"); }
    if (options.codec != "ffv1" && options.codec != "qtrle" && options.codec != "apng"
        && options.codec != "rawvideo" && options.codec != "png") {
        result.warnings.emplace_back("selected codec/pixel format can reduce color fidelity or precision");
    }
    if (container == "gif") { result.warnings.emplace_back("GIF timing is quantized to centiseconds and colors to a palette"); }
    return result;
}

} // namespace iiSharedCanvas
