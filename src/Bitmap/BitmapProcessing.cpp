#include "BitmapProcessing.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <numbers>

namespace iiSharedCanvas {
namespace {
constexpr std::uint64_t MaximumPixels = 67108864;
bool extentValid(CanvasExtent e) { return e.width > 0 && e.height > 0 && std::uint64_t(e.width) * e.height <= MaximumPixels; }
bool valid(const RasterLayer &p) { return extentValid({p.width, p.height}) && p.pixels.size() == std::size_t(p.width) * p.height; }
bool valid(const RasterMask &m) { return extentValid(m.extent) && m.alpha.size() == std::size_t(m.extent.width) * m.extent.height; }
double unit(double v) { return std::clamp(v, 0.0, 1.0); }
unsigned byte(double v) { return unsigned(std::lround(unit(v) * 255)); }
struct Rgb { double r, g, b; };
Rgb rgb(std::uint32_t p) { return {double((p >> 16) & 255) / 255, double((p >> 8) & 255) / 255, double(p & 255) / 255}; }
std::uint32_t pack(Rgb c, unsigned alpha) { return alpha ? (alpha << 24) | (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b) : 0; }
double luminance(Rgb c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
Rgb mix(Rgb a, Rgb b, double t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
double smooth(double a, double b, double x) { const double t = unit((x - a) / std::max(1e-9, b - a)); return t * t * (3 - 2 * t); }
std::uint32_t hash(std::uint32_t x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; return x ^ (x >> 16); }
double random(std::uint32_t x) { return double(hash(x)) / std::numeric_limits<std::uint32_t>::max(); }
Rgb sample(const RasterLayer &p, int x, int y) { return rgb(p.pixels[std::clamp(y, 0, p.height - 1) * p.width + std::clamp(x, 0, p.width - 1)]); }
// Running-window separable box convolution; three passes approximate Gaussian sigma.
RasterLayer blur(const RasterLayer &input, int radius) {
    auto p = input;
    radius = std::clamp(radius, 0, 256);
    if (!radius) return p;
    for (int axis = 0; axis < 2; ++axis) {
        auto out = p;
        const int lines = axis == 0 ? p.height : p.width, length = axis == 0 ? p.width : p.height;
        auto index = [&](int line, int point) { return axis == 0 ? line * p.width + point : point * p.width + line; };
        for (int line = 0; line < lines; ++line) {
            std::array<double, 4> sum{};
            auto accumulate = [&](int point, double sign) {
                auto px = p.pixels[index(line, std::clamp(point, 0, length - 1))];
                auto c = rgb(px); const double alpha = double(px >> 24) / 255;
                sum[0] += sign * c.r * alpha; sum[1] += sign * c.g * alpha;
                sum[2] += sign * c.b * alpha; sum[3] += sign * alpha;
            };
            for (int k = -radius; k <= radius; ++k) accumulate(k, 1);
            for (int point = 0; point < length; ++point) {
                const auto alpha = p.pixels[index(line, point)] >> 24;
                Rgb c{}; if (sum[3] > 1e-9) c = {sum[0] / sum[3], sum[1] / sum[3], sum[2] / sum[3]};
                out.pixels[index(line, point)] = pack(c, alpha);
                accumulate(point - radius, -1); accumulate(point + radius + 1, 1);
            }
        }
        p = std::move(out);
    }
    return p;
}
}

RasterProcessResult adjustRaster(const RasterLayer &input, const ColorAdjustment &a) {
    if (!valid(input)) return {{}, "invalid raster extent or storage"};
    const std::array params{a.exposure, a.offset, a.contrast, a.pivot, a.highlights, a.highlightRange,
        a.rolloff, a.shadows, a.shadowRange, a.blackProtection, a.whites, a.whiteClip, a.blacks,
        a.blackClip, a.temperature, a.tint, a.vibrance, a.lowSaturationBias, a.saturation,
        a.channelSaturation, a.skinProtection, a.gradeHue, a.gradeBalance, a.curve};
    if (std::ranges::any_of(params, [](double v) { return !std::isfinite(v) || std::abs(v) > 100; }))
        return {{}, "color parameters must be finite and bounded"};
    auto out = input;
    const auto exposure = std::exp2(a.exposure);
    for (std::size_t i = 0; i < input.pixels.size(); ++i) {
        const auto px = input.pixels[i]; auto c = rgb(px); const auto original = c;
        const double before = luminance(c);
        auto tone = [&](double v) {
            if (a.linearLight) v = std::pow(v, 2.2);
            v = v * exposure + a.offset;
            if (a.protectHighlights && a.exposure > 0) v = original.r == original.g && original.g == original.b
                ? std::min(v, before + (1 - before) * (1 - std::exp(-a.exposure))) : std::min(v, 1.0);
            v = (v - a.pivot) * (1 + a.contrast) + a.pivot;
            double hi = smooth(a.highlightRange * 0.8, 1, unit(v));
            double lo = 1 - smooth(0, std::max(0.01, a.shadowRange), unit(v));
            v += a.highlights * hi * 0.5 + a.shadows * lo * (1 - a.blackProtection * (1 - unit(v)));
            v += a.whites * smooth(a.whiteClip * 0.5, 1, unit(v));
            v += a.blacks * (1 - smooth(a.blackClip, 0.5, unit(v)));
            if (a.liftBlacks) v = std::max(v, std::max(0.0, a.blacks) * 0.1);
            if (a.rolloff > 0 && v > 0.8) v = 0.8 + 0.2 * (1 - std::exp(-(v - 0.8) / (0.2 + a.rolloff)));
            if (a.linearLight) v = std::pow(std::max(0.0, v), 1 / 2.2);
            return v;
        };
        c = {tone(c.r), tone(c.g), tone(c.b)};
        if (a.preserveColor && a.highlights != 0) {
            const double after = luminance(c), ratio = after / std::max(0.001, before);
            c = mix(c, {original.r * ratio, original.g * ratio, original.b * ratio}, 0.25);
        }
        c.r += a.temperature * 0.15 + a.tint * 0.05;
        c.b -= a.temperature * 0.15;
        c.g -= a.tint * 0.10;
        const double l = luminance(c), max = std::max({c.r, c.g, c.b}), min = std::min({c.r, c.g, c.b});
        const bool skin = original.r > original.g && original.g > original.b && original.r - original.b > 0.05;
        const double protect = skin ? 1 - unit(a.skinProtection) : 1;
        const double sat = 1 + (a.saturation + a.vibrance * (1 - unit(max - min)) * a.lowSaturationBias) * protect;
        c = {l + (c.r - l) * sat, l + (c.g - l) * sat, l + (c.b - l) * sat};
        double *channels[] = {&c.r, &c.g, &c.b};
        if (a.saturationChannel >= 0 && a.saturationChannel < 3)
            *channels[a.saturationChannel] = l + (*channels[a.saturationChannel] - l) * (1 + a.channelSaturation);
        if (a.colorize) {
            const double h = a.gradeHue * 2 * std::numbers::pi;
            c = mix(c, {l * (1 + 0.5 * std::cos(h)), l * (1 + 0.5 * std::cos(h - 2.094)), l * (1 + 0.5 * std::cos(h + 2.094))}, std::max(0.2, std::abs(a.gradeBalance)));
        } else if (a.gradeBalance != 0) {
            const double range = a.gradeRange == 0 ? 1 - smooth(0, 0.6, l) : a.gradeRange == 2 ? smooth(0.4, 1, l) : 1 - std::abs(l - 0.5) * 2;
            const double h = a.gradeHue * 2 * std::numbers::pi, t = a.gradeBalance * range * protect * 0.2;
            c.r += std::cos(h) * t; c.g += std::cos(h - 2.094) * t; c.b += std::cos(h + 2.094) * t;
        }
        for (int channel = 0; channel < 3; ++channel) if (a.curveChannel < 0 || channel == a.curveChannel) {
            double &v = *channels[channel];
            v += a.curve * (a.smoothCurve ? v * (1 - v) : 0.5 - std::abs(v - 0.5));
        }
        if (a.adaptive) c = mix(original, c, 0.5 + 0.5 * (1 - std::abs(before - 0.5) * 2));
        if (a.gamutLimit) c = {unit(c.r), unit(c.g), unit(c.b)};
        out.pixels[i] = pack(c, px >> 24);
    }
    return {std::move(out), {}};
}

RasterProcessResult effectRaster(const RasterLayer &input, const RasterEffect &e) {
    if (!valid(input)) return {{}, "invalid raster extent or storage"};
    for (double p : {e.amount, e.radius, e.angle, e.scale, e.detail, e.threshold, e.midpoint, e.roundness, e.feather})
        if (!std::isfinite(p)) return {{}, "effect parameters must be finite"};
    if (e.radius < 0 || e.radius > 256 || std::abs(e.amount) > 16 || e.scale <= 0 || e.scale > 65536)
        return {{}, "effect parameters exceed processing limits"};
    auto out = input;
    const int radius = int(std::ceil(e.radius));
    const bool spatial = e.kind == RasterEffectKind::GaussianBlur || e.kind == RasterEffectKind::LensBlur
        || e.kind == RasterEffectKind::Sharpen || e.kind == RasterEffectKind::Texture || e.kind == RasterEffectKind::Clarity
        || e.kind == RasterEffectKind::LuminanceDenoise || e.kind == RasterEffectKind::ColorDenoise;
    auto blurred = spatial ? blur(input, radius) : input;
    if (e.kind == RasterEffectKind::GaussianBlur || e.kind == RasterEffectKind::LensBlur) blurred = blur(blur(blurred, radius), radius);
    const double angle = e.angle * std::numbers::pi / 180;
    for (int y = 0; y < input.height; ++y) for (int x = 0; x < input.width; ++x) {
        const auto i = y * input.width + x; const auto px = input.pixels[i];
        const auto original = rgb(px); auto c = original, b = rgb(blurred.pixels[i]);
        double edge = std::abs(luminance(original) - luminance(b));
        switch (e.kind) {
        case RasterEffectKind::GaussianBlur: case RasterEffectKind::LensBlur:
            c = mix(original, b, unit(e.amount) * (e.edgeAware ? 1 - smooth(0.02, 0.2, edge) : 1)); break;
        case RasterEffectKind::MotionBlur: {
            Rgb total{}; const int steps = std::clamp(radius, 1, 32);
            for (int k = -steps; k <= steps; ++k) { auto s = sample(input, x + int(std::round(std::cos(angle) * k * e.radius / steps)), y + int(std::round(std::sin(angle) * k * e.radius / steps))); total.r += s.r; total.g += s.g; total.b += s.b; }
            c = mix(original, {total.r / (2 * steps + 1), total.g / (2 * steps + 1), total.b / (2 * steps + 1)}, unit(e.amount)); break;
        }
        case RasterEffectKind::Texture: case RasterEffectKind::Clarity: case RasterEffectKind::Sharpen: {
            double strength = e.amount * (e.kind == RasterEffectKind::Sharpen ? 2 : 1) * (1 + e.detail);
            strength *= smooth(e.threshold * 0.1, e.threshold * 0.1 + 0.1, edge);
            if (e.natural) strength *= 0.5;
            c = {c.r + (c.r - b.r) * strength, c.g + (c.g - b.g) * strength, c.b + (c.b - b.b) * strength}; break;
        }
        case RasterEffectKind::Dehaze: { const double strength = std::clamp(e.amount * (0.5 + e.detail), -3.0, 3.0); c = {(c.r - strength * 0.15) / (1 - strength * 0.3), (c.g - strength * 0.15) / (1 - strength * 0.3), (c.b - strength * 0.15) / (1 - strength * 0.3)}; break; }
        case RasterEffectKind::Vignette: {
            const double dx = (x + 0.5 - input.width / 2.0) / (input.width / 2.0), dy = (y + 0.5 - input.height / 2.0) / (input.height / 2.0);
            const double distance = std::sqrt(dx * dx + dy * dy * std::max(0.05, e.roundness));
            const double strength = e.amount * smooth(e.midpoint, e.midpoint + std::max(0.01, e.feather), distance);
            c = {c.r * (1 + strength), c.g * (1 + strength), c.b * (1 + strength)}; break;
        }
        case RasterEffectKind::Grain: case RasterEffectKind::Noise: {
            const std::uint32_t tile = std::uint32_t(int(x / e.scale) + int(y / e.scale) * input.width);
            const double nr = (random(tile ^ e.seed) - 0.5) * e.amount;
            const double ng = e.monochromatic ? nr : (random(tile ^ (e.seed + 13)) - 0.5) * e.amount;
            const double nb = e.monochromatic ? nr : (random(tile ^ (e.seed + 29)) - 0.5) * e.amount;
            const double response = e.kind == RasterEffectKind::Grain ? 1 - std::abs(luminance(c) - 0.5) : 1;
            c = {c.r + nr * response, c.g + ng * response, c.b + nb * response}; break;
        }
        case RasterEffectKind::LuminanceDenoise: {
            const double l = luminance(c), target = luminance(b), t = unit(e.amount) * (1 - smooth(e.detail * 0.1, 0.3, edge));
            c = {c.r + (target - l) * t, c.g + (target - l) * t, c.b + (target - l) * t}; break;
        }
        case RasterEffectKind::ColorDenoise: {
            const double l = luminance(c), target = luminance(b), t = unit(e.amount) * (1 - smooth(e.detail * 0.1, 0.3, edge));
            c = mix(c, {b.r + l - target, b.g + l - target, b.b + l - target}, t); break;
        }
        case RasterEffectKind::LensDistortion: {
            const double dx = (x - input.width / 2.0) / std::max(1, input.width), dy = (y - input.height / 2.0) / std::max(1, input.height);
            const double k = 1 + e.amount * (dx * dx + dy * dy);
            c = sample(input, int(std::round(input.width / 2.0 + dx * input.width * k)), int(std::round(input.height / 2.0 + dy * input.height * k))); break;
        }
        case RasterEffectKind::ChromaticAberration: {
            const int shift = int(std::round(e.amount * e.radius)); c.r = sample(input, x + shift, y).r; c.b = sample(input, x - shift, y).b; break;
        }
        }
        out.pixels[i] = pack(c, px >> 24);
    }
    return {std::move(out), {}};
}

RasterProcessResult fillRaster(CanvasExtent extent, const RasterFill &f) {
    if (!extentValid(extent) || !std::isfinite(f.opacity) || f.opacity < 0 || f.opacity > 1
        || !std::isfinite(f.scale) || f.scale <= 0 || !std::isfinite(f.angle)) return {{}, "invalid fill extent or parameters"};
    auto out = makeRasterLayer(extent.width, extent.height); const auto a = rgb(f.firstArgb), b = rgb(f.secondArgb);
    const double angle = f.angle * std::numbers::pi / 180, ca = std::cos(angle), sa = std::sin(angle);
    for (int y = 0; y < extent.height; ++y) for (int x = 0; x < extent.width; ++x) {
        const double dx = (x + 0.5) / extent.width - 0.5, dy = (y + 0.5) / extent.height - 0.5;
        double t = 0;
        if (f.kind == RasterFillKind::Linear) t = unit(0.5 + dx * ca + dy * sa);
        else if (f.kind == RasterFillKind::Radial) t = unit(std::hypot(dx, dy) * 2);
        else if (f.kind == RasterFillKind::Angular) { t = (std::atan2(dy, dx) - angle) / (2 * std::numbers::pi); t -= std::floor(t); }
        else if (f.kind == RasterFillKind::Pattern) {
            const double u = (x * ca + y * sa) / f.scale, v = (-x * sa + y * ca) / f.scale;
            t = f.seamless ? double((int(std::floor(u)) + int(std::floor(v))) & 1) : unit((std::sin(u * 2) + std::cos(v * 3)) / 4 + 0.5);
        }
        if (f.dither && f.kind != RasterFillKind::Solid) t = unit(t + (random(x + y * extent.width) - 0.5) / 255);
        const double alpha = ((f.firstArgb >> 24) * (1 - t) + (f.secondArgb >> 24) * t) / 255 * f.opacity;
        out.pixels[y * extent.width + x] = pack(mix(a, b, t), byte(alpha));
    }
    return {std::move(out), {}};
}
RasterProcessResult maskRaster(const RasterLayer &input, const RasterMask &mask) {
    if (!valid(input) || !valid(mask) || input.width != mask.extent.width || input.height != mask.extent.height) return {{}, "mask and raster dimensions must match"};
    auto out = input;
    for (std::size_t i = 0; i < out.pixels.size(); ++i) out.pixels[i] = pack(rgb(input.pixels[i]), unsigned(std::lround((input.pixels[i] >> 24) * mask.alpha[i] / 255.0)));
    return {std::move(out), {}};
}
RasterProcessResult blendRaster(const RasterLayer &base, const RasterLayer &processed, const RasterMask &mask) {
    if (!valid(base) || !valid(processed) || !valid(mask) || base.width != processed.width || base.height != processed.height
        || base.width != mask.extent.width || base.height != mask.extent.height) return {{}, "masked edit dimensions must match"};
    auto out = base;
    for (std::size_t i = 0; i < out.pixels.size(); ++i) {
        const double t = mask.alpha[i] / 255.0;
        if (mask.alpha[i] == 0) continue;
        if (mask.alpha[i] == 255) { out.pixels[i] = processed.pixels[i]; continue; }
        const double a = (base.pixels[i] >> 24) * (1 - t), b = (processed.pixels[i] >> 24) * t;
        const double alpha = a + b;
        out.pixels[i] = alpha > 0 ? pack(mix(rgb(base.pixels[i]), rgb(processed.pixels[i]), b / alpha), unsigned(std::lround(alpha))) : 0;
    }
    return {std::move(out), {}};
}
RasterMaskResult featherMask(const RasterMask &mask, double radius) {
    if (!valid(mask) || !std::isfinite(radius) || radius < 0 || radius > 256) return {{}, "invalid feather mask or radius"};
    auto pixels = makeRasterLayer(mask.extent.width, mask.extent.height);
    for (std::size_t i = 0; i < pixels.pixels.size(); ++i) pixels.pixels[i] = 0xff000000 | (mask.alpha[i] * 0x010101U);
    auto blurred = blur(pixels, int(std::ceil(radius))); auto out = mask;
    for (std::size_t i = 0; i < out.alpha.size(); ++i) out.alpha[i] = blurred.pixels[i] & 255;
    return {std::move(out), {}};
}
RasterMaskResult combineMasks(const RasterMask &base, const RasterMask &next, MaskOperation op) {
    if (!valid(base) || !valid(next) || base.extent.width != next.extent.width || base.extent.height != next.extent.height) return {{}, "mask dimensions must match"};
    auto out = base;
    for (std::size_t i = 0; i < out.alpha.size(); ++i) {
        if (op == MaskOperation::Replace) out.alpha[i] = next.alpha[i];
        else if (op == MaskOperation::Add) out.alpha[i] = std::max(base.alpha[i], next.alpha[i]);
        else if (op == MaskOperation::Subtract) out.alpha[i] = std::max(0, int(base.alpha[i]) - next.alpha[i]);
        else out.alpha[i] = std::min(base.alpha[i], next.alpha[i]);
    }
    return {std::move(out), {}};
}
RasterMaskResult floodMask(const RasterLayer &input, CanvasOrigin seed, double tolerance, bool contiguous) {
    if (!valid(input) || seed.x < 0 || seed.x >= input.width || seed.y < 0 || seed.y >= input.height || !std::isfinite(tolerance) || tolerance < 0 || tolerance > 1) return {{}, "invalid wand seed or tolerance"};
    RasterMask out{{input.width, input.height}, std::vector<std::uint8_t>(input.pixels.size())};
    const auto target = rgb(input.pixels[seed.y * input.width + seed.x]);
    auto matches = [&](std::size_t i) { auto c = rgb(input.pixels[i]); return std::max({std::abs(c.r - target.r), std::abs(c.g - target.g), std::abs(c.b - target.b)}) <= tolerance; };
    if (!contiguous) { for (std::size_t i = 0; i < out.alpha.size(); ++i) if (matches(i)) out.alpha[i] = 255; }
    else {
        std::vector<bool> visited(out.alpha.size()); std::deque<CanvasOrigin> queue{seed};
        while (!queue.empty()) { auto p = queue.front(); queue.pop_front(); const auto i = p.y * input.width + p.x;
            if (visited[i]) continue; visited[i] = true; if (!matches(i)) continue; out.alpha[i] = 255;
            if (p.x > 0) queue.push_back({p.x - 1, p.y}); if (p.x + 1 < input.width) queue.push_back({p.x + 1, p.y});
            if (p.y > 0) queue.push_back({p.x, p.y - 1}); if (p.y + 1 < input.height) queue.push_back({p.x, p.y + 1});
        }
    }
    return {std::move(out), {}};
}
} // namespace iiSharedCanvas
