#include "VisualCodecVideoEncoder.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace example {
namespace {
namespace visual = ii::visual;
using namespace iiSharedCanvas;
MediaBytesResult failure(MediaIoCode code, std::string message)
{
    return {{}, {code, std::move(message), {}}};
}
MediaBytesResult failure(const visual::image::Error &error)
{
    using Code = visual::image::ErrorCode;
    switch (error.code) {
    case Code::invalid_input: return failure(MediaIoCode::InvalidArgument, error.message);
    case Code::unsupported_format: return failure(MediaIoCode::UnsupportedFormat, error.message);
    case Code::limit_exceeded: return failure(MediaIoCode::LimitExceeded, error.message);
    case Code::cancelled: return failure(MediaIoCode::Cancelled, error.message);
    case Code::codec_failure: return failure(MediaIoCode::IoError, error.message);
    }
    return failure(MediaIoCode::IoError, error.message);
}
std::size_t boundedSize(std::uint64_t value)
{
    return static_cast<std::size_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::size_t>::max()));
}
}

VisualCodecVideoEncoder::VisualCodecVideoEncoder(visual::video::EncodeOptions options,
                                                 std::uint32_t matteArgb)
    : options_(std::move(options)), matteArgb_(matteArgb) {}

iiSharedCanvas::MediaBytesResult VisualCodecVideoEncoder::encode(
    std::span<const iiSharedCanvas::VideoEncodeFrame> frames, iiSharedCanvas::FrameRate rate,
    const iiSharedCanvas::VideoEncodeRequest &request)
{
    using namespace iiSharedCanvas;
    if (request.cancelled && request.cancelled()) return failure(MediaIoCode::Cancelled, "video encoding was cancelled");
    if (frames.empty() || !rate.numerator || !rate.denominator || (matteArgb_ >> 24) != 255)
        return failure(MediaIoCode::InvalidArgument, "video frames, frame rate, or matte are invalid");
    if (rate.numerator > std::uint32_t(std::numeric_limits<int>::max())
        || rate.denominator > std::uint32_t(std::numeric_limits<int>::max()))
        return failure(MediaIoCode::UnsupportedFeature, "video frame rate exceeds the codec rational range");
    if (options_.spatial.projection != visual::spatial::Projection::rectilinear
        || options_.spatial.stereo != visual::spatial::StereoLayout::mono)
        return failure(MediaIoCode::UnsupportedFeature, "the canvas video adapter exports planar mono frames");
    const auto width = frames.front().width, height = frames.front().height;
    if (width <= 0 || height <= 0) return failure(MediaIoCode::InvalidArgument, "video frame extent is invalid");
    const auto pixels = std::uint64_t(width) * std::uint64_t(height);
    if (pixels > request.limits.maxPixelsPerFrame || pixels > request.limits.maxDecodedBytes / 4
        || frames.size() > request.limits.maxFrames || frames.size() > request.limits.maxDecodedBytes / (pixels * 4)
        || pixels > std::numeric_limits<std::size_t>::max() / 4)
        return failure(MediaIoCode::LimitExceeded, "video frames exceed the pixel or decoded byte limit");
    const bool flatten = options_.codec != visual::video::Codec::ffv1;
    bool alphaFlattened = false;
    std::vector<std::vector<std::uint8_t>> rgba;
    std::vector<visual::video::FrameView> views;
    rgba.reserve(frames.size()); views.reserve(frames.size());
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (request.cancelled && request.cancelled()) return failure(MediaIoCode::Cancelled, "video encoding was cancelled");
        const auto &frame = frames[index];
        if (frame.width != width || frame.height != height || frame.pixels.size() != pixels)
            return failure(MediaIoCode::InvalidData, "video frame dimensions or pixel count are inconsistent");
        auto &buffer = rgba.emplace_back(static_cast<std::size_t>(pixels * 4));
        for (std::size_t pixel = 0; pixel < frame.pixels.size(); ++pixel) {
            const auto color = frame.pixels[pixel];
            const auto alpha = color >> 24;
            if (flatten && alpha != 255) alphaFlattened = true;
            for (unsigned channel = 0; channel < 3; ++channel) {
                const auto shift = 16U - channel * 8U;
                const auto component = (color >> shift) & 255U;
                const auto matte = (matteArgb_ >> shift) & 255U;
                buffer[pixel * 4 + channel] = std::uint8_t(flatten
                    ? (component * alpha + matte * (255 - alpha) + 127) / 255 : component);
            }
            buffer[pixel * 4 + 3] = flatten ? 255 : std::uint8_t(alpha);
        }
        views.push_back({{std::uint32_t(width), std::uint32_t(height), visual::image::PixelFormat::rgba8, 0, buffer},
                         static_cast<std::int64_t>(index), 1});
    }
    visual::video::Limits limits;
    limits.output_bytes = boundedSize(request.limits.maxOutputBytes);
    limits.decoded_bytes = boundedSize(request.limits.maxDecodedBytes);
    limits.frames = request.limits.maxFrames;
    limits.image.pixel_bytes = boundedSize(std::min(request.limits.maxDecodedBytes, pixels * 4));
    auto options = options_;
    options.time_base = {int(rate.denominator), int(rate.numerator)};
    if (request.cancelled && request.cancelled()) return failure(MediaIoCode::Cancelled, "video encoding was cancelled");
    auto encoded = visual::video::encode(views, options, limits);
    if (!encoded) return failure(encoded.error());
    if (request.cancelled && request.cancelled()) return failure(MediaIoCode::Cancelled, "video encoding was cancelled");
    MediaBytesResult result{std::move(*encoded), {}};
    if (alphaFlattened) result.result.warnings.emplace_back("video alpha composited onto the requested matte");
    if (flatten) result.result.warnings.emplace_back("selected codec may reduce color fidelity or precision");
    return result;
}
} // namespace example
