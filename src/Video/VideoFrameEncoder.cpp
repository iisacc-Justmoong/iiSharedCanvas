#include "VideoFrameEncoder.h"

#include "Render/FrameRenderer.h"
#include "Validation/Validation.h"

#include <limits>
#include <new>
#include <stdexcept>

namespace iiSharedCanvas {
namespace {
MediaBytesResult failure(MediaIoCode code, std::string message)
{
    return {{}, {code, std::move(message), {}}};
}
bool cancelled(const VideoEncodeRequest &request)
{
    return request.cancelled && request.cancelled();
}
}

MediaBytesResult encodeVideoFrames(const Document &document, VideoFrameEncoder &encoder,
                                   const VideoEncodeRequest &request)
{
    try {
        if (cancelled(request)) return failure(MediaIoCode::Cancelled, "video encoding was cancelled");
        const auto valid = validate(document);
        if (!valid.ok()) return failure(MediaIoCode::InvalidArgument, valid.issues.front().message);
        const auto last = request.lastFrame.value_or(document.timeline.frameCount - 1);
        if (request.firstFrame > last || last >= document.timeline.frameCount)
            return failure(MediaIoCode::InvalidArgument, "video frame selection is outside the document");
        const auto count = std::uint64_t(last) - request.firstFrame + 1;
        if (count > request.limits.maxFrames || !request.limits.maxOutputBytes)
            return failure(MediaIoCode::LimitExceeded, "video frame count or output budget exceeds the limit");
        const auto extent = documentViewRegion(document).extent;
        if (extent.width <= 0 || extent.height <= 0)
            return failure(MediaIoCode::InvalidArgument, "video view extent is invalid");
        const auto pixels = std::uint64_t(extent.width) * std::uint64_t(extent.height);
        if (pixels > request.limits.maxPixelsPerFrame || pixels > request.limits.maxDecodedBytes / 4)
            return failure(MediaIoCode::LimitExceeded, "video frame extent exceeds the pixel or byte limit");
        const auto frameBytes = pixels * 4;
        if (count > request.limits.maxDecodedBytes / frameBytes
            || count > std::numeric_limits<std::size_t>::max())
            return failure(MediaIoCode::LimitExceeded, "rendered video frames exceed the decoded byte limit");
        std::vector<RasterLayer> frames;
        frames.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t index = request.firstFrame; index <= last; ++index) {
            if (cancelled(request)) return failure(MediaIoCode::Cancelled, "video encoding was cancelled");
            auto frame = renderFrame(document, static_cast<FrameIndex>(index));
            if (!frame.ok()) return failure(MediaIoCode::InvalidData, frame.message);
            frames.push_back(std::move(frame.pixels));
        }
        if (cancelled(request)) return failure(MediaIoCode::Cancelled, "video encoding was cancelled");
        std::vector<VideoEncodeFrame> views;
        views.reserve(frames.size());
        for (const auto &frame : frames) views.push_back({frame.width, frame.height, frame.pixels});
        auto result = encoder.encode(views, document.timeline.frameRate, request);
        if (!result.ok()) { result.bytes.clear(); return result; }
        if (cancelled(request)) return failure(MediaIoCode::Cancelled, "video encoding was cancelled");
        if (result.bytes.empty()) return failure(MediaIoCode::InvalidData, "encoder returned empty video output");
        if (result.bytes.size() > request.limits.maxOutputBytes)
            return failure(MediaIoCode::LimitExceeded, "encoded video exceeds the output byte limit");
        if (!document.audioTracks.empty())
            result.result.warnings.emplace_back("document audio tracks are not included in video-only export");
        return result;
    } catch (const std::bad_alloc &) {
        return failure(MediaIoCode::LimitExceeded, "video encoding allocation failed");
    } catch (const std::length_error &) {
        return failure(MediaIoCode::LimitExceeded, "video encoding allocation is too large");
    } catch (const std::exception &error) {
        return failure(MediaIoCode::IoError, error.what());
    } catch (...) {
        return failure(MediaIoCode::IoError, "video encoder raised an unexpected error");
    }
}
} // namespace iiSharedCanvas
