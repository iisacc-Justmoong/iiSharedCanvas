#include "MediaIo_p.hpp"

#include <QColorSpace>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <algorithm>
#include <cstring>
#include <limits>

namespace iiSharedCanvas::media_detail {

MediaIoResult error(MediaIoCode code, const QString &message)
{
    return {code, message.toStdString(), {}};
}

MediaIoResult checkExtent(CanvasExtent extent, const MediaLimits &limits)
{
    if (extent.width <= 0 || extent.height <= 0) {
        return error(MediaIoCode::InvalidArgument, "positive pixel dimensions are required");
    }
    const auto count = std::uint64_t(extent.width) * std::uint64_t(extent.height);
    if (count > limits.maxPixelsPerFrame || count > limits.maxDecodedBytes / 4
        || count > std::uint64_t(std::numeric_limits<qsizetype>::max()) / 4) {
        return error(MediaIoCode::LimitExceeded, "decoded pixel dimensions exceed media limits");
    }
    return {};
}

MediaIoResult checkRaster(const RasterLayer &pixels, const MediaLimits &limits)
{
    auto result = checkExtent({pixels.width, pixels.height}, limits);
    if (result.ok() && pixels.pixels.size() != std::uint64_t(pixels.width) * pixels.height) {
        return error(MediaIoCode::InvalidArgument, "raster storage does not match its dimensions");
    }
    return result;
}

static bool validPath(const std::string &path)
{
    return !path.empty() && path.find('\0') == std::string::npos
        && QString::fromUtf8(path).toUtf8().toStdString() == path;
}

MediaIoResult checkInput(const std::string &path, const MediaLimits &limits, QString &absolute)
{
    if (!validPath(path) || QString::fromStdString(path).contains("://")) {
        return error(MediaIoCode::InvalidArgument, "a valid UTF-8 local file path is required");
    }
    const QFileInfo info(QString::fromStdString(path));
    if (!info.isFile() || !info.isReadable()) {
        return error(MediaIoCode::IoError, "input is not a readable regular file");
    }
    if (info.size() <= 0) { return error(MediaIoCode::InvalidData, "input file is empty"); }
    if (std::uint64_t(info.size()) > limits.maxInputBytes) {
        return error(MediaIoCode::LimitExceeded, "encoded input exceeds the byte limit");
    }
    absolute = info.canonicalFilePath();
    return {};
}

MediaIoResult checkDestination(const std::string &path, bool overwrite, QString &absolute)
{
    if (!validPath(path) || QString::fromStdString(path).contains("://")) {
        return error(MediaIoCode::InvalidArgument, "a valid UTF-8 local export path is required");
    }
    const QFileInfo info(QString::fromStdString(path));
    absolute = info.absoluteFilePath();
    if (info.suffix().compare("iisc", Qt::CaseInsensitive) == 0) {
        return error(MediaIoCode::InvalidArgument, "media exports must not replace working canvas files");
    }
    if (info.isSymLink() || (info.exists() && !info.isFile()) || !info.dir().exists()) {
        return error(MediaIoCode::IoError, "export destination must be a regular file in an existing directory");
    }
    if (info.exists()) {
        if (!overwrite) { return error(MediaIoCode::AlreadyExists, "export destination already exists"); }
        QFile existing(absolute);
        if (!existing.open(QIODevice::ReadOnly)) {
            return error(MediaIoCode::IoError, "cannot inspect existing export destination");
        }
        const auto header = existing.peek(16);
        if (header.startsWith("SQLite format 3") || header.startsWith("IISC\r\n\x1a\n")) {
            return error(MediaIoCode::InvalidArgument, "media export refuses to replace a canvas/database file");
        }
    }
    return {};
}

MediaBytesResult readFile(const std::string &path, const MediaLimits &limits)
{
    MediaBytesResult result;
    QString absolute;
    result.result = checkInput(path, limits, absolute);
    if (!result.ok()) { return result; }
    QFile file(absolute);
    if (!file.open(QIODevice::ReadOnly)) {
        result.result = error(MediaIoCode::IoError, file.errorString());
        return result;
    }
    while (!file.atEnd()) {
        const QByteArray block = file.read(64 * 1024);
        if (block.isEmpty() && file.error() != QFile::NoError) {
            result.result = error(MediaIoCode::IoError, file.errorString());
            break;
        }
        if (std::uint64_t(block.size()) > limits.maxInputBytes - result.bytes.size()) {
            result.result = error(MediaIoCode::LimitExceeded, "input grew beyond the byte limit");
            break;
        }
        result.bytes.insert(result.bytes.end(), block.begin(), block.end());
    }
    if (!result.ok()) { result.bytes.clear(); }
    return result;
}

MediaIoResult publishFile(const QString &temporary, const std::string &path,
                          bool overwrite, const MediaLimits &limits)
{
    QString absolute;
    auto result = checkDestination(path, overwrite, absolute);
    if (!result.ok()) { return result; }
    const auto size = QFileInfo(temporary).size();
    if (size <= 0) { return error(MediaIoCode::IoError, "encoder produced no file"); }
    if (std::uint64_t(size) > limits.maxOutputBytes) {
        return error(MediaIoCode::LimitExceeded, "encoded output exceeds the byte limit");
    }
    if (!overwrite) {
        if (QFile::rename(temporary, absolute)) { return {}; }
        return error(QFileInfo::exists(absolute) ? MediaIoCode::AlreadyExists : MediaIoCode::IoError,
                     "cannot publish the completed export without replacing another file");
    }
    QFile source(temporary);
    QSaveFile output(absolute);
    output.setDirectWriteFallback(false);
    if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        return error(MediaIoCode::IoError, "cannot open the completed export for atomic publication");
    }
    std::uint64_t copied = 0;
    while (!source.atEnd()) {
        const auto block = source.read(64 * 1024);
        if (block.isEmpty() && source.error() != QFile::NoError) {
            return error(MediaIoCode::IoError, source.errorString());
        }
        copied += std::uint64_t(block.size());
        if (copied > limits.maxOutputBytes) {
            return error(MediaIoCode::LimitExceeded, "encoded output exceeds the byte limit");
        }
        if (output.write(block) != block.size()) { return error(MediaIoCode::IoError, output.errorString()); }
    }
    return output.commit() ? MediaIoResult{} : error(MediaIoCode::IoError, output.errorString());
}

MediaIoResult writeFile(const std::string &path, std::span<const std::uint8_t> bytes,
                        bool overwrite, const MediaLimits &limits)
{
    QString absolute;
    auto result = checkDestination(path, overwrite, absolute);
    if (!result.ok()) { return result; }
    if (bytes.size() > limits.maxOutputBytes || bytes.size() > std::uint64_t(std::numeric_limits<qint64>::max())) {
        return error(MediaIoCode::LimitExceeded, "encoded output exceeds the byte limit");
    }
    QTemporaryFile temporary(QFileInfo(absolute).dir().filePath(".iisc-export-XXXXXX"));
    if (!temporary.open() || temporary.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size()))
                               != qint64(bytes.size()) || !temporary.flush()) {
        return error(MediaIoCode::IoError, temporary.errorString());
    }
    const auto temporaryPath = temporary.fileName();
    temporary.close();
    return publishFile(temporaryPath, path, overwrite, limits);
}

QImage imageFromRaster(const RasterLayer &pixels)
{
    QImage image(reinterpret_cast<const uchar *>(pixels.pixels.data()), pixels.width, pixels.height,
                 qsizetype(pixels.width) * 4, QImage::Format_ARGB32);
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

RasterLayer rasterFromImage(const QImage &source, MediaIoResult &result)
{
    QImage image = source;
    if (source.depth() > 32 || source.format() == QImage::Format_CMYK8888) {
        result.warnings.emplace_back("source precision/color model converted to 8-bit sRGB ARGB");
    }
    if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace::SRgb) {
        image = image.convertedToColorSpace(QColorSpace::SRgb);
        result.warnings.emplace_back("embedded color profile converted to sRGB");
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    if (image.isNull()) {
        result = error(MediaIoCode::InvalidData, "image color conversion failed");
        return {};
    }
    auto pixels = makeRasterLayer(image.width(), image.height());
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(pixels.pixels.data() + std::size_t(y) * image.width(), image.constScanLine(y),
                    std::size_t(image.width()) * 4);
    }
    return pixels;
}

QByteArray rgbaBytes(const RasterLayer &pixels)
{
    QByteArray bytes(qsizetype(pixels.pixels.size() * 4), Qt::Uninitialized);
    for (std::size_t i = 0; i < pixels.pixels.size(); ++i) {
        const auto argb = pixels.pixels[i];
        bytes[qsizetype(i * 4)] = char(argb >> 16);
        bytes[qsizetype(i * 4 + 1)] = char(argb >> 8);
        bytes[qsizetype(i * 4 + 2)] = char(argb);
        bytes[qsizetype(i * 4 + 3)] = char(argb >> 24);
    }
    return bytes;
}

RasterLayer rasterFromRgba(const QByteArray &bytes, CanvasExtent extent)
{
    auto pixels = makeRasterLayer(extent.width, extent.height);
    const auto *data = reinterpret_cast<const unsigned char *>(bytes.constData());
    for (std::size_t i = 0; i < pixels.pixels.size(); ++i) {
        pixels.pixels[i] = (std::uint32_t(data[i * 4 + 3]) << 24)
            | (std::uint32_t(data[i * 4]) << 16) | (std::uint32_t(data[i * 4 + 1]) << 8) | data[i * 4 + 2];
    }
    return pixels;
}

QString normalizedFormat(const QString &format)
{
    const auto lower = format.trimmed().toLower();
    if (lower == "jpg" || lower == "jfif") { return "jpeg"; }
    if (lower == "tif") { return "tiff"; }
    if (lower == "heif") { return "heic"; }
    return lower;
}

MediaIoResult runProcess(const std::string &program, const QStringList &arguments,
                         const MediaBackendOptions &options,
                         const OutputConsumer &consume, const InputProducer &produce)
{
    if (options.timeoutMs <= 0) { return error(MediaIoCode::InvalidArgument, "process timeout must be positive"); }
    if (options.cancelled && options.cancelled()) { return error(MediaIoCode::Cancelled, "media operation cancelled"); }
    if (!validPath(program)) { return error(MediaIoCode::DependencyUnavailable, "media executable is not configured"); }
    const auto executable = QStandardPaths::findExecutable(QString::fromStdString(program));
    if (executable.isEmpty()) { return error(MediaIoCode::DependencyUnavailable, "media executable was not found"); }
    QProcess process;
    QElapsedTimer timer;
    timer.start();
    process.start(executable, arguments, QIODevice::ReadWrite);
    if (!process.waitForStarted(std::min(options.timeoutMs, 5000))) {
        const auto code = process.error() == QProcess::Timedout ? MediaIoCode::TimedOut : MediaIoCode::DependencyUnavailable;
        const auto message = process.errorString();
        process.kill();
        process.waitForFinished(5000);
        return error(code, message);
    }
    bool inputEnded = !produce;
    if (inputEnded) { process.closeWriteChannel(); }
    QByteArray diagnostics;
    MediaIoResult result;
    const auto drain = [&]() {
        process.setReadChannel(QProcess::StandardError);
        while (process.bytesAvailable() > 0) {
            diagnostics += process.read(16 * 1024);
            if (diagnostics.size() > 16 * 1024) { diagnostics.remove(0, diagnostics.size() - 16 * 1024); }
        }
        process.setReadChannel(QProcess::StandardOutput);
        while (process.bytesAvailable() > 0 && result.ok()) {
            const auto block = process.read(64 * 1024);
            if (consume) { result = consume(block); }
        }
    };
    while (process.state() != QProcess::NotRunning) {
        if (options.cancelled && options.cancelled()) {
            result = error(MediaIoCode::Cancelled, "media operation cancelled");
        } else if (timer.elapsed() >= options.timeoutMs) {
            result = error(MediaIoCode::TimedOut, "media process exceeded its deadline");
        }
        if (!result.ok()) { break; }
        drain();
        if (!result.ok()) { break; }
        if (!inputEnded && process.bytesToWrite() < 256 * 1024) {
            auto block = produce();
            if (!block.ok()) { result = std::move(block.result); break; }
            if (block.bytes.empty()) {
                inputEnded = true;
                process.closeWriteChannel();
            } else if (process.write(reinterpret_cast<const char *>(block.bytes.data()), qint64(block.bytes.size())) < 0) {
                result = error(MediaIoCode::IoError, "cannot stream pixels to encoder");
                break;
            }
        }
        process.waitForReadyRead(10);
    }
    if (!result.ok()) {
        process.kill();
        process.waitForFinished(5000);
        return result;
    }
    drain();
    if (!result.ok()) { return result; }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return error(MediaIoCode::InvalidData, QString::fromUtf8(diagnostics).trimmed());
    }
    return {};
}

MediaIoResult captureProcess(const std::string &program, const QStringList &arguments,
                             const MediaBackendOptions &options, QByteArray &output, std::uint64_t limit)
{
    output.clear();
    return runProcess(program, arguments, options, [&](const QByteArray &block) {
        if (std::uint64_t(block.size()) > limit - std::uint64_t(output.size())) {
            return error(MediaIoCode::LimitExceeded, "media process output exceeds the byte limit");
        }
        output += block;
        return MediaIoResult{};
    });
}

} // namespace iiSharedCanvas::media_detail
