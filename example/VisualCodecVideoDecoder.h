#pragma once

#include <Video/VideoFrameDecoder.h>

namespace example {

// Application-owned composition of iiSharedCanvas and iiVisualCodec. This
// accepts square-pixel CFR video and canonical right-angle display matrices.
class VisualCodecVideoDecoder final : public iiSharedCanvas::VideoFrameDecoder {
public:
    iiSharedCanvas::DecodedVideoResult decode(std::span<const std::uint8_t> encoded,
                                              const iiSharedCanvas::VideoDecodeRequest &request) override;
};

} // namespace example
