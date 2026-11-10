#include "BitmapProcessing.h"
#include "Bitmap/SelectionPolygon_p.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <numbers>
#include <queue>
#include <new>

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

RasterMaskResult selectionGeometry(CanvasExtent extent, SelectionGeometry shape, Point first, Point second, bool antialias) {
    if (!extentValid(extent) || (shape != SelectionGeometry::Rectangle && shape != SelectionGeometry::Ellipse) ||
        !std::isfinite(first.x) || !std::isfinite(first.y) || !std::isfinite(second.x) || !std::isfinite(second.y))
        return {{}, "invalid selection extent or geometry"};
    RasterMask mask{extent, std::vector<std::uint8_t>(std::size_t(extent.width) * extent.height, 0)};
    const double left = std::min(first.x, second.x), right = std::max(first.x, second.x);
    const double top = std::min(first.y, second.y), bottom = std::max(first.y, second.y);
    if (left == right || top == bottom) return {std::move(mask), {}};
    const double centerX = left / 2 + right / 2, centerY = top / 2 + bottom / 2;
    const double radiusX = right / 2 - left / 2, radiusY = bottom / 2 - top / 2;
    const auto limit = [](double value, int maximum) { return std::clamp(value, 0.0, double(maximum)); };
    const int x0 = int(std::floor(limit(left, extent.width))), x1 = int(std::ceil(limit(right, extent.width)));
    const int y0 = int(std::floor(limit(top, extent.height))), y1 = int(std::ceil(limit(bottom, extent.height)));
    const int samples = antialias ? 4 : 1, total = samples * samples;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        int coverage = 0;
        for (int sy = 0; sy < samples; ++sy) for (int sx = 0; sx < samples; ++sx) {
            const double px = x + (sx + 0.5) / samples, py = y + (sy + 0.5) / samples;
            bool inside = px >= left && px < right && py >= top && py < bottom;
            if (inside && shape == SelectionGeometry::Ellipse) {
                const double dx = (px - centerX) / radiusX, dy = (py - centerY) / radiusY;
                inside = dx * dx + dy * dy <= 1;
            }
            coverage += int(inside);
        }
        mask.alpha[std::size_t(y) * extent.width + x] = std::uint8_t((coverage * 255 + total / 2) / total);
    }
    return {std::move(mask), {}};
}

RasterMaskResult detail::selectionPolygonRegion(CanvasExtent domain, CanvasRegion region,
                                                   std::span<const Point> points, bool antialias) {
    constexpr std::size_t maxPoints = 65536;
    constexpr std::uint64_t maxEdgeScans = 50'000'000;
    constexpr double maxCoordinate = 1'000'000'000;
    if (domain.width<=0 || domain.height<=0 || !extentValid(region.extent) || region.origin.x<0 || region.origin.y<0 ||
        std::int64_t(region.origin.x)+region.extent.width>domain.width ||
        std::int64_t(region.origin.y)+region.extent.height>domain.height || points.size()<3 || points.size()>maxPoints)
        return {{}, "invalid selection polygon extent or point count"};
    double top=points.front().y,bottom=top;
    for(const auto& point:points) {
        if(!std::isfinite(point.x) || !std::isfinite(point.y) ||
           std::abs(point.x)>maxCoordinate || std::abs(point.y)>maxCoordinate)
            return {{}, "invalid selection polygon point"};
        top=std::min(top,point.y);bottom=std::max(bottom,point.y);
    }
    const int firstY=std::max(region.origin.y,int(std::floor(std::clamp(top,0.0,double(domain.height)))));
    const int lastY=std::min(region.origin.y+region.extent.height,int(std::ceil(std::clamp(bottom,0.0,double(domain.height)))));
    const int samples=antialias?4:1;
    if(lastY>firstY && std::uint64_t(lastY-firstY)*samples*points.size()>maxEdgeScans)
        return {{}, "selection polygon exceeds the edge scan budget"};
    RasterMask mask{region.extent,std::vector<std::uint8_t>(std::size_t(region.extent.width)*region.extent.height,0)};
    std::vector<double> crossings;crossings.reserve(points.size());
    for(int y=firstY;y<lastY;++y) for(int sy=0;sy<samples;++sy) {
        const double sampleY=y+(sy+0.5)/samples;crossings.clear();
        for(std::size_t i=0;i<points.size();++i) {
            const auto& a=points[i];const auto& b=points[(i+1)%points.size()];
            if((a.y<=sampleY && b.y>sampleY) || (b.y<=sampleY && a.y>sampleY)) {
                const double x=a.x+(b.x-a.x)*((sampleY-a.y)/(b.y-a.y));
                crossings.push_back(std::clamp(x,0.0,double(domain.width)));
            }
        }
        std::sort(crossings.begin(),crossings.end());
        for(std::size_t i=1;i<crossings.size();i+=2) {
            const double left=crossings[i-1],right=crossings[i];
            const int x0=std::max(region.origin.x,int(std::floor(left)));
            const int x1=std::min(region.origin.x+region.extent.width,int(std::ceil(right)));
            for(int x=x0;x<x1;++x)for(int sx=0;sx<samples;++sx) {
                const double sampleX=x+(sx+0.5)/samples;
                if(sampleX>=left && sampleX<right)
                    ++mask.alpha[std::size_t(y-region.origin.y)*region.extent.width+x-region.origin.x];
            }
        }
    }
    const int total=samples*samples;
    for(auto& alpha:mask.alpha)alpha=std::uint8_t((int(alpha)*255+total/2)/total);
    return {std::move(mask),{}};
}
RasterMaskResult selectionPolygon(CanvasExtent extent, std::span<const Point> points, bool antialias) {
    if (!extentValid(extent)) return {{}, "invalid selection polygon extent or point count"};
    return detail::selectionPolygonRegion(extent,{{0,0},extent},points,antialias);
}

RasterProcessResult adjustRaster(const RasterLayer &input, const ColorAdjustment &a) {
    if (!valid(input)) return {{}, "invalid raster extent or storage"};
    if (const auto error = validateColorAdjustment(a); !error.empty()) return {{}, error};
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
    if (const auto error = validateRasterEffect(e); !error.empty()) return {{}, error};
    auto out = input;
    const int radius = int(std::ceil(e.radius));
    const bool spatial = e.kind == RasterEffectKind::GaussianBlur || e.kind == RasterEffectKind::LensBlur
        || e.kind == RasterEffectKind::Sharpen || e.kind == RasterEffectKind::Texture || e.kind == RasterEffectKind::Clarity
        || e.kind == RasterEffectKind::LuminanceDenoise || e.kind == RasterEffectKind::ColorDenoise;
    auto blurred = spatial ? blur(input, radius) : input;
    if (e.kind == RasterEffectKind::GaussianBlur || e.kind == RasterEffectKind::LensBlur) blurred = blur(blur(blurred, radius), radius);
    Rgb average{};
    if (e.kind == RasterEffectKind::Average) {
        std::array<std::uint64_t, 3> sum{};
        for (auto p : input.pixels) { sum[0] += (p >> 16) & 255; sum[1] += (p >> 8) & 255; sum[2] += p & 255; }
        const auto count = input.pixels.size();
        average = {double((sum[0] + count/2) / count) / 255,
                   double((sum[1] + count/2) / count) / 255,
                   double((sum[2] + count/2) / count) / 255};
    }
    const double angle = e.angle * std::numbers::pi / 180;
    for (int y = 0; y < input.height; ++y) for (int x = 0; x < input.width; ++x) {
        const auto i = y * input.width + x; const auto px = input.pixels[i];
        const auto original = rgb(px); auto c = original, b = rgb(blurred.pixels[i]);
        double edge = std::abs(luminance(original) - luminance(b));
        switch (e.kind) {
        case RasterEffectKind::Average: c = mix(original, average, unit(e.amount)); break;
        case RasterEffectKind::Solarize: {
            const auto solar = [](double v) { return v < 128.0/255 ? v : 1-v; };
            c = mix(original, {solar(c.r), solar(c.g), solar(c.b)}, unit(e.amount)); break;
        }
        case RasterEffectKind::Invert: c = mix(original, {1-c.r, 1-c.g, 1-c.b}, unit(e.amount)); break;
        case RasterEffectKind::Grayscale: {
            const auto l = luminance(c); c = mix(original, {l,l,l}, unit(e.amount)); break;
        }
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
            const std::uint32_t tile = std::uint32_t(std::fmod(std::floor(x / e.scale)
                + std::floor(y / e.scale) * input.width, 4294967296.0));
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
RasterMaskResult antialiasBinaryMask(const RasterMask &mask) {
    if (!valid(mask) || std::ranges::any_of(mask.alpha, [](auto a) { return a != 0 && a != 255; }))
        return {{}, "antialias requires a valid binary mask"};
    auto out = mask;
    const int width=mask.extent.width, height=mask.extent.height;
    const auto selected = [&](int x, int y) {
        x=std::clamp(x,0,width-1); y=std::clamp(y,0,height-1);
        return mask.alpha[std::size_t(y)*width+x] == 255;
    };
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        const bool center=selected(x,y); bool edge=false;
        for (int dy=-1;dy<=1 && !edge;++dy) for (int dx=-1;dx<=1;++dx)
            if (selected(x+dx,y+dy)!=center) {edge=true;break;}
        if (!edge) continue;
        unsigned inside=0;
        for (int sy=0;sy<4;++sy) for (int sx=0;sx<4;++sx) {
            // Bilinear reconstruction of center samples; the 0.5 contour
            // defines the boundary. Integer weights avoid platform drift.
            const int x0=x+(sx<2 ? -1 : 0), y0=y+(sy<2 ? -1 : 0);
            const int fx=sx<2 ? 2*sx+5 : 2*sx-3;
            const int fy=sy<2 ? 2*sy+5 : 2*sy-3;
            const int score=selected(x0,y0)*(8-fx)*(8-fy) + selected(x0+1,y0)*fx*(8-fy) +
                            selected(x0,y0+1)*(8-fx)*fy + selected(x0+1,y0+1)*fx*fy;
            inside += score >= 32;
        }
        out.alpha[std::size_t(y)*width+x]=std::uint8_t((inside*255+8)/16);
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
RasterMaskResult selectionWand(const RasterLayer &input, CanvasOrigin seed, double tolerance, bool contiguous) {
    if (!valid(input) || seed.x < 0 || seed.x >= input.width || seed.y < 0 || seed.y >= input.height ||
        !std::isfinite(tolerance) || tolerance < 0 || tolerance > 1)
        return {{}, "invalid wand seed or tolerance"};
    const auto components = [](std::uint32_t pixel) {
        const double alpha = double(pixel >> 24) / 255.0;
        const auto color = rgb(pixel);
        return std::array<double, 4>{color.r * alpha, color.g * alpha, color.b * alpha, alpha};
    };
    const auto target = components(input.pixels[std::size_t(seed.y) * input.width + seed.x]);
    const auto matches = [&](std::size_t index) {
        const auto color = components(input.pixels[index]);
        double distance = 0;
        for (std::size_t n = 0; n < color.size(); ++n)
            distance = std::max(distance, std::abs(color[n] - target[n]));
        return distance <= tolerance;
    };
    RasterMask out{{input.width, input.height}, std::vector<std::uint8_t>(input.pixels.size())};
    if (!contiguous) {
        for (std::size_t i = 0; i < out.alpha.size(); ++i) if (matches(i)) out.alpha[i] = 255;
    } else {
        const auto start = std::size_t(seed.y) * input.width + seed.x;
        std::deque<std::size_t> queue{start}; out.alpha[start] = 1;
        const auto visit = [&](std::size_t index) {
            if (!out.alpha[index]) { out.alpha[index] = 1; queue.push_back(index); }
        };
        while (!queue.empty()) {
            const auto index = queue.front(); queue.pop_front();
            if (!matches(index)) continue;
            out.alpha[index] = 255;
            const auto x = index % std::size_t(input.width), y = index / std::size_t(input.width);
            if (x > 0) visit(index - 1);
            if (x + 1 < std::size_t(input.width)) visit(index + 1);
            if (y > 0) visit(index - std::size_t(input.width));
            if (y + 1 < std::size_t(input.height)) visit(index + std::size_t(input.width));
        }
        for (auto &coverage : out.alpha) if (coverage == 1) coverage = 0;
    }
    return {std::move(out), {}};
}
RasterProcessResult gradientRaster(CanvasExtent extent, const GradientDefinition& gradient) {
    const auto finitePoint = [](Point p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::abs(p.x) <= 1e9 && std::abs(p.y) <= 1e9; };
    if (!extentValid(extent) || !finitePoint(gradient.start) || !finitePoint(gradient.end) ||
        !std::isfinite(gradient.opacity) || gradient.opacity < 0 || gradient.opacity > 1 ||
        (gradient.kind != GradientKind::Linear && gradient.kind != GradientKind::Radial) ||
        (gradient.spread != GradientSpread::Pad && gradient.spread != GradientSpread::Repeat) ||
        gradient.stops.size() < 2 || gradient.stops.size() > 256)
        return {{}, "invalid gradient extent or parameters"};
    const auto dx = gradient.end.x - gradient.start.x, dy = gradient.end.y - gradient.start.y;
    const auto lengthSquared = dx * dx + dy * dy;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1e-18) return {{}, "gradient endpoints must differ"};
    double previous = -1;
    for (const auto& stop : gradient.stops) {
        if (!std::isfinite(stop.position) || stop.position < 0 || stop.position > 1 || stop.position <= previous)
            return {{}, "gradient stops must be strictly increasing within zero and one"};
        previous = stop.position;
    }
    const auto colorAt = [&](double t) {
        auto upper = std::upper_bound(gradient.stops.begin(), gradient.stops.end(), t,
                                     [](double position, const GradientStop& stop) { return position < stop.position; });
        const GradientStop* a; const GradientStop* b;
        if (upper == gradient.stops.begin()) a = b = &gradient.stops.front();
        else if (upper == gradient.stops.end()) a = b = &gradient.stops.back();
        else { b = &*upper; a = &*(upper - 1); }
        const double f = a == b ? 0 : (t - a->position) / (b->position - a->position);
        const auto ca = rgb(a->argb), cb = rgb(b->argb);
        const double aa = double(a->argb >> 24) / 255, ab = double(b->argb >> 24) / 255;
        const double alpha = aa + (ab - aa) * f;
        Rgb color{};
        if (alpha > 0) color = {(ca.r * aa * (1 - f) + cb.r * ab * f) / alpha,
                                (ca.g * aa * (1 - f) + cb.g * ab * f) / alpha,
                                (ca.b * aa * (1 - f) + cb.b * ab * f) / alpha};
        return pack(color, byte(alpha * gradient.opacity));
    };
    auto pixels = makeRasterLayer(extent.width, extent.height);
    const double length = std::sqrt(lengthSquared);
    for (int y = 0; y < extent.height; ++y) for (int x = 0; x < extent.width; ++x) {
        const double px = x + .5 - gradient.start.x, py = y + .5 - gradient.start.y;
        double t = gradient.kind == GradientKind::Linear ? (px * dx + py * dy) / lengthSquared
                                                        : std::hypot(px, py) / length;
        if (gradient.spread == GradientSpread::Repeat) t -= std::floor(t); else t = unit(t);
        pixels.pixels[std::size_t(y) * extent.width + x] = colorAt(t);
    }
    return {std::move(pixels), {}};
}
MagneticPathResult magneticSelectionPath(const RasterLayer& image, Point start, Point end, const MagneticPathOptions& options)
{
    constexpr std::size_t hardLimit = 1048576;
    if (!valid(image) || !std::isfinite(start.x) || !std::isfinite(start.y) ||
        !std::isfinite(end.x) || !std::isfinite(end.y) || !std::isfinite(options.radius) ||
        options.radius < 1 || options.radius > 64 || !std::isfinite(options.minimumStrength) ||
        options.minimumStrength < 0 || options.minimumStrength > 1 || !options.maxSearchPixels)
        return {{},false,"Invalid magnetic path input"};
    const auto clampPoint = [&](Point p) { return Point{std::clamp(p.x,0.,double(image.width-1)),std::clamp(p.y,0.,double(image.height-1))}; };
    start=clampPoint(start);end=clampPoint(end);
    const int radius=int(std::ceil(options.radius));
    const int sx=int(std::floor(start.x)),sy=int(std::floor(start.y));
    const int ex=int(std::floor(end.x)),ey=int(std::floor(end.y));
    const int left=std::max(0,std::min(sx,ex)-radius),top=std::max(0,std::min(sy,ey)-radius);
    const int right=std::min(image.width-1,std::max(sx,ex)+radius),bottom=std::min(image.height-1,std::max(sy,ey)+radius);
    const int width=right-left+1,height=bottom-top+1;
    const auto count=std::uint64_t(width)*height;
    if (count>std::min<std::size_t>(options.maxSearchPixels,hardLimit))
        return {{},false,"Magnetic path exceeds the search budget"};
    try {
        // Compare premultiplied channels, so RGB hidden beneath zero alpha has no influence.
        const auto channels=[&](int x,int y) {
            x=std::clamp(x,0,image.width-1);y=std::clamp(y,0,image.height-1);
            const auto p=image.pixels[std::size_t(y)*image.width+x];const unsigned a=p>>24;
            return std::array<unsigned,4>{a,((p>>16)&255)*a/255,((p>>8)&255)*a/255,(p&255)*a/255};
        };
        const auto gradient=[&](int x,int y) {
            const auto l=channels(x-1,y),r=channels(x+1,y),u=channels(x,y-1),d=channels(x,y+1);unsigned strength=0;
            for(int i=0;i<4;++i) strength=std::max({strength,unsigned(std::abs(int(l[i])-int(r[i]))),unsigned(std::abs(int(u[i])-int(d[i])))});
            return double(strength)/255.;
        };
        std::vector<double> edges(static_cast<std::size_t>(count));double maximum=0;
        const auto index=[&](int x,int y) { return std::size_t(y-top)*width+std::size_t(x-left); };
        for(int y=top;y<=bottom;++y) for(int x=left;x<=right;++x) {
            const auto v=gradient(x,y);edges[index(x,y)]=v;maximum=std::max(maximum,v);
        }
        if(maximum<=0 || maximum<options.minimumStrength) return {{start,end},false,{}};
        const auto snap=[&](int x,int y) {
            int bx=x,by=y;double best=-1;
            for(int yy=std::max(top,y-radius);yy<=std::min(bottom,y+radius);++yy)
                for(int xx=std::max(left,x-radius);xx<=std::min(right,x+radius);++xx) {
                    const double distance=std::hypot(double(xx-x),double(yy-y));if(distance>options.radius)continue;
                    const auto edge=edges[index(xx,yy)];if(edge<options.minimumStrength || edge<=0)continue;
                    const double score=edge-.2*distance/options.radius;
                    if(score>best) {best=score;bx=xx;by=yy;}
                }
            return std::pair{bx,by};
        };
        const auto [ax,ay]=snap(sx,sy);const auto [bx,by]=snap(ex,ey);
        const auto startIndex=index(ax,ay),goalIndex=index(bx,by);
        struct Node { double f,g;std::size_t index; };
        struct Later { bool operator()(const Node& a,const Node& b)const { if(a.f!=b.f)return a.f>b.f;if(a.g!=b.g)return a.g>b.g;return a.index>b.index; } };
        std::priority_queue<Node,std::vector<Node>,Later> queue;
        std::vector<double> distance(static_cast<std::size_t>(count),std::numeric_limits<double>::infinity());
        std::vector<std::size_t> previous(static_cast<std::size_t>(count),static_cast<std::size_t>(count));
        const auto estimate=[&](int x,int y){return std::hypot(double(x-bx),double(y-by));};
        distance[startIndex]=0;queue.push({estimate(ax,ay),0,startIndex});
        constexpr std::array<std::pair<int,int>,8> directions{{{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{1,-1},{-1,1},{1,1}}};
        while(!queue.empty()) {
            const auto node=queue.top();queue.pop();if(node.g!=distance[node.index])continue;if(node.index==goalIndex)break;
            const int x=left+int(node.index%width),y=top+int(node.index/width);
            for(auto [dx,dy]:directions) {
                const int nx=x+dx,ny=y+dy;if(nx<left||nx>right||ny<top||ny>bottom)continue;
                const auto next=index(nx,ny);const double step=(dx&&dy)?std::numbers::sqrt2:1.;
                const auto cost=node.g+step*(1.+8.*(1.-edges[next]));
                if(cost<distance[next]) {distance[next]=cost;previous[next]=node.index;queue.push({cost+estimate(nx,ny),cost,next});}
            }
        }
        if(!std::isfinite(distance[goalIndex]))return {{},false,"Magnetic path could not be traced"};
        std::vector<Point> path;for(auto current=goalIndex;;current=previous[current]) {
            path.push_back({double(left+int(current%width))+.5,double(top+int(current/width))+.5});
            if(current==startIndex)break;if(previous[current]>=count)return {{},false,"Magnetic path is incomplete"};
        }
        std::reverse(path.begin(),path.end());if(path.size()==1)path.push_back(path.front());
        return {std::move(path),true,{}};
    } catch(const std::bad_alloc&) { return {{},false,"Insufficient memory for magnetic path"}; }
}
} // namespace iiSharedCanvas
