#include "VisualCodecVideoEncoder.h"
#include "VisualCodecVideoDecoder.h"

#include <iiVisualCodec/VideoCodec.h>

#include <iostream>
#include <string_view>

namespace {
using namespace iiSharedCanvas;
int failures = 0;
void expect(bool value, std::string_view message)
{
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
Document fixture()
{
    Document document;
    document.extent = {16, 8};
    document.timeline = {{25, 1}, 3};
    document.assets.emplace_back(VideoAsset{"video", {25, 1},
        {makeRasterLayer(16, 8, 0xffff0000U), makeRasterLayer(16, 8, 0xff00ff00U),
         makeRasterLayer(16, 8, 0x800000ffU)}});
    document.layers.emplace_back(VideoLayer{{"layer", "Video"}, StaticSource{"video"},
                                             {0, {}, VideoEndBehavior::Hold}});
    return document;
}
}
int main()
{
    namespace video = ii::visual::video;
    auto document = fixture();
    example::VisualCodecVideoEncoder encoder;
    VideoEncodeRequest request; request.firstFrame = 1; request.lastFrame = 2;
    const auto output = encodeVideoFrames(document, encoder, request);
    expect(output.ok(), "application adapter encodes the selected range as lossless FFV1");
    if (output.ok()) {
        auto decoded = video::decode(output.bytes);
        expect(bool(decoded) && decoded->frames.size() == 2,
               "iiVisualCodec independently decodes the encoded frame count");
        if (decoded && decoded->frames.size() == 2) {
            const auto &green = decoded->frames[0].image.rgba;
            const auto &blue = decoded->frames[1].image.rgba;
            expect(green.size() >= 4 && green[0] == 0 && green[1] == 255 && green[2] == 0 && green[3] == 255
                   && blue.size() >= 4 && blue[0] == 0 && blue[1] == 0 && blue[2] == 255 && blue[3] == 128,
                   "lossless frame order, channels and alpha survive the real codec round trip");
            const auto rate = decoded->info.frame_rate;
            if (rate.numerator <= 0 || rate.denominator <= 0
                || std::int64_t(rate.numerator) != 25 * std::int64_t(rate.denominator))
                std::cerr << "encoded frame rate: " << rate.numerator << '/' << rate.denominator << '\n';
            expect(rate.numerator > 0 && rate.denominator > 0
                   && std::int64_t(rate.numerator) == 25 * std::int64_t(rate.denominator),
                   "the document frame rate reaches the encoded stream");
        }
        example::VisualCodecVideoDecoder decoder;
        const auto reimport = decodeVideoAsset(output.bytes, decoder, "roundtrip");
        expect(reimport.ok() && reimport.asset.frames.size() == 2
               && reimport.asset.frames[1].pixels[0] == 0x800000ffU,
               "the application decoder can reimport the exported frames with their alpha");
    }
    auto fractional = fixture();
    fractional.timeline.frameRate = {30000, 1001};
    std::get<VideoAsset>(fractional.assets[0]).frameRate = fractional.timeline.frameRate;
    const auto fractional_output = encodeVideoFrames(fractional, encoder);
    example::VisualCodecVideoDecoder fractional_decoder;
    VideoDecodeRequest fractional_request; fractional_request.frameRate = fractional.timeline.frameRate;
    if (fractional_output.ok()) {
        const auto imported = decodeVideoAsset(fractional_output.bytes, fractional_decoder, "fractional", fractional_request);
        expect(imported.ok() && imported.asset.frames.size() == 3,
               "fractional frame rate survives the application encoder and decoder");
    } else {
        expect(false, "fractional frame rate video can be encoded");
    }
    request.limits.maxOutputBytes = 16;
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::LimitExceeded,
           "the real codec enforces the output byte limit");
    request = {}; request.cancelled = [] { return true; };
    expect(encodeVideoFrames(document, encoder, request).result.code == MediaIoCode::Cancelled,
           "cancellation prevents real encoding");
    video::EncodeOptions lossy; lossy.codec = video::Codec::mpeg4;
    example::VisualCodecVideoEncoder lossyEncoder(lossy, 0xffffffffU);
    request = {}; request.firstFrame = 2;
    const auto flattened = encodeVideoFrames(document, lossyEncoder, request);
    expect(flattened.ok() && flattened.result.warnings.size() >= 2,
           "an opaque codec reports alpha flattening and color loss");
    if (flattened.ok()) {
        auto decoded = video::decode(flattened.bytes);
        expect(bool(decoded) && !decoded->frames.empty()
               && decoded->frames[0].image.rgba[3] == 255,
               "the opaque codec receives pixels composited on the selected matte");
    }
    return failures ? 1 : 0;
}
