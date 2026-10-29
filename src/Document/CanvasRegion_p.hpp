#pragma once

#include "Document/Document.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace iiSharedCanvas::detail {

// Union and align with 64-bit arithmetic before narrowing to document geometry.
inline std::optional<CanvasRegion> expandedCanvasRegion(CanvasRegion current,
                                                         CanvasRegion requested,
                                                         std::int32_t chunkSize) noexcept {
    if (current.extent.width <= 0 || current.extent.height <= 0 ||
        requested.extent.width <= 0 || requested.extent.height <= 0 || chunkSize <= 0)
        return std::nullopt;
    const auto floor = [chunkSize](std::int64_t value) {
        const auto remainder = value % chunkSize;
        return remainder < 0 ? value - remainder - chunkSize : value - remainder;
    };
    const auto ceil = [chunkSize](std::int64_t value) {
        const auto remainder = value % chunkSize;
        return remainder > 0 ? value - remainder + chunkSize : value - remainder;
    };
    const auto left = floor(std::min<std::int64_t>(current.origin.x, requested.origin.x));
    const auto top = floor(std::min<std::int64_t>(current.origin.y, requested.origin.y));
    const auto right = ceil(std::max(std::int64_t(current.origin.x) + current.extent.width,
                                     std::int64_t(requested.origin.x) + requested.extent.width));
    const auto bottom = ceil(std::max(std::int64_t(current.origin.y) + current.extent.height,
                                      std::int64_t(requested.origin.y) + requested.extent.height));
    const auto width = right - left, height = bottom - top;
    constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    if (left < minimum || top < minimum || right > maximum || bottom > maximum ||
        width <= 0 || height <= 0 || width > maximum || height > maximum)
        return std::nullopt;
    return CanvasRegion{{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top)},
                        {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)}};
}
} // namespace iiSharedCanvas::detail
