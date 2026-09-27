#pragma once
#include <Layer/RasterLayer.h>
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace iiSharedCanvas::detail {
inline std::string validateBinaryLineMask(std::int32_t width, std::int32_t height,
                                         std::span<const std::uint8_t> mask)
{
    if (width <= 0 || height <= 0 || std::uint64_t(width) * height != mask.size())
        return "binary line dimensions must be positive and match the sample count";
    if (std::any_of(mask.begin(), mask.end(), [](auto value) { return value > 1; }))
        return "binary line samples must be 0 (background) or 1 (line)";
    return {};
}
inline RasterLayer binaryLinePreview(std::int32_t width, std::int32_t height,
                                     std::span<const std::uint8_t> mask, std::uint64_t maxPixels)
{
    const auto message = validateBinaryLineMask(width, height, mask);
    if (!message.empty()) throw std::invalid_argument(message);
    if (mask.size() > maxPixels) throw std::length_error("binary line output exceeds maxPixels");
    auto pixels = makeRasterLayer(width, height, 0xff000000);
    for (std::size_t i = 0; i < mask.size(); ++i)
        pixels.pixels[i] = mask[i] ? 0xffffffff : 0xff000000;
    return pixels;
}
} // namespace iiSharedCanvas::detail
