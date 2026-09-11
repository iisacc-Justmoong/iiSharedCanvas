#include "IiscInput_p.hpp"

#include <File/DocumentFile.h>
#include <Serialization/IiscCodec.h>

#include <QDir>
#include <QElapsedTimer>
#include <iiFileProvider.h>
#include <QFileInfo>
#include <QTemporaryDir>


#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

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

namespace {
void backupWorkingFile(const QString &sourcePath, const QString &destinationPath,
                        const MediaLimits &mediaLimits)
{
    std::uint64_t encodedBytes = std::uint64_t(QFileInfo(sourcePath).size());
    for (const auto suffix : {"-wal", "-journal", "-shm"}) {
        const QFileInfo sidecar(sourcePath + suffix);
        if (!sidecar.exists() && !sidecar.isSymLink()) { continue; }
        if (sidecar.isSymLink() || !sidecar.isFile() || sidecar.size() < 0) {
            throw Failure(MediaIoCode::InvalidData, "SQLite source sidecars must be regular local files, not symbolic links");
        }
        if (encodedBytes > mediaLimits.maxInputBytes
            || std::uint64_t(sidecar.size()) > mediaLimits.maxInputBytes - encodedBytes) {
            throw Failure(MediaIoCode::LimitExceeded, "SQLite source and sidecars exceed the input byte budget");
        }
        encodedBytes += std::uint64_t(sidecar.size());
    }
    // No URI flag or immutable mode: the read-only connection must include
    // committed WAL content and may not force a checkpoint or journal-mode change.
    const auto byteLimit = std::min(mediaLimits.maxInputBytes, mediaLimits.maxDecodedBytes);
    try {
        iiFileProvider::Database source(sourcePath.toStdString(), true);
        source.execute("PRAGMA query_only=ON; PRAGMA trusted_schema=OFF");
        iiFileProvider::Transaction transaction(&source, false);
        iiFileProvider::File::create(destinationPath, {});
        iiFileProvider::Database destination(destinationPath.toStdString());
        source.backupTo(destination, byteLimit);
        transaction.commit();
    } catch (const iiFileProvider::FileError &error) {
        throw Failure(error.code() == iiFileProvider::FileCode::LimitExceeded ? MediaIoCode::LimitExceeded
            : error.code() == iiFileProvider::FileCode::TimedOut ? MediaIoCode::TimedOut : MediaIoCode::IoError, error.what());
    }
    if (QFileInfo(destinationPath).size() < 0
        || std::uint64_t(QFileInfo(destinationPath).size()) > byteLimit) {
        throw Failure(MediaIoCode::LimitExceeded, "private SQLite backup exceeds its byte budget");
    }
}

SerializationLimits serializationLimits(const MediaLimits &mediaLimits, std::uint32_t maxLayers)
{
    SerializationLimits limits;
    limits.maximumContainerBytes = mediaLimits.maxInputBytes;
    limits.maximumCanvasPixels = mediaLimits.maxPixelsPerFrame;
    limits.maximumTotalRasterPixels = mediaLimits.maxDecodedBytes / sizeof(std::uint32_t);
    limits.maximumLayers = maxLayers;
    return limits;
}

} // namespace

Document loadDocument(const QString &path, const QString &outputParent,
                       const MediaLimits &mediaLimits, std::uint32_t maxLayers)
{
    auto input = iiFileProvider::File::openRead(path);
    QIODevice &file = *input;
    if (file.size() <= 0) { throw Failure(MediaIoCode::InvalidData, "native input is empty"); }
    if (std::uint64_t(file.size()) > mediaLimits.maxInputBytes) {
        throw Failure(MediaIoCode::LimitExceeded, "native input exceeds the input byte budget");
    }
    const auto prefix = file.peek(16);
    const auto limits = serializationLimits(mediaLimits, maxLayers);
    if (prefix == QByteArray("SQLite format 3\0", 16)) {
        file.close();
        QTemporaryDir directory(QDir(outputParent).filePath(".iisc-input-XXXXXX"));
        if (!directory.isValid()) {
            throw Failure(MediaIoCode::IoError, "cannot create a private snapshot directory beside the destination");
        }
        const auto backup = directory.filePath("source.iisc");
        backupWorkingFile(path, backup, mediaLimits);
        // DocumentFile's authoring connection only ever touches the private
        // consistent backup, not the source database or its journal/WAL.
        DocumentFile reader;
        const auto opened = reader.open(backup.toStdString(), limits);
        if (!opened.ok() || !reader.document()) {
            throw Failure(opened.code == DocumentFileCode::LimitExceeded ? MediaIoCode::LimitExceeded
                          : opened.code == DocumentFileCode::UnsupportedFormat ? MediaIoCode::UnsupportedFormat
                          : MediaIoCode::InvalidData, opened.message);
        }
        return *reader.document();
    }
    if (!prefix.startsWith(QByteArray("IISC\r\n\x1a\n", 8))) {
        throw Failure(MediaIoCode::UnsupportedFormat, "input is not a native iisc snapshot or working file");
    }
    std::vector<std::uint8_t> bytes;
    while (!file.atEnd()) {
        const auto block = file.read(64 * 1024);
        if (block.isEmpty() && !file.atEnd()) {
            throw Failure(MediaIoCode::IoError, file.errorString().toStdString());
        }
        if (std::uint64_t(block.size()) > mediaLimits.maxInputBytes - bytes.size()) {
            throw Failure(MediaIoCode::LimitExceeded, "native input grew beyond the input byte budget");
        }
        bytes.insert(bytes.end(), block.begin(), block.end());
    }
    auto decoded = decodeIisc(bytes, limits);
    if (!decoded.ok()) {
        throw Failure(decoded.error.code == IiscErrorCode::LimitExceeded ? MediaIoCode::LimitExceeded
                      : decoded.error.code == IiscErrorCode::UnsupportedVersion ? MediaIoCode::UnsupportedFormat
                      : MediaIoCode::InvalidData, decoded.error.message);
    }
    return std::move(decoded.document);
}

} // namespace iisc_tools
