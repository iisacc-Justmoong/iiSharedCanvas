#include "Bitmap/ChannelExchange.h"

#include <Render/Compositor.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <stdexcept>

namespace iiSharedCanvas {
namespace {

bool validChannel(RasterChannel channel)
{
    return channel >= RasterChannel::Composite && channel <= RasterChannel::Luminance;
}

bool validRaster(const RasterLayer *image, std::size_t maximumPixels)
{
    if (!image || image->width <= 0 || image->height <= 0) return false;
    const auto width = static_cast<std::size_t>(image->width);
    const auto height = static_cast<std::size_t>(image->height);
    return height <= maximumPixels / width && image->pixels.size() == width * height;
}

bool validOptions(const ChannelCompositeOptions &options, const RasterLayer &target, bool scalar)
{
    if (!std::isfinite(options.opacity) || options.opacity < 0 || options.opacity > 1 ||
        static_cast<unsigned>(options.blendMode) > static_cast<unsigned>(RasterBlendMode::Dissolve)) return false;
    // Erasure is meaningful for a complete pixel, but not for a single scalar
    // channel. Nonseparable color modes also have no scalar-channel definition.
    if (scalar && (options.blendMode == RasterBlendMode::DestinationOut ||
        (options.blendMode >= RasterBlendMode::Hue && options.blendMode <= RasterBlendMode::Luminosity))) return false;
    if (!options.selection) return true;
    return options.selection->extent == CanvasExtent{target.width, target.height} &&
        options.selection->alpha.size() == target.pixels.size();
}

std::uint8_t channelValue(std::uint32_t pixel, RasterChannel channel)
{
    switch (channel) {
    case RasterChannel::Red: return static_cast<std::uint8_t>(pixel >> 16);
    case RasterChannel::Green: return static_cast<std::uint8_t>(pixel >> 8);
    case RasterChannel::Blue: return static_cast<std::uint8_t>(pixel);
    case RasterChannel::Alpha: return static_cast<std::uint8_t>(pixel >> 24);
    case RasterChannel::Composite:
    case RasterChannel::Luminance:
        return static_cast<std::uint8_t>(std::lround(.2126 * ((pixel >> 16) & 255U) +
            .7152 * ((pixel >> 8) & 255U) + .0722 * (pixel & 255U)));
    }
    return 0;
}

std::uint8_t value(const ChannelSource &source, std::size_t index)
{
    const auto component = channelValue(source.pixels->pixels[index], source.channel);
    return source.invert ? static_cast<std::uint8_t>(255U - component) : component;
}

std::uint32_t gray(std::uint8_t component)
{
    return 0xff000000U | (std::uint32_t(component) * 0x010101U);
}

unsigned shift(RasterChannel channel)
{
    switch (channel) {
    case RasterChannel::Red: return 16;
    case RasterChannel::Green: return 8;
    case RasterChannel::Blue: return 0;
    case RasterChannel::Alpha: return 24;
    default: return 0;
    }
}

bool aligned(const RasterLayer &a, const RasterLayer &b)
{
    return a.width == b.width && a.height == b.height;
}
} // namespace

RasterProcessResult ApplyImageComposite::operator()(const ChannelSource &source, const RasterLayer &target,
    RasterChannel targetChannel, const ChannelCompositeOptions &options) const
{
    if (!validChannel(source.channel) || !validChannel(targetChannel) ||
        targetChannel == RasterChannel::Luminance ||
        !validRaster(source.pixels, options.maximumPixels) || !validRaster(&target, options.maximumPixels) ||
        !aligned(*source.pixels, target) || !validOptions(options, target, targetChannel != RasterChannel::Composite))
        return {{}, "invalid image application, channel, geometry, mask, blend or budget"};
    try {
        RasterLayer result = target;
        for (std::size_t i = 0; i < result.pixels.size(); ++i) {
            const auto coverage = options.selection ? options.selection->alpha[i] : 255U;
            if (!coverage || options.opacity == 0) continue;
            const double opacity = options.opacity * double(coverage) / 255.;
            if (targetChannel == RasterChannel::Composite) {
                std::uint32_t foreground = source.pixels->pixels[i];
                if (source.channel != RasterChannel::Composite) foreground = gray(value(source, i));
                else if (source.invert) foreground ^= 0x00ffffffU;
                result.pixels[i] = compositeRasterPixel(foreground, target.pixels[i], options.blendMode,
                    opacity, static_cast<std::uint32_t>(i));
            } else {
                const auto component = channelValue(target.pixels[i], targetChannel);
                const auto blended = compositeRasterPixel(gray(value(source, i)), gray(component),
                    options.blendMode, opacity, static_cast<std::uint32_t>(i));
                const auto offset = shift(targetChannel);
                result.pixels[i] = (target.pixels[i] & ~(0xffU << offset)) | ((blended & 0xffU) << offset);
            }
        }
        return {std::move(result), {}};
    } catch (const std::bad_alloc &) {
        return {{}, "insufficient memory for image application"};
    } catch (const std::length_error &) {
        return {{}, "image application exceeds container limits"};
    }
}

RasterMaskResult ChannelCalculations::operator()(const ChannelSource &first, const ChannelSource &second,
    bool invertResult, const ChannelCompositeOptions &options) const
{
    if (!validChannel(first.channel) || !validChannel(second.channel) ||
        first.channel == RasterChannel::Composite || second.channel == RasterChannel::Composite ||
        !validRaster(first.pixels, options.maximumPixels) || !validRaster(second.pixels, options.maximumPixels) ||
        !aligned(*first.pixels, *second.pixels) || !validOptions(options, *second.pixels, true))
        return {{}, "invalid channel calculation, geometry, mask, blend or budget"};
    try {
        RasterMask result{{second.pixels->width, second.pixels->height}, {}};
        result.alpha.resize(second.pixels->pixels.size());
        for (std::size_t i = 0; i < result.alpha.size(); ++i) {
            const auto backdrop = value(second, i);
            const auto blended = compositeRasterPixel(gray(value(first, i)), gray(backdrop), options.blendMode,
                options.opacity, static_cast<std::uint32_t>(i));
            auto resultValue = blended & 255U;
            if (invertResult) resultValue = 255U - resultValue;
            const auto coverage = options.selection ? options.selection->alpha[i] : 255U;
            result.alpha[i] = static_cast<std::uint8_t>((resultValue * coverage + backdrop * (255U - coverage) + 127U) / 255U);
        }
        return {std::move(result), {}};
    } catch (const std::bad_alloc &) {
        return {{}, "insufficient memory for channel calculation"};
    } catch (const std::length_error &) {
        return {{}, "channel calculation exceeds container limits"};
    }
}
} // namespace iiSharedCanvas
