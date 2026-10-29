#include "VisualCodecVideoDecoder.h"

#include <iiVisualCodec/VideoCodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace example {
namespace {
namespace visual = ii::visual;
using namespace iiSharedCanvas;
DecodedVideoResult failure(MediaIoCode code, std::string message)
{
    return {{}, {code, std::move(message), {}}};
}
DecodedVideoResult failure(const visual::image::Error &error)
{
    using Code = visual::image::ErrorCode;
    switch (error.code) {
    case Code::invalid_input: return failure(MediaIoCode::InvalidData, error.message);
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
bool sameRate(FrameRate a, visual::video::Rational b)
{
    return b.numerator > 0 && b.denominator > 0
        && std::uint64_t(a.numerator) * std::uint64_t(b.denominator)
           == std::uint64_t(a.denominator) * std::uint64_t(b.numerator);
}
std::optional<unsigned> quarterTurns(const visual::video::DisplayInfo &info)
{
    constexpr std::array<std::int32_t, 9> identity{65536, 0, 0, 0, 65536, 0, 0, 0, 1073741824};
    constexpr std::array<std::int32_t, 9> left{0, -65536, 0, 65536, 0, 0, 0, 0, 1073741824};
    constexpr std::array<std::int32_t, 9> upsideDown{-65536, 0, 0, 0, -65536, 0, 0, 0, 1073741824};
    constexpr std::array<std::int32_t, 9> right{0, 65536, 0, -65536, 0, 0, 0, 0, 1073741824};
    if (!info.has_display_matrix || info.display_matrix == identity) return 0;
    if (info.display_matrix == left) return 1;
    if (info.display_matrix == upsideDown) return 2;
    if (info.display_matrix == right) return 3;
    return {};
}
bool constantTiming(const visual::video::Video &video)
{
    const auto rate = video.info.frame_rate, base = video.info.time_base;
    if (video.frames.empty() || rate.numerator <= 0 || rate.denominator <= 0
        || base.numerator <= 0 || base.denominator <= 0) return false;
    const long double ticks = (static_cast<long double>(base.denominator) * rate.denominator)
                            / (static_cast<long double>(base.numerator) * rate.numerator);
    if (!std::isfinite(ticks) || ticks < 1) return false;
    const auto first = video.frames.front().pts;
    for (std::size_t i = 0; i < video.frames.size(); ++i) {
        const auto &frame = video.frames[i];
        if (frame.pts < first) return false;
        const auto delta = std::uint64_t(frame.pts) - std::uint64_t(first);
        // Keep tick comparisons precise even where long double is binary64.
        if (delta > (1ULL << 40) || std::abs(static_cast<long double>(delta) - i * ticks) > 0.500001L)
            return false;
        if (frame.duration > 0 && std::abs(static_cast<long double>(frame.duration) - ticks) > 0.500001L)
            return false;
    }
    return true;
}
}

iiSharedCanvas::DecodedVideoResult VisualCodecVideoDecoder::decode(
    std::span<const std::uint8_t> encoded, const iiSharedCanvas::VideoDecodeRequest &request)
{
    using namespace iiSharedCanvas;
    visual::video::Limits limits;
    limits.input_bytes = boundedSize(request.limits.maxInputBytes);
    limits.decoded_bytes = boundedSize(request.limits.maxDecodedBytes);
    limits.frames = request.limits.maxFrames;
    const auto maxPixels = request.limits.maxPixelsPerFrame;
    const auto maxPixelBytes = maxPixels > std::numeric_limits<std::uint64_t>::max() / 4
        ? std::numeric_limits<std::uint64_t>::max() : maxPixels * 4;
    limits.image.pixel_bytes = boundedSize(std::min(maxPixelBytes, request.limits.maxDecodedBytes));
    if (request.cancelled && request.cancelled()) return failure(MediaIoCode::Cancelled, "video decoding was cancelled");
    const auto display = visual::video::inspect_display(encoded, limits);
    if (!display) return failure(display.error());
    if (request.cancelled && request.cancelled()) return failure(MediaIoCode::Cancelled, "video decoding was cancelled");
    const auto aspect = display->sample_aspect_ratio;
    const auto turns = quarterTurns(*display);
    if ((aspect.numerator != 0 && (aspect.numerator <= 0 || aspect.denominator <= 0
                                   || aspect.numerator != aspect.denominator))
        || !turns || (!display->rotation_tag.empty() && display->rotation_tag != "0")
        || display->video.spatial.projection != visual::spatial::Projection::rectilinear
        || display->video.spatial.stereo != visual::spatial::StereoLayout::mono)
        return failure(MediaIoCode::UnsupportedFeature, "video display transform or spatial layout requires normalization");
    const auto decoded = visual::video::decode_with_timing(encoded, limits, request.cancelled);
    if (!decoded) return failure(decoded.error());
    if (decoded->frame_display_matrix_differs)
        return failure(MediaIoCode::UnsupportedFeature, "video has a frame display matrix that differs from the stream");
    const auto &video = decoded->video;
    if (decoded->used_fallback_timestamps || !constantTiming(video)
        || (request.frameRate && !sameRate(*request.frameRate, video.info.frame_rate)))
        return failure(MediaIoCode::UnsupportedFeature, "video timing requires normalization or resampling");
    const auto start = std::size_t(request.firstFrame);
    if (start >= video.frames.size()) return failure(MediaIoCode::InvalidData, "requested frame range is unavailable");
    const auto available = video.frames.size() - start;
    const auto count = request.frameCount ? std::size_t(*request.frameCount) : available;
    if (count == 0 || count > available) return failure(MediaIoCode::InvalidData, "requested frame range is incomplete");
    DecodedVideoResult output;
    output.video.frameRate = {std::uint32_t(video.info.frame_rate.numerator), std::uint32_t(video.info.frame_rate.denominator)};
    output.video.omittedAudioStreams = display->audio_stream_count;
    output.video.frames.reserve(count);
    if (aspect.numerator == 0) output.result.warnings.emplace_back("unspecified sample aspect ratio assumed square");
    if (*turns) output.result.warnings.emplace_back("right-angle display rotation normalized to upright pixels");
    if (std::any_of(video.frames.begin(), video.frames.end(), [](const auto &frame) { return frame.duration <= 0; }))
        output.result.warnings.emplace_back("missing frame duration assumed to match the constant frame rate");
    for (std::size_t i = start; i < start + count; ++i) {
        if (request.cancelled && request.cancelled()) return failure(MediaIoCode::Cancelled, "video decoding was cancelled");
        const auto &image = video.frames[i].image;
        if (!image.width || !image.height || image.width > std::uint32_t(std::numeric_limits<int>::max())
            || image.height > std::uint32_t(std::numeric_limits<int>::max())
            || image.rgba.size() != std::uint64_t(image.width) * image.height * 4)
            return failure(MediaIoCode::InvalidData, "decoder returned an invalid RGBA frame");
        const auto width = image.width, height = image.height;
        auto frame = makeRasterLayer(int(*turns % 2 ? height : width), int(*turns % 2 ? width : height));
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
            const auto offset = (std::size_t(y) * width + x) * 4;
            std::uint32_t dx = x, dy = y;
            if (*turns == 1) { dx = y; dy = width - 1 - x; }
            else if (*turns == 2) { dx = width - 1 - x; dy = height - 1 - y; }
            else if (*turns == 3) { dx = height - 1 - y; dy = x; }
            frame.pixels[std::size_t(dy) * std::size_t(frame.width) + dx] = (std::uint32_t(image.rgba[offset + 3]) << 24)
                | (std::uint32_t(image.rgba[offset]) << 16) | (std::uint32_t(image.rgba[offset + 1]) << 8)
                | std::uint32_t(image.rgba[offset + 2]);
        }
        output.video.frames.push_back(std::move(frame));
    }
    return output;
}
} // namespace example
