#include "CanvasSampling.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace iiSharedCanvas {
namespace {
// 64 x 32 -> 96 bits, represented as (high, low). This avoids compiler-specific
// integer types and floating-point rounding at rational video frame boundaries.
std::pair<std::uint64_t, std::uint64_t> product(std::uint64_t value, std::uint32_t factor) noexcept
{
    const std::uint64_t low = (value & 0xffffffffULL) * factor;
    const std::uint64_t high = (value >> 32) * factor;
    const std::uint64_t sum = low + (high << 32);
    return {(high >> 32) + (sum < low ? 1 : 0), sum};
}

MotionValue motionAt(const std::vector<MotionKeyframe> &keys, FrameIndex frame) noexcept
{
    if (keys.empty()) { return {}; }
    const auto right = std::upper_bound(keys.begin(), keys.end(), frame,
        [](FrameIndex position, const MotionKeyframe &key) { return position < key.frame; });
    if (right == keys.begin()) { return right->value; }
    const auto &left = *(right - 1);
    if (right == keys.end() || left.interpolation == MotionInterpolation::Hold) { return left.value; }
    double t = static_cast<double>(frame - left.frame) / (right->frame - left.frame);
    if (left.interpolation == MotionInterpolation::SmoothStep) { t = t * t * (3 - 2 * t); }
    const auto point = [t](Point a, Point b) {
        return Point{std::lerp(a.x, b.x, t), std::lerp(a.y, b.y, t)};
    };
    return {point(left.value.position, right->value.position),
            point(left.value.scale, right->value.scale), point(left.value.anchor, right->value.anchor),
            std::lerp(left.value.rotationDegrees, right->value.rotationDegrees, t),
            std::lerp(left.value.opacity, right->value.opacity, t)};
}
}

std::optional<FrameIndex> videoFrameIndexAt(
    const Document &document, const VideoLayer &layer, FrameIndex frame) noexcept
{
    const auto &range = layer.properties.frameRange;
    if (frame >= document.timeline.frameCount
        || (range && (frame < range->firstFrame || frame > range->lastFrame))) { return {}; }
    const auto *source = std::get_if<StaticSource>(&layer.source);
    const auto *asset = source ? findVideoAsset(document, source->assetId) : nullptr;
    if (!asset || asset->frames.empty() || asset->frames.size() > std::numeric_limits<FrameIndex>::max()
        || !asset->frameRate.numerator || !asset->frameRate.denominator
        || !document.timeline.frameRate.numerator || !document.timeline.frameRate.denominator) { return {}; }
    const auto end = layer.playback.sourceOutFrame.value_or(static_cast<FrameIndex>(asset->frames.size()));
    if (end > asset->frames.size() || layer.playback.sourceInFrame >= end) { return {}; }
    const FrameIndex count = end - layer.playback.sourceInFrame;
    const FrameIndex elapsed = frame - (range ? range->firstFrame : 0);
    const std::uint64_t numerator = std::uint64_t(asset->frameRate.numerator) * document.timeline.frameRate.denominator;
    const std::uint64_t denominator = std::uint64_t(asset->frameRate.denominator) * document.timeline.frameRate.numerator;
    const auto ticks = product(numerator, elapsed);
    // Only the source range matters. Binary-search the exact quotient capped at count.
    std::uint64_t low = 0, high = count;
    while (low < high) {
        const auto middle = static_cast<FrameIndex>(low + (high - low + 1) / 2);
        if (product(denominator, middle) <= ticks) { low = middle; }
        else { high = middle - 1; }
    }
    if (low == count) {
        return layer.playback.endBehavior == VideoEndBehavior::Hold
            ? std::optional<FrameIndex>{end - 1} : std::nullopt;
    }
    return layer.playback.sourceInFrame + static_cast<FrameIndex>(low);
}

const RasterLayer *resolveVideoFrameAt(
    const Document &document, const VideoLayer &layer, FrameIndex frame) noexcept
{
    const auto index = videoFrameIndexAt(document, layer, frame);
    if (!index) { return nullptr; }
    return &findVideoAsset(document, std::get<StaticSource>(layer.source).assetId)->frames[*index];
}

LayerSample sampleLayerAt(const Document &document, const Layer &layer, FrameIndex frame) noexcept
{
    const auto &properties = layerProperties(layer);
    LayerSample sample{properties.visible && layerExistsAt(document, layer, frame),
                       properties.opacity, properties.transform};
    if (const auto *video = std::get_if<VideoLayer>(&layer)) {
        sample.visible = sample.visible && videoFrameIndexAt(document, *video, frame).has_value();
    }
    if (properties.motion.empty()) { return sample; }
    const auto value = motionAt(properties.motion, frame);
    const double angle = std::remainder(value.rotationDegrees, 360.0) * std::numbers::pi / 180.0;
    const double sine = std::sin(angle), cosine = std::cos(angle);
    const double a = cosine * value.scale.x, b = sine * value.scale.x;
    const double c = -sine * value.scale.y, d = cosine * value.scale.y;
    const double x = value.position.x + value.anchor.x - a * value.anchor.x - c * value.anchor.y;
    const double y = value.position.y + value.anchor.y - b * value.anchor.x - d * value.anchor.y;
    const auto &base = properties.transform;
    sample.transform.m11 = base.m11 * a + base.m21 * b;
    sample.transform.m12 = base.m12 * a + base.m22 * b;
    sample.transform.m21 = base.m11 * c + base.m21 * d;
    sample.transform.m22 = base.m12 * c + base.m22 * d;
    sample.transform.translationX = base.m11 * x + base.m21 * y + base.translationX;
    sample.transform.translationY = base.m12 * x + base.m22 * y + base.translationY;
    sample.opacity *= value.opacity;
    return sample;
}
} // namespace iiSharedCanvas
