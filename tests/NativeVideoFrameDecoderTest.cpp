#include <Video/VideoFrameDecoder.h>
#include <Render/FrameRenderer.h>

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace {
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool value, std::string_view message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
class Decoder final : public VideoFrameDecoder {
public:
    int calls = 0;
    VideoDecodeRequest observed;
    DecodedVideoResult next;
    bool throwOnDecode = false;
    DecodedVideoResult decode(std::span<const std::uint8_t>, const VideoDecodeRequest &request) override
    {
        ++calls; observed = request;
        if (throwOnDecode) throw std::runtime_error("decoder failed");
        return next;
    }
};
Decoder fixture()
{
    Decoder decoder;
    decoder.next.video.frameRate = {30000, 1001};
    decoder.next.video.frames = {makeRasterLayer(2, 1, 0xffff0000U), makeRasterLayer(2, 1, 0x8000ff00U)};
    decoder.next.video.omittedAudioStreams = 1;
    decoder.next.result.warnings.push_back("decoder warning");
    return decoder;
}
}

int main()
{
    using namespace iiSharedCanvas;
    constexpr std::array<std::uint8_t, 3> bytes{1, 2, 3};
    auto decoder = fixture();
    VideoDecodeRequest request; request.firstFrame = 4; request.frameCount = 2;
    request.frameRate = FrameRate{30000, 1001};
    auto asset = decodeVideoAsset(bytes, decoder, "영상", request);
    expect(asset.ok() && asset.asset.id == "영상" && asset.asset.frames.size() == 2
           && asset.asset.frameRate.numerator == 30000 && asset.asset.frameRate.denominator == 1001,
           "decoder output becomes a native video asset with exact frame rate and Unicode identity");
    expect(asset.ok() && asset.asset.frames[1].pixels[0] == 0x8000ff00U,
           "asset preserves straight-alpha frame pixels");
    expect(decoder.calls == 1 && decoder.observed.firstFrame == 4 && decoder.observed.frameCount == 2,
           "the caller's selection and limits reach the injected decoder");
    expect(asset.result.warnings.size() == 2, "backend warnings and omitted audio are reported");

    decoder = fixture();
    auto document = decodeVideoDocument(bytes, decoder, "frame", "layer", request);
    expect(document.ok() && document.document.timeline.frameCount == 2
           && document.document.assets.size() == 2 && document.document.frames.size() == 2,
           "decoded frames become a complete dynamic bitmap document");
    if (document.ok()) {
        auto first = renderFrame(document.document, 0), second = renderFrame(document.document, 1);
        expect(first.ok() && second.ok() && first.pixels.pixels[0] == 0xffff0000U
               && second.pixels.pixels[0] == 0x8000ff00U,
               "every frame resolves and renders with the original pixels");
    }

    decoder = fixture();
    expect(decodeVideoAsset({}, decoder, "video").result.code == MediaIoCode::InvalidArgument && decoder.calls == 0,
           "empty input is rejected before the backend runs");
    expect(decodeVideoAsset(bytes, decoder, std::string("bad\xff", 4)).result.code == MediaIoCode::InvalidArgument
           && decoder.calls == 0, "invalid UTF-8 identities are rejected before decode");
    request = {}; request.firstFrame = std::numeric_limits<FrameIndex>::max(); request.frameCount = 2;
    expect(decodeVideoAsset(bytes, decoder, "video", request).result.code == MediaIoCode::InvalidArgument
           && decoder.calls == 0, "overflowing frame selection is rejected before decode");
    request = {}; request.frameRate = FrameRate{0, 1};
    expect(decodeVideoAsset(bytes, decoder, "video", request).result.code == MediaIoCode::InvalidArgument
           && decoder.calls == 0, "invalid requested frame rate is rejected before decode");
    VideoDecodeRequest small; small.limits.maxInputBytes = 2;
    expect(decodeVideoAsset(bytes, decoder, "video", small).result.code == MediaIoCode::LimitExceeded
           && decoder.calls == 0, "compressed input budget is enforced before decode");
    small = {}; small.limits.maxDecodedBytes = 8;
    expect(decodeVideoAsset(bytes, decoder, "v", small).result.code == MediaIoCode::LimitExceeded && decoder.calls == 1,
           "total decoded frame budget is enforced");
    decoder = fixture(); decoder.next.video.frames[1].width = 1;
    expect(decodeVideoAsset(bytes, decoder, "video").result.code == MediaIoCode::InvalidData,
           "inconsistent backend frame geometry is rejected");
    decoder = fixture(); decoder.next.video.frames[1].pixels.pop_back();
    expect(decodeVideoAsset(bytes, decoder, "video").result.code == MediaIoCode::InvalidData,
           "backend pixel-count mismatch is rejected");
    decoder = fixture(); decoder.next.video.frameRate = {0, 1};
    expect(decodeVideoAsset(bytes, decoder, "video").result.code == MediaIoCode::InvalidData,
           "invalid backend frame rate is rejected");
    decoder = fixture(); request = {}; request.frameRate = FrameRate{60000, 2002};
    expect(decodeVideoAsset(bytes, decoder, "video", request).ok(),
           "equivalent rational frame rates are accepted");
    request.frameRate = FrameRate{24, 1};
    expect(decodeVideoAsset(bytes, decoder, "video", request).result.code == MediaIoCode::InvalidData,
           "a backend cannot return a different requested frame rate");
    decoder = fixture(); request = {}; request.frameCount = 1;
    auto incomplete = decodeVideoAsset(bytes, decoder, "video", request);
    expect(incomplete.result.code == MediaIoCode::InvalidData && incomplete.asset.frames.empty(),
           "an incomplete frame selection never exposes a partial asset");
    decoder = fixture(); request = {}; request.limits.maxFrames = 1;
    expect(decodeVideoAsset(bytes, decoder, "video", request).result.code == MediaIoCode::LimitExceeded,
           "returned frame count obeys the requested limit");
    decoder = fixture(); decoder.next.result.code = MediaIoCode::UnsupportedFeature;
    expect(decodeVideoAsset(bytes, decoder, "video").result.code == MediaIoCode::UnsupportedFeature,
           "backend feature errors are preserved");
    decoder = fixture(); decoder.throwOnDecode = true;
    expect(decodeVideoAsset(bytes, decoder, "video").result.code == MediaIoCode::IoError,
           "backend exceptions are reported through the SDK result");
    decoder = fixture(); request = {}; request.cancelled = [] { return true; };
    expect(decodeVideoAsset(bytes, decoder, "video", request).result.code == MediaIoCode::Cancelled
           && decoder.calls == 0, "cancellation is checked before invoking the backend");
    decoder = fixture(); request = {};
    int cancellationChecks = 0;
    request.cancelled = [&] { return ++cancellationChecks == 2; };
    auto cancelled = decodeVideoDocument(bytes, decoder, "frame", "layer", request);
    expect(cancelled.result.code == MediaIoCode::Cancelled && decoder.calls == 1
           && cancelled.document.assets.empty(), "cancellation after decode discards the whole document");
    return failures ? 1 : 0;
}
