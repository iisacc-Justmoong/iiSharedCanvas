#pragma once

#include "Document/Document.h"
#include "Export.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iiSharedCanvas {

enum class MediaIoCode {
    None, InvalidArgument, UnsupportedFormat, UnsupportedFeature,
    DependencyUnavailable, InvalidData, LimitExceeded, AlreadyExists,
    IoError, Cancelled, TimedOut,
};

struct MediaIoResult {
    MediaIoCode code = MediaIoCode::None;
    std::string message;
    std::vector<std::string> warnings;
    [[nodiscard]] bool ok() const noexcept { return code == MediaIoCode::None; }
};

struct MediaLimits {
    std::uint64_t maxInputBytes = 256ULL * 1024 * 1024;
    std::uint64_t maxOutputBytes = 1024ULL * 1024 * 1024;
    std::uint64_t maxPixelsPerFrame = 64ULL * 1024 * 1024;
    std::uint64_t maxDecodedBytes = 512ULL * 1024 * 1024;
    std::uint32_t maxFrames = 4096;
    std::uint32_t maxVectorCommands = 1024 * 1024;
    std::uint32_t maxXmlDepth = 128;
};

struct MediaBackendOptions {
    // Application-controlled executable paths; never interpreted by a shell.
    std::string ffmpegPath = "ffmpeg";
    std::string ffprobePath = "ffprobe";
    int timeoutMs = 60000;
    // Called on the caller's thread. These synchronous APIs may run on a worker.
    std::function<bool()> cancelled;
};

struct MediaFormatCapability {
    std::string name;
    bool canRead = false;
    bool canWrite = false;
};

struct MediaTextEntry {
    std::string key;
    std::string value;
    bool operator==(const MediaTextEntry &) const = default;
};

struct MediaBytesResult {
    std::vector<std::uint8_t> bytes;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

struct BitmapImportResult {
    RasterAsset asset;
    std::string format;
    std::vector<MediaTextEntry> text;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

struct MediaDocumentResult {
    Document document;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

} // namespace iiSharedCanvas
