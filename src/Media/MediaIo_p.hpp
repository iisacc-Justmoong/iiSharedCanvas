#pragma once

#include "MediaIo.h"

#include <QByteArray>
#include <QBuffer>
#include <QImage>
#include <QStringList>

#include <span>

namespace iiSharedCanvas::media_detail {

class BoundedBuffer final : public QBuffer {
public:
    BoundedBuffer(QByteArray *data, std::uint64_t limit) : QBuffer(data), m_limit(limit) {}
    bool exceeded = false;
protected:
    qint64 writeData(const char *data, qint64 size) override
    {
        if (size < 0 || std::uint64_t(pos()) > m_limit || std::uint64_t(size) > m_limit - std::uint64_t(pos())) {
            exceeded = true;
            return -1;
        }
        return QBuffer::writeData(data, size);
    }
private:
    std::uint64_t m_limit;
};

MediaIoResult error(MediaIoCode code, const QString &message);
MediaIoResult checkExtent(CanvasExtent extent, const MediaLimits &limits);
MediaIoResult checkRaster(const RasterLayer &pixels, const MediaLimits &limits);
MediaIoResult checkInput(const std::string &path, const MediaLimits &limits, QString &absolute);
MediaIoResult checkDestination(const std::string &path, bool overwrite, QString &absolute);
MediaBytesResult readFile(const std::string &path, const MediaLimits &limits);
MediaIoResult writeFile(const std::string &path, std::span<const std::uint8_t> bytes,
                        bool overwrite, const MediaLimits &limits);
MediaIoResult publishFile(const QString &temporary, const std::string &path,
                          bool overwrite, const MediaLimits &limits);
QImage imageFromRaster(const RasterLayer &pixels);
RasterLayer rasterFromImage(const QImage &source, MediaIoResult &result);
QByteArray rgbaBytes(const RasterLayer &pixels);
RasterLayer rasterFromRgba(const QByteArray &bytes, CanvasExtent extent);
QString normalizedFormat(const QString &format);

using OutputConsumer = std::function<MediaIoResult(const QByteArray &)>;
using InputProducer = std::function<MediaBytesResult()>;
// A successful empty input block means EOF. Output is delivered in bounded blocks.
MediaIoResult runProcess(const std::string &program, const QStringList &arguments,
                         const MediaBackendOptions &options,
                         const OutputConsumer &consume,
                         const InputProducer &produce = {});
MediaIoResult captureProcess(const std::string &program, const QStringList &arguments,
                             const MediaBackendOptions &options, QByteArray &output,
                             std::uint64_t limit = 4 * 1024 * 1024);

} // namespace iiSharedCanvas::media_detail
