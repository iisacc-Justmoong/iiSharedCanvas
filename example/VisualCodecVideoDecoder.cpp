#include "VisualCodecVideoDecoder.h"

#include <iiVisualCodec/VideoCodec.h>
#include <boost/multiprecision/cpp_int.hpp>

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

// Plan output indices with exact integer comparisons. Timestamps are relative
// to the first presentation timestamp, avoiding overflow at large epoch PTS.
// The caller controls frame and byte limits and owns the returned selection.
struct TimingSelection {
    std::vector<std::size_t> indices;
    FrameRate rate;
    MediaIoResult result;
};
TimingSelection selectTiming(const visual::video::Video &video, const VideoDecodeRequest &request)
{
    using boost::multiprecision::cpp_int;
    auto failed = [](MediaIoCode code, const char *message) {
        return TimingSelection{{}, {}, {code, message, {}}};
    };
    const auto base = video.info.time_base, nominal = video.info.frame_rate;
    if (video.frames.empty() || base.numerator <= 0 || base.denominator <= 0
        || nominal.numerator <= 0 || nominal.denominator <= 0)
        return failed(MediaIoCode::InvalidData, "video has no usable presentation time base or frame rate");
    const auto target = request.frameRate.value_or(FrameRate{std::uint32_t(nominal.numerator),
                                                              std::uint32_t(nominal.denominator)});
    if (!target.numerator || !target.denominator)
        return failed(MediaIoCode::InvalidArgument, "invalid normalized frame rate");
    for (std::size_t i = 1; i < video.frames.size(); ++i)
        if (video.frames[i].pts <= video.frames[i - 1].pts)
            return failed(MediaIoCode::InvalidData, "video presentation timestamps are not strictly increasing");
    const cpp_int periodNumerator = cpp_int(base.denominator) * target.denominator;
    const cpp_int periodDenominator = cpp_int(base.numerator) * target.numerator;
    const cpp_int delta = cpp_int(video.frames.back().pts) - video.frames.front().pts;
    cpp_int endNumerator, endDenominator;
    if (video.frames.back().duration > 0) {
        endNumerator = delta + video.frames.back().duration; endDenominator = 1;
    } else {
        endDenominator = cpp_int(base.numerator) * nominal.numerator;
        endNumerator = delta * endDenominator + cpp_int(base.denominator) * nominal.denominator;
    }
    const cpp_int divisor = endDenominator * periodNumerator;
    const cpp_int total = (endNumerator * periodDenominator + divisor - 1) / divisor;
    const cpp_int first = request.firstFrame;
    if (first >= total) return failed(MediaIoCode::InvalidData, "requested normalized frame range is unavailable");
    cpp_int count = request.frameCount ? cpp_int(*request.frameCount) : total - first;
    if (first + count > total) return failed(MediaIoCode::InvalidData, "requested normalized frame range is incomplete");
    if (count > request.limits.maxFrames || count > std::numeric_limits<FrameIndex>::max())
        return failed(MediaIoCode::LimitExceeded, "normalized video exceeds the frame count limit");
    TimingSelection selection;
    selection.rate = target;
    const auto countValue = count.convert_to<std::size_t>();
    selection.indices.reserve(countValue);
    std::size_t source = 0;
    for (std::size_t i = 0; i < countValue; ++i) {
        if (request.cancelled && request.cancelled())
            return failed(MediaIoCode::Cancelled, "video timing normalization was cancelled");
        const cpp_int sample = (first + i) * periodNumerator;
        while (source + 1 < video.frames.size()
               && (cpp_int(video.frames[source + 1].pts) - video.frames.front().pts) * periodDenominator <= sample)
            ++source;
        selection.indices.push_back(source);
    }
    return selection;
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
    const bool nonSquare = aspect.numerator != 0 && aspect.numerator != aspect.denominator;
    if ((aspect.numerator != 0 && (aspect.numerator <= 0 || aspect.denominator <= 0))
        || (nonSquare && !normalizeSampleAspect_)
        || !turns || (!display->rotation_tag.empty() && display->rotation_tag != "0")
        || display->video.spatial.projection != visual::spatial::Projection::rectilinear
        || display->video.spatial.stereo != visual::spatial::StereoLayout::mono)
        return failure(MediaIoCode::UnsupportedFeature, "video display transform or spatial layout requires normalization");
    // Check the normalized output extent before asking the codec to allocate
    // frames; this also reports an exceeded display budget independently of
    // any decoder-specific padding allocations.
    if (nonSquare) {
        const auto product = std::uint64_t(display->video.width) * std::uint64_t(aspect.numerator);
        const auto width = (product + std::uint64_t(aspect.denominator) / 2) / std::uint64_t(aspect.denominator);
        if (!width || width > std::uint64_t(std::numeric_limits<int>::max()) || !display->video.height
            || width > request.limits.maxPixelsPerFrame / display->video.height
            || width > request.limits.maxDecodedBytes / 4 / display->video.height)
            return failure(MediaIoCode::LimitExceeded, "video sample aspect exceeds display limits");
    }
    const auto decoded = visual::video::decode_with_timing(encoded, limits, request.cancelled);
    if (!decoded) return failure(decoded.error());
    if (decoded->frame_display_matrix_differs)
        return failure(MediaIoCode::UnsupportedFeature, "video has a frame display matrix that differs from the stream");
    const auto &video = decoded->video;
    const bool needsNormalization = !constantTiming(video)
        || (request.frameRate && !sameRate(*request.frameRate, video.info.frame_rate));
    if (decoded->used_fallback_timestamps || (needsNormalization && !normalizeTiming_))
        return failure(MediaIoCode::UnsupportedFeature, "video timing requires normalization or resampling");
    TimingSelection selection;
    if (needsNormalization) {
        selection = selectTiming(video, request);
        if (!selection.result.ok()) return {{}, std::move(selection.result)};
    } else {
        const auto start = std::size_t(request.firstFrame);
        if (start >= video.frames.size()) return failure(MediaIoCode::InvalidData, "requested frame range is unavailable");
        const auto available = video.frames.size() - start;
        const auto count = request.frameCount ? std::size_t(*request.frameCount) : available;
        if (count == 0 || count > available) return failure(MediaIoCode::InvalidData, "requested frame range is incomplete");
        selection.rate = {std::uint32_t(video.info.frame_rate.numerator), std::uint32_t(video.info.frame_rate.denominator)};
        selection.indices.reserve(count);
        for (std::size_t i = 0; i < count; ++i) selection.indices.push_back(start + i);
    }
    const auto &firstImage = video.frames[selection.indices.front()].image;
    if (!firstImage.width || !firstImage.height || firstImage.width > std::uint32_t(std::numeric_limits<int>::max())
        || firstImage.height > std::uint32_t(std::numeric_limits<int>::max()))
        return failure(MediaIoCode::InvalidData, "decoder returned an invalid frame extent");
    std::uint64_t displayWidth = firstImage.width;
    if (nonSquare) {
        const auto numerator = std::uint64_t(firstImage.width) * std::uint64_t(aspect.numerator);
        displayWidth = (numerator + std::uint64_t(aspect.denominator) / 2) / std::uint64_t(aspect.denominator);
        if (!displayWidth || displayWidth > std::uint64_t(std::numeric_limits<int>::max()))
            return failure(MediaIoCode::LimitExceeded, "video sample aspect exceeds display dimensions");
    }
    const auto pixelCount = displayWidth * firstImage.height;
    if (!pixelCount || pixelCount > request.limits.maxPixelsPerFrame
        || pixelCount > request.limits.maxDecodedBytes / 4
        || selection.indices.size() > request.limits.maxDecodedBytes / (pixelCount * 4))
        return failure(MediaIoCode::LimitExceeded, "normalized video exceeds the decoded byte limit");
    DecodedVideoResult output;
    output.video.frameRate = selection.rate;
    output.video.omittedAudioStreams = display->audio_stream_count;
    output.video.frames.reserve(selection.indices.size());
    if (needsNormalization) output.result.warnings.emplace_back("variable presentation timing resampled to a fixed frame rate");
    if (aspect.numerator == 0) output.result.warnings.emplace_back("unspecified sample aspect ratio assumed square");
    if (nonSquare) output.result.warnings.emplace_back("non-square pixels resampled to square display pixels");
    if (*turns) output.result.warnings.emplace_back("right-angle display rotation normalized to upright pixels");
    if (std::any_of(video.frames.begin(), video.frames.end(), [](const auto &frame) { return frame.duration <= 0; }))
        output.result.warnings.emplace_back("missing frame duration assumed to match the constant frame rate");
    for (const auto i : selection.indices) {
        if (request.cancelled && request.cancelled()) return failure(MediaIoCode::Cancelled, "video decoding was cancelled");
        const auto &image = video.frames[i].image;
        if (!image.width || !image.height || image.width > std::uint32_t(std::numeric_limits<int>::max())
            || image.height > std::uint32_t(std::numeric_limits<int>::max())
            || image.width != firstImage.width || image.height != firstImage.height
            || image.rgba.size() != std::uint64_t(image.width) * image.height * 4)
            return failure(MediaIoCode::InvalidData, "decoder returned an invalid RGBA frame");
        const auto sourceWidth = image.width, width = std::uint32_t(displayWidth), height = image.height;
        auto frame = makeRasterLayer(int(*turns % 2 ? height : width), int(*turns % 2 ? width : height));
        for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t color;
            if (width == sourceWidth) {
                const auto offset = (std::size_t(y) * sourceWidth + x) * 4;
                color = (std::uint32_t(image.rgba[offset + 3]) << 24) | (std::uint32_t(image.rgba[offset]) << 16)
                    | (std::uint32_t(image.rgba[offset + 1]) << 8) | image.rgba[offset + 2];
            } else {
                // Interpolate straight RGBA in premultiplied form to avoid
                // pulling invisible RGB into the boundary of transparent art.
                const auto sourceX = (double(x) + .5) * sourceWidth / width - .5;
                const auto left = int(std::floor(sourceX));
                const auto fraction = sourceX - left;
                const auto x0 = std::clamp(left, 0, int(sourceWidth) - 1);
                const auto x1 = std::clamp(left + 1, 0, int(sourceWidth) - 1);
                const auto p0 = (std::size_t(y) * sourceWidth + std::size_t(x0)) * 4;
                const auto p1 = (std::size_t(y) * sourceWidth + std::size_t(x1)) * 4;
                const auto a0 = image.rgba[p0 + 3] * (1 - fraction);
                const auto a1 = image.rgba[p1 + 3] * fraction;
                const auto alpha = a0 + a1;
                const auto byte = [](double value) { return std::uint32_t(std::clamp(std::lround(value), 0L, 255L)); };
                color = byte(alpha) << 24;
                if (alpha > 0) {
                    color |= byte((image.rgba[p0] * a0 + image.rgba[p1] * a1) / alpha) << 16;
                    color |= byte((image.rgba[p0 + 1] * a0 + image.rgba[p1 + 1] * a1) / alpha) << 8;
                    color |= byte((image.rgba[p0 + 2] * a0 + image.rgba[p1 + 2] * a1) / alpha);
                }
            }
            std::uint32_t dx = x, dy = y;
            if (*turns == 1) { dx = y; dy = width - 1 - x; }
            else if (*turns == 2) { dx = width - 1 - x; dy = height - 1 - y; }
            else if (*turns == 3) { dx = height - 1 - y; dy = x; }
            frame.pixels[std::size_t(dy) * std::size_t(frame.width) + dx] = color;
        }
        output.video.frames.push_back(std::move(frame));
    }
    return output;
}
} // namespace example
