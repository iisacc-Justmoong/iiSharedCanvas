#include <Video/VideoFrameEncoder.h>

#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool value, std::string_view message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
class Encoder final : public VideoFrameEncoder {
public:
    int calls = 0;
    FrameRate rate;
    std::vector<std::vector<std::uint32_t>> frames;
    MediaBytesResult next{{1, 2, 3}, {}};
    bool throwOnEncode = false;
    MediaBytesResult encode(std::span<const VideoEncodeFrame> input, FrameRate frameRate,
                            const VideoEncodeRequest &) override
    {
        ++calls;
        if (throwOnEncode) throw std::runtime_error("encoder failed");
        rate = frameRate;
        for (const auto &frame : input) frames.emplace_back(frame.pixels.begin(), frame.pixels.end());
        return next;
    }
};
Document fixture()
{
    Document document;
    document.extent = {2, 1};
    document.timeline = {{30000, 1001}, 3};
    document.assets.emplace_back(VideoAsset{"video", {30000, 1001},
        {makeRasterLayer(2, 1, 0xffff0000U), makeRasterLayer(2, 1, 0xff00ff00U),
         makeRasterLayer(2, 1, 0x800000ffU)}});
    document.layers.emplace_back(VideoLayer{{"layer", "Video"}, StaticSource{"video"},
                                             {0, {}, VideoEndBehavior::Hold}});
    return document;
}
}

int main()
{
    auto document = fixture();
    Encoder encoder;
    VideoEncodeRequest request;
    request.firstFrame = 1; request.lastFrame = 2;
    encoder.next.result.warnings.push_back("backend warning");
    auto result = encodeVideoFrames(document, encoder, request);
    expect(result.ok() && result.bytes == std::vector<std::uint8_t>({1, 2, 3}),
           "successful encoded bytes are returned");
    expect(encoder.calls == 1 && encoder.frames.size() == 2
           && encoder.rate.numerator == 30000 && encoder.rate.denominator == 1001,
           "the selected inclusive range and exact rational rate reach the encoder");
    expect(encoder.frames.size() == 2 && encoder.frames[0][0] == 0xff00ff00U
           && encoder.frames[1][0] == 0x800000ffU,
           "rendered frames preserve their order and straight alpha");
    expect(result.result.warnings.size() == 1, "encoder warnings survive the boundary");

    encoder = {};
    request.lastFrame = 3;
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::InvalidArgument
           && encoder.calls == 0, "out-of-range frame selections do not invoke the encoder");
    request = {}; request.limits.maxFrames = 2;
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::LimitExceeded
           && encoder.calls == 0, "the complete selection must fit the frame limit");
    request = {}; request.limits.maxPixelsPerFrame = 1;
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::LimitExceeded
           && encoder.calls == 0, "the view extent must fit the per-frame limit");
    request = {}; request.limits.maxDecodedBytes = 16;
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::LimitExceeded
           && encoder.calls == 0, "all rendered pixels must fit the decoded byte limit");

    request = {}; request.cancelled = [] { return true; };
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::Cancelled
           && encoder.calls == 0, "cancellation before rendering skips the encoder");
    int checks = 0;
    request.cancelled = [&] { return ++checks >= 5; };
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::Cancelled,
           "cancellation after frame rendering discards the result");
    checks = 0; encoder = {};
    request.cancelled = [&] { return ++checks >= 6; };
    result = encodeVideoFrames(document, encoder, request);
    expect(encoder.calls == 1 && result.result.code == MediaIoCode::Cancelled && result.bytes.empty(),
           "cancellation after encoding discards completed bytes");

    request = {}; encoder = {};
    encoder.next = {{1, 2}, {MediaIoCode::IoError, "failed", {}}};
    result = encodeVideoFrames(document, encoder, request);
    expect(result.result.code == MediaIoCode::IoError && result.bytes.empty(),
           "a backend failure cannot return partial bytes");
    encoder.next = {{}, {}};
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::InvalidData,
           "an empty successful backend result is rejected");
    encoder.next = {{1, 2, 3}, {}}; request.limits.maxOutputBytes = 2;
    result = encodeVideoFrames(document, encoder, request);
    expect(result.result.code == MediaIoCode::LimitExceeded && result.bytes.empty(),
           "a backend cannot return bytes beyond the output limit");
    request = {}; encoder.throwOnEncode = true;
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::IoError,
           "backend exceptions become errors");

    encoder = {};
    document.audioTracks.push_back(AudioTrackLayer{"audio", "Audio"});
    result = encodeVideoFrames(document, encoder, request);
    expect(result.ok() && !result.result.warnings.empty(),
           "the video-only export reports document audio tracks");
    document.timeline.frameRate.numerator = 0;
    encoder = {};
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::InvalidArgument
           && encoder.calls == 0, "invalid documents are rejected before encoding");
    return failures ? 1 : 0;
}
