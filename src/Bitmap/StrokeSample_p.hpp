#pragma once

#include <Stroke/StrokePoint.h>
#include <Stroke/Rasterizer.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace iiSharedCanvas::detail {
inline StrokePoint legacyStrokeSample(DocumentPoint position, double pressure, double time)
{
    StrokePoint sample;
    sample.position = position;
    sample.pressure = pressure;
    sample.time = time;
    return sample;
}
inline bool validStrokeSample(const StrokePoint& sample)
{
    return std::isfinite(sample.position.x) && std::abs(sample.position.x) <= 1e9
        && std::isfinite(sample.position.y) && std::abs(sample.position.y) <= 1e9
        && std::isfinite(sample.time) && std::abs(sample.time) <= 1e12
        && std::isfinite(sample.pressure) && sample.pressure >= 0 && sample.pressure <= 1
        && std::isfinite(sample.velocity) && sample.velocity >= 0
        && std::isfinite(sample.arcLength) && sample.arcLength >= 0
        && std::isfinite(sample.tiltX) && std::abs(sample.tiltX) <= 1
        && std::isfinite(sample.tiltY) && std::abs(sample.tiltY) <= 1
        && std::isfinite(sample.rotationRadians)
        && std::isfinite(sample.tangentialPressure) && std::abs(sample.tangentialPressure) <= 1
        && std::all_of(sample.custom.begin(), sample.custom.end(),
                       [](double value) { return std::isfinite(value); });
}
inline bool clockCatchUp(const Rasterizer& value)
{
    return value.smoothingEnabled && value.smoothing > 0
        && value.smoothingMode == BrushSmoothingMode::PulledString && value.smoothingCatchUp;
}
inline bool rebaseStrokeClock(RasterDabStream& stream, double seconds)
{
    if (!stream.active || !std::isfinite(seconds) || std::abs(seconds) > 1e12
        || seconds < stream.previousPoint.time) return false;
    auto shifted = stream;
    const double delta = seconds - shifted.previousPoint.time;
    shifted.startTime += delta;
    shifted.nextDabTime += delta;
    shifted.previousPoint.time = seconds;
    if (!std::isfinite(shifted.startTime) || !std::isfinite(shifted.nextDabTime)) return false;
    stream = std::move(shifted);
    return true;
}
}
