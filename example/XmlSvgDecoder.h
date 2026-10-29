#pragma once

#include <Media/MediaIo.h>

#include <span>
#include <string>

namespace iiSharedCanvas::example {

struct CanvasSvgDecodeResult {
    VectorAsset asset;
    MediaIoResult result;
    [[nodiscard]] bool ok() const noexcept { return result.ok(); }
};

// Application-owned iiXml and iiCompression adapter for the canonical SVG/SVGZ
// emitted by encodeSvg.
// Unsupported SVG constructs are rejected instead of being silently discarded.
[[nodiscard]] CanvasSvgDecodeResult decodeCanvasSvg(
    std::span<const std::uint8_t> bytes, std::string assetId = "vector",
    const MediaLimits &limits = {});

} // namespace iiSharedCanvas::example
