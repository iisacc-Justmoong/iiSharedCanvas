#pragma once

#include "Media/MediaIo.h"

#include <functional>
#include <optional>
#include <span>

namespace iiSharedCanvas {

struct VideoEncodeRequest {
    FrameIndex firstFrame = 0;
    std::optional<FrameIndex> lastFrame; // Inclusive; absent selects the document end.
    MediaLimits limits;
    // Called synchronously on the caller's thread during rendering and encoding.
    std::function<bool()> cancelled;
};

// A borrowed row-major view. Pixels are straight-alpha ARGB and remain valid
// only until the encoder's synchronous call returns.
struct VideoEncodeFrame {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::span<const std::uint32_t> pixels;
};

// The application supplies the codec and its format settings. Frames are
// display-sized, straight-ARGB, constant-rate and ordered from time zero.
// The encoder enforces the request's output limit while producing bytes. It
// may call cancelled on the calling thread and must never return partial bytes
// as a successful result. File publication remains the application's choice.
class IISHAREDCANVAS_EXPORT VideoFrameEncoder {
public:
    virtual ~VideoFrameEncoder() = default;
    virtual MediaBytesResult encode(std::span<const VideoEncodeFrame> frames, FrameRate rate,
                                    const VideoEncodeRequest &request) = 0;
};

// Renders the complete selected range before invoking the encoder. Limits
// cover the combined frame pixels, not renderer/backend temporary allocations.
IISHAREDCANVAS_EXPORT MediaBytesResult encodeVideoFrames(
    const Document &document, VideoFrameEncoder &encoder, const VideoEncodeRequest &request = {});

} // namespace iiSharedCanvas
