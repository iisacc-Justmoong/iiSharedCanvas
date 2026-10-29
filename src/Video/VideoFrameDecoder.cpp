#include "VideoFrameDecoder.h"
#include "Validation/Validation.h"

#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>

namespace iiSharedCanvas {
namespace {
MediaIoResult error(MediaIoCode code, std::string message) { return {code, std::move(message), {}}; }
bool validId(std::string_view value)
{
    if (value.empty()) return false;
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<std::uint8_t>(value[i++]);
        if (first < 0x80) { if (!first) return false; continue; }
        unsigned count = 0; std::uint32_t code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; code = first & 0x07; minimum = 0x10000; }
        else return false;
        if (count > value.size() - i) return false;
        for (unsigned part = 0; part < count; ++part) {
            const auto next = static_cast<std::uint8_t>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
bool sameRate(FrameRate a, FrameRate b)
{
    return std::uint64_t(a.numerator) * b.denominator == std::uint64_t(b.numerator) * a.denominator;
}
MediaIoResult preflight(std::span<const std::uint8_t> encoded, const VideoDecodeRequest &request)
{
    if (encoded.empty()) return error(MediaIoCode::InvalidArgument, "video input must not be empty");
    if (encoded.size() > request.limits.maxInputBytes)
        return error(MediaIoCode::LimitExceeded, "video input exceeds the compressed byte limit");
    if (!request.limits.maxFrames || !request.limits.maxPixelsPerFrame || !request.limits.maxDecodedBytes
        || (request.frameRate && (!request.frameRate->numerator || !request.frameRate->denominator))
        || (request.frameCount && (!*request.frameCount || *request.frameCount > request.limits.maxFrames))
        || (request.frameCount && std::uint64_t(request.firstFrame) + *request.frameCount
                                   > std::uint64_t(std::numeric_limits<FrameIndex>::max()) + 1))
        return error(MediaIoCode::InvalidArgument, "invalid video frame selection, rate, or limits");
    if (request.cancelled && request.cancelled()) return error(MediaIoCode::Cancelled, "video decoding was cancelled");
    return {};
}
MediaIoResult validateFrames(const DecodedVideoFrames &video, const VideoDecodeRequest &request)
{
    if (!video.frameRate.numerator || !video.frameRate.denominator || video.frames.empty())
        return error(MediaIoCode::InvalidData, "decoder returned an empty sequence or invalid frame rate");
    if (video.frames.size() > request.limits.maxFrames || video.frames.size() > std::numeric_limits<FrameIndex>::max())
        return error(MediaIoCode::LimitExceeded, "decoded video exceeds the frame count limit");
    if (request.frameCount && video.frames.size() != *request.frameCount)
        return error(MediaIoCode::InvalidData, "decoder returned an incomplete requested frame range");
    if (request.frameRate && !sameRate(video.frameRate, *request.frameRate))
        return error(MediaIoCode::InvalidData, "decoder returned a different requested frame rate");
    const auto width = video.frames.front().width, height = video.frames.front().height;
    if (width <= 0 || height <= 0) return error(MediaIoCode::InvalidData, "decoder returned an invalid frame extent");
    const auto pixels = std::uint64_t(width) * std::uint64_t(height);
    if (pixels > request.limits.maxPixelsPerFrame || pixels > request.limits.maxDecodedBytes / 4)
        return error(MediaIoCode::LimitExceeded, "decoded video frame exceeds the pixel or byte limit");
    const auto bytes = pixels * 4;
    if (video.frames.size() > request.limits.maxDecodedBytes / bytes)
        return error(MediaIoCode::LimitExceeded, "decoded video exceeds the total byte limit");
    for (const auto &frame : video.frames) {
        if (frame.width != width || frame.height != height || frame.pixels.size() != pixels)
            return error(MediaIoCode::InvalidData, "decoder returned inconsistent frame dimensions or pixel counts");
    }
    return {};
}
DecodedVideoResult decode(std::span<const std::uint8_t> encoded, VideoFrameDecoder &decoder,
                          const VideoDecodeRequest &request)
{
    DecodedVideoResult result;
    result.result = preflight(encoded, request);
    if (!result.ok()) return result;
    result = decoder.decode(encoded, request);
    if (!result.ok()) { result.video = {}; return result; }
    if (request.cancelled && request.cancelled()) {
        result.video = {}; result.result = error(MediaIoCode::Cancelled, "video decoding was cancelled"); return result;
    }
    const auto valid = validateFrames(result.video, request);
    if (!valid.ok()) { result.video = {}; result.result = valid; return result; }
    if (result.video.omittedAudioStreams)
        result.result.warnings.emplace_back("audio streams are not imported into the canvas document");
    return result;
}
MediaIoResult exceptionResult()
{
    try { throw; }
    catch (const std::bad_alloc &) { return error(MediaIoCode::LimitExceeded, "video import allocation failed"); }
    catch (const std::length_error &) { return error(MediaIoCode::LimitExceeded, "video import allocation is too large"); }
    catch (const std::exception &failure) { return error(MediaIoCode::IoError, failure.what()); }
    catch (...) { return error(MediaIoCode::IoError, "video decoder raised an unexpected error"); }
}
}

VideoFrameAssetResult decodeVideoAsset(std::span<const std::uint8_t> encoded, VideoFrameDecoder &decoder,
                                       const std::string &assetId, const VideoDecodeRequest &request)
{
    VideoFrameAssetResult output;
    try {
        if (!validId(assetId)) { output.result = error(MediaIoCode::InvalidArgument, "video asset identity must be valid UTF-8"); return output; }
        if (assetId.size() > request.limits.maxDecodedBytes / 8) {
            output.result = error(MediaIoCode::LimitExceeded, "video asset identity exceeds the memory budget"); return output;
        }
        auto result = decode(encoded, decoder, request);
        output.result = std::move(result.result);
        if (!output.ok()) return output;
        VideoAsset asset;
        asset.id = assetId;
        asset.frameRate = result.video.frameRate;
        asset.frames = std::move(result.video.frames);
        output.asset = std::move(asset);
    } catch (...) { output.result = exceptionResult(); }
    return output;
}

MediaDocumentResult decodeVideoDocument(std::span<const std::uint8_t> encoded, VideoFrameDecoder &decoder,
                                        const std::string &assetIdPrefix, const std::string &layerId,
                                        const VideoDecodeRequest &request)
{
    MediaDocumentResult output;
    try {
        if (!validId(assetIdPrefix) || !validId(layerId)) {
            output.result = error(MediaIoCode::InvalidArgument, "video layer and asset identities must be valid UTF-8"); return output;
        }
        if (assetIdPrefix.size() > request.limits.maxDecodedBytes / 8 || layerId.size() > request.limits.maxDecodedBytes / 8) {
            output.result = error(MediaIoCode::LimitExceeded, "video identities exceed the memory budget"); return output;
        }
        auto result = decode(encoded, decoder, request);
        output.result = std::move(result.result);
        if (!output.ok()) return output;
        const auto count = result.video.frames.size();
        const auto idBytes = assetIdPrefix.size() + 16ULL;
        if (idBytes > request.limits.maxDecodedBytes / 4 || count > request.limits.maxDecodedBytes / (idBytes * 4)) {
            output.result = error(MediaIoCode::LimitExceeded, "video frame identities exceed the memory budget"); return output;
        }
        Document document;
        document.extent = {result.video.frames.front().width, result.video.frames.front().height};
        document.timeline = {result.video.frameRate, static_cast<FrameIndex>(count)};
        document.assets.reserve(count); document.frames.reserve(count);
        KeyframedSource keys; keys.frameIndices.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto id = assetIdPrefix + "." + std::to_string(i);
            document.assets.emplace_back(RasterAsset{id, std::move(result.video.frames[i])});
            document.frames.push_back({static_cast<FrameIndex>(i), {{layerId, std::move(id)}}});
            keys.frameIndices.push_back(static_cast<FrameIndex>(i));
        }
        DynamicBitmapLayer layer;
        layer.properties.id = layerId; layer.properties.name = "Imported video"; layer.content = std::move(keys);
        document.layers.emplace_back(std::move(layer));
        const auto valid = validate(document);
        if (!valid.ok()) { output.result = error(MediaIoCode::InvalidData, "decoder returned an invalid document: " + valid.issues.front().message); return output; }
        output.document = std::move(document);
    } catch (...) { output.result = exceptionResult(); }
    return output;
}
} // namespace iiSharedCanvas
