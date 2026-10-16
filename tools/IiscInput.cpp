#include "IiscInput_p.hpp"

#include <File/IiscInput_p.hpp>
#include <iiFileProvider.h>

#include <algorithm>

namespace iisc_tools {
using namespace iiSharedCanvas;
const char *codeName(MediaIoCode code)
{
    switch (code) {
    case MediaIoCode::None: return "None";
    case MediaIoCode::InvalidArgument: return "InvalidArgument";
    case MediaIoCode::UnsupportedFormat: return "UnsupportedFormat";
    case MediaIoCode::UnsupportedFeature: return "UnsupportedFeature";
    case MediaIoCode::DependencyUnavailable: return "DependencyUnavailable";
    case MediaIoCode::InvalidData: return "InvalidData";
    case MediaIoCode::LimitExceeded: return "LimitExceeded";
    case MediaIoCode::AlreadyExists: return "AlreadyExists";
    case MediaIoCode::IoError: return "IoError";
    case MediaIoCode::Cancelled: return "Cancelled";
    case MediaIoCode::TimedOut: return "TimedOut";
    }
    return "UnknownError";
}
Document loadDocument(const QString &path, const QString &outputParent, const MediaLimits &mediaLimits, std::uint32_t maxLayers)
{
    SerializationLimits limits;
    limits.maximumContainerBytes = mediaLimits.maxInputBytes;
    limits.maximumCanvasPixels = mediaLimits.maxPixelsPerFrame;
    limits.maximumTotalRasterPixels = mediaLimits.maxDecodedBytes / sizeof(std::uint32_t);
    limits.maximumLayers = maxLayers;
    try {
        if (iiFileProvider::File::readPrefix(path,16) == QByteArray("SQLite format 3\0",16)) {
            limits.maximumContainerBytes = std::min(limits.maximumContainerBytes,mediaLimits.maxDecodedBytes);
        }
        return detail::loadIiscDocumentReadOnly(path.toStdString(),outputParent.toStdString(),limits);
    } catch (const detail::IiscInputFailure &error) {
        throw Failure(error.code == DocumentFileCode::LimitExceeded ? MediaIoCode::LimitExceeded
            : error.code == DocumentFileCode::UnsupportedFormat ? MediaIoCode::UnsupportedFormat
            : error.code == DocumentFileCode::IoError ? MediaIoCode::IoError : MediaIoCode::InvalidData, error.what());
    } catch (const iiFileProvider::FileError &error) {
        throw Failure(error.code() == iiFileProvider::FileCode::LimitExceeded ? MediaIoCode::LimitExceeded
            : error.code() == iiFileProvider::FileCode::TimedOut ? MediaIoCode::TimedOut : MediaIoCode::IoError, error.what());
    }
}
} // namespace iisc_tools
