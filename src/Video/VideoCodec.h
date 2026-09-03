#pragma once

#include "Media/MediaIo.h"

#include <optional>

namespace iiSharedCanvas {

struct VideoCapabilities {
    std::string version;
    std::vector<std::string> demuxers;
    std::vector<std::string> muxers;
    std::vector<std::string> videoDecoders;
    std::vector<std::string> videoEncoders;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

struct VideoInfo {
    CanvasExtent extent; // Display-oriented, square-pixel decoded extent.
    FrameRate frameRate;
    std::string container;
    std::string codec;
    std::string pixelFormat;
    std::uint32_t audioStreamCount = 0;
};

struct VideoProbeResult {
    VideoInfo info;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

struct VideoImportOptions {
    std::string assetIdPrefix = "video.frame";
    std::string layerId = "video";
    std::optional<FrameRate> frameRate; // VFR input is resampled to this CFR.
    FrameIndex firstFrame = 0; // Index after CFR sampling, not packet number.
    std::optional<FrameIndex> frameCount;
    MediaBackendOptions backend;
    MediaLimits limits;
};

struct VideoExportOptions {
    std::string container = "matroska";
    std::string codec = "ffv1";
    std::string pixelFormat = "bgra";
    FrameIndex firstFrame = 0;
    std::optional<FrameIndex> lastFrame; // Inclusive; absent means document end.
    std::uint32_t matteArgb = 0xffffffffU;
    bool overwrite = false;
    MediaBackendOptions backend;
    MediaLimits limits;
};

IISHAREDCANVAS_EXPORT VideoCapabilities videoCapabilities(const MediaBackendOptions &backend = {});
IISHAREDCANVAS_EXPORT VideoProbeResult probeVideo(
    const std::string &path, const MediaBackendOptions &backend = {}, const MediaLimits &limits = {});
IISHAREDCANVAS_EXPORT MediaDocumentResult importVideo(
    const std::string &path, const VideoImportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaIoResult exportVideo(
    const Document &document, const std::string &path, const VideoExportOptions &options = {});

} // namespace iiSharedCanvas
