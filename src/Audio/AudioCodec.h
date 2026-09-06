#pragma once

#include "Media/MediaIo.h"

#include <span>

namespace iiSharedCanvas {

struct AudioImportOptions {
    std::string assetId = "audio";
    MediaLimits limits;
};

struct AudioImportResult {
    AudioAsset asset;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

// Bounded RIFF/WAVE PCM16, mono or stereo. No implicit resampling or decoding
// through an external executable. Returned assets are detached document values.
IISHAREDCANVAS_EXPORT AudioImportResult decodeAudioWav(
    std::span<const std::uint8_t> bytes, const AudioImportOptions &options = {});
IISHAREDCANVAS_EXPORT AudioImportResult importAudioWav(
    const std::string &path, const AudioImportOptions &options = {});
IISHAREDCANVAS_EXPORT MediaBytesResult encodeAudioWav(
    const AudioAsset &asset, const MediaLimits &limits = {});
IISHAREDCANVAS_EXPORT MediaIoResult exportAudioWav(
    const AudioAsset &asset, const std::string &path, const MediaLimits &limits = {});

} // namespace iiSharedCanvas
