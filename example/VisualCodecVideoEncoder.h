#pragma once

#include <Video/VideoFrameEncoder.h>
#include <iiVisualCodec/VideoCodec.h>

namespace example {

// Application-owned composition of iiSharedCanvas and iiVisualCodec.
class VisualCodecVideoEncoder final : public iiSharedCanvas::VideoFrameEncoder {
public:
    explicit VisualCodecVideoEncoder(ii::visual::video::EncodeOptions options = {},
                                     std::uint32_t matteArgb = 0xffffffffU);
    iiSharedCanvas::MediaBytesResult encode(std::span<const iiSharedCanvas::VideoEncodeFrame> frames,
                                           iiSharedCanvas::FrameRate rate,
                                           const iiSharedCanvas::VideoEncodeRequest &request) override;
private:
    ii::visual::video::EncodeOptions options_;
    std::uint32_t matteArgb_;
};

} // namespace example
