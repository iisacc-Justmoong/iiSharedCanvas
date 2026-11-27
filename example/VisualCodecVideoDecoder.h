#pragma once

#include <Video/VideoFrameDecoder.h>

namespace example {

// Application-owned composition of iiSharedCanvas and iiVisualCodec. This
// accepts square-pixel video and canonical right-angle display matrices.
// Timing conversion is opt-in and never uses a process-global codec choice.
class VisualCodecVideoDecoder final : public iiSharedCanvas::VideoFrameDecoder {
public:
    explicit VisualCodecVideoDecoder(bool normalizeTiming = false, bool normalizeSampleAspect = false) noexcept
        : normalizeTiming_(normalizeTiming), normalizeSampleAspect_(normalizeSampleAspect) {}
    // Decodes borrowed bytes on the calling thread. Request limits, selection,
    // cancellation and the configured timing policy apply to the whole call.
    iiSharedCanvas::DecodedVideoResult decode(std::span<const std::uint8_t> encoded,
                                              const iiSharedCanvas::VideoDecodeRequest &request) override;
private:
    bool normalizeTiming_;
    bool normalizeSampleAspect_;
};

} // namespace example
