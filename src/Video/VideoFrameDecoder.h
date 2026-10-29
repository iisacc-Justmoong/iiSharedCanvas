#pragma once

#include "Media/MediaIo.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iiSharedCanvas {

struct VideoDecodeRequest {
    MediaLimits limits;
    std::optional<FrameRate> frameRate;
    FrameIndex firstFrame = 0;
    std::optional<FrameIndex> frameCount;
    // Called synchronously on the caller's thread before and after decoding.
    std::function<bool()> cancelled;
};

struct DecodedVideoFrames {
    FrameRate frameRate;
    std::vector<RasterLayer> frames;
    std::uint32_t omittedAudioStreams = 0;
};
struct DecodedVideoResult {
    DecodedVideoFrames video;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

// The application supplies the codec. Successful output must contain
// display-oriented, square-pixel, constant-rate straight-ARGB frames of equal
// size, already selected by firstFrame/frameCount and the requested frameRate.
// Implementations enforce the provided input/decoded/frame limits while they
// decode and may call cancelled on the same thread. No SDK owns the backend.
class IISHAREDCANVAS_EXPORT VideoFrameDecoder {
public:
    virtual ~VideoFrameDecoder() = default;
    virtual DecodedVideoResult decode(std::span<const std::uint8_t> encoded,
                                      const VideoDecodeRequest &request) = 0;
};

struct VideoFrameAssetResult {
    VideoAsset asset;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

// Byte acquisition and file publication belong to the caller. These functions
// validate backend output before returning a complete asset or document.
IISHAREDCANVAS_EXPORT VideoFrameAssetResult decodeVideoAsset(
    std::span<const std::uint8_t> encoded, VideoFrameDecoder &decoder,
    const std::string &assetId, const VideoDecodeRequest &request = {});
IISHAREDCANVAS_EXPORT MediaDocumentResult decodeVideoDocument(
    std::span<const std::uint8_t> encoded, VideoFrameDecoder &decoder,
    const std::string &assetIdPrefix, const std::string &layerId,
    const VideoDecodeRequest &request = {});

} // namespace iiSharedCanvas
