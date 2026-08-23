#pragma once

#include "Document/Document.h"
#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace iiSharedCanvas {

inline constexpr std::size_t IiscHeaderSize = 32;

struct SerializationLimits {
    std::uint64_t maximumContainerBytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumCanvasPixels = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTotalRasterPixels = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTotalVectorPaths = 1024ULL * 1024ULL;
    std::uint64_t maximumTotalPathCommands = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTotalKeyframes = 16ULL * 1024ULL * 1024ULL;
    std::uint64_t maximumTotalStringBytes = 64ULL * 1024ULL * 1024ULL;
    std::uint32_t maximumStringBytes = 1024U * 1024U;
    std::uint32_t maximumAssets = 65536U;
    std::uint32_t maximumLayers = 65536U;
    std::uint32_t maximumRasterChunks = 1048576U;
};

enum class IiscErrorCode {
    None,
    InvalidDocument,
    UnsupportedVersion,
    InvalidHeader,
    TruncatedData,
    ChecksumMismatch,
    LimitExceeded,
    InvalidData,
    TrailingData,
};

struct IiscError {
    IiscErrorCode code = IiscErrorCode::None;
    std::uint64_t offset = 0;
    std::string message;
};

struct IiscEncodeResult {
    std::vector<std::uint8_t> bytes;
    IiscError error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.code == IiscErrorCode::None;
    }
};

struct IiscDecodeResult {
    Document document;
    IiscError error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.code == IiscErrorCode::None;
    }
};

IISHAREDCANVAS_EXPORT IiscEncodeResult encodeIisc(
    const Document &document,
    SerializationLimits limits = {});
IISHAREDCANVAS_EXPORT IiscDecodeResult decodeIisc(
    std::span<const std::uint8_t> bytes,
    SerializationLimits limits = {});

} // namespace iiSharedCanvas
