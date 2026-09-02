#include "Timeline/TimelineProject.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace iiSharedCanvas {
namespace {

void addIssue(TimelineValidationResult &result,
              TimelineValidationCode code,
              std::string path,
              std::string message)
{
    result.issues.push_back({code, std::move(path), std::move(message)});
}

bool finite(double value) noexcept
{
    return std::isfinite(value);
}

bool validTimeBase(const TimelineTimeBase &timeBase) noexcept
{
    return timeBase.numerator > 0 && timeBase.denominator > 0;
}

bool validFrameRate(const TimelineFrameRate &rate) noexcept
{
    return rate.numerator > 0 && rate.denominator > 0;
}

bool validRational(const TimelineRational &value,
                   bool allowZeroNumerator = false) noexcept
{
    return value.denominator > 0
        && (allowZeroNumerator || value.numerator != 0);
}

std::uint64_t signedMagnitude(std::int64_t value) noexcept
{
    return value < 0
        ? static_cast<std::uint64_t>(-(value + 1)) + 1U
        : static_cast<std::uint64_t>(value);
}

bool clipDurationsMatch(std::int64_t timelineDuration,
                        TimelineTimeBase timelineTimeBase,
                        std::int64_t sourceDuration,
                        TimelineTimeBase sourceTimeBase,
                        TimelineRational playbackRate) noexcept
{
    if (timelineDuration <= 0 || sourceDuration <= 0
        || !validTimeBase(timelineTimeBase)
        || !validTimeBase(sourceTimeBase)
        || !validRational(playbackRate)) {
        return false;
    }

    std::array<std::uint64_t, 4> sourceFactors{
        static_cast<std::uint64_t>(sourceDuration),
        sourceTimeBase.numerator,
        timelineTimeBase.denominator,
        playbackRate.denominator,
    };
    std::array<std::uint64_t, 4> timelineFactors{
        static_cast<std::uint64_t>(timelineDuration),
        timelineTimeBase.numerator,
        sourceTimeBase.denominator,
        signedMagnitude(playbackRate.numerator),
    };
    for (std::uint64_t &sourceFactor : sourceFactors) {
        for (std::uint64_t &timelineFactor : timelineFactors) {
            const std::uint64_t divisor = std::gcd(sourceFactor, timelineFactor);
            sourceFactor /= divisor;
            timelineFactor /= divisor;
        }
    }
    return std::all_of(sourceFactors.begin(), sourceFactors.end(),
                       [](std::uint64_t factor) { return factor == 1U; })
        && std::all_of(timelineFactors.begin(), timelineFactors.end(),
                       [](std::uint64_t factor) { return factor == 1U; });
}

bool frameRateLessOrEqual(const TimelineFrameRate &left,
                          const TimelineFrameRate &right) noexcept
{
    if (!validFrameRate(left) || !validFrameRate(right)) {
        return false;
    }
    return static_cast<std::uint64_t>(left.numerator) * right.denominator
        <= static_cast<std::uint64_t>(right.numerator) * left.denominator;
}

std::optional<std::int64_t> rangeEnd(const TimelineTickRange &range) noexcept
{
    if (range.duration < 0
        || range.start > std::numeric_limits<std::int64_t>::max() - range.duration) {
        return std::nullopt;
    }
    return range.start + range.duration;
}

bool rangeInside(const TimelineTickRange &range,
                 std::int64_t ownerStart,
                 std::int64_t ownerDuration,
                 bool allowZeroDuration) noexcept
{
    if (ownerDuration < 0
        || range.duration < (allowZeroDuration ? 0 : 1)
        || range.start < ownerStart) {
        return false;
    }
    const auto end = rangeEnd(range);
    if (!end
        || ownerStart > std::numeric_limits<std::int64_t>::max() - ownerDuration) {
        return false;
    }
    return *end <= ownerStart + ownerDuration;
}

bool rangesIntersect(const TimelineTickRange &left,
                     const TimelineTickRange &right) noexcept
{
    const auto leftEnd = rangeEnd(left);
    const auto rightEnd = rangeEnd(right);
    return leftEnd && rightEnd
        && left.start < *rightEnd
        && right.start < *leftEnd;
}

bool finite(const TimelineVector2 &value) noexcept
{
    return finite(value.x) && finite(value.y);
}

bool finite(const TimelineColor &value) noexcept
{
    return finite(value.red) && finite(value.green)
        && finite(value.blue) && finite(value.alpha);
}

bool normalized(const TimelineColor &value) noexcept
{
    return finite(value)
        && value.red >= 0.0 && value.red <= 1.0
        && value.green >= 0.0 && value.green <= 1.0
        && value.blue >= 0.0 && value.blue <= 1.0
        && value.alpha >= 0.0 && value.alpha <= 1.0;
}

bool finite(const TimelineBezierTangent &value) noexcept
{
    return finite(value.timeOffset) && finite(value.valueOffset);
}

bool finite(const TimelinePropertyValue &value) noexcept
{
    return std::visit([](const auto &stored) {
        using Value = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<Value, double>) {
            return finite(stored);
        } else if constexpr (std::is_same_v<Value, TimelineColor>
                             || std::is_same_v<Value, TimelineVector2>) {
            return finite(stored);
        } else if constexpr (std::is_same_v<Value, TimelineRational>) {
            return stored.denominator > 0;
        } else if constexpr (std::is_same_v<Value, std::vector<double>>) {
            return std::all_of(stored.begin(), stored.end(), [](double number) {
                return finite(number);
            });
        }
        return true;
    }, value);
}

bool supportsContinuousInterpolation(const TimelinePropertyValue &value) noexcept
{
    return std::visit([](const auto &stored) {
        using Value = std::decay_t<decltype(stored)>;
        return std::is_same_v<Value, std::int64_t>
            || std::is_same_v<Value, double>
            || std::is_same_v<Value, TimelineRational>
            || std::is_same_v<Value, TimelineColor>
            || std::is_same_v<Value, TimelineVector2>
            || std::is_same_v<Value, std::vector<double>>;
    }, value);
}

void validateProperties(const std::vector<TimelineProperty> &properties,
                        const std::string &path,
                        TimelineValidationResult &result)
{
    std::unordered_set<std::string> keys;
    for (std::size_t index = 0; index < properties.size(); ++index) {
        const TimelineProperty &property = properties[index];
        const std::string propertyPath = path + "[" + std::to_string(index) + "]";
        if (property.key.empty()) {
            addIssue(result, TimelineValidationCode::DuplicateProperty,
                     propertyPath + ".key", "property keys must not be empty");
        } else if (!keys.insert(property.key).second) {
            addIssue(result, TimelineValidationCode::DuplicateProperty,
                     propertyPath + ".key", "property keys must be unique in their scope");
        }
        if (!finite(property.value)) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     propertyPath + ".value", "property values must be finite and rational denominators non-zero");
        }
    }
}

void validateFrameRate(const TimelineFrameRate &rate,
                       const std::string &path,
                       TimelineValidationResult &result)
{
    if (!validFrameRate(rate)) {
        addIssue(result, TimelineValidationCode::InvalidFrameRate, path,
                 "frame-rate numerator and denominator must be positive");
    }
}

void validateTimeBase(const TimelineTimeBase &timeBase,
                      const std::string &path,
                      TimelineValidationResult &result)
{
    if (!validTimeBase(timeBase)) {
        addIssue(result, TimelineValidationCode::InvalidTimeBase, path,
                 "time-base numerator and denominator must be positive");
    }
}

void validateTimecode(const TimelineTimecode &timecode,
                      const std::string &path,
                      TimelineValidationResult &result)
{
    validateFrameRate(timecode.frameRate, path + ".frameRate", result);
    if (timecode.nominalFramesPerSecond == 0) {
        addIssue(result, TimelineValidationCode::InvalidTimecode,
                 path + ".nominalFramesPerSecond",
                 "timecode nominal frame count must be positive");
    }
    if (validFrameRate(timecode.frameRate)) {
        const std::uint64_t doubledNumerator
            = static_cast<std::uint64_t>(timecode.frameRate.numerator) * 2U;
        const std::uint64_t doubledDenominator
            = static_cast<std::uint64_t>(timecode.frameRate.denominator) * 2U;
        const std::uint64_t roundedFramesPerSecond
            = (doubledNumerator + timecode.frameRate.denominator)
                / doubledDenominator;
        if (roundedFramesPerSecond != timecode.nominalFramesPerSecond) {
            addIssue(result, TimelineValidationCode::InvalidTimecode,
                     path + ".nominalFramesPerSecond",
                     "timecode nominal frame count must match the rounded exact frame rate");
        }
    }
    if (timecode.counting == TimelineTimecodeCounting::DropFrame
        && !(timecode.frameRate.denominator == 1'001
             && timecode.nominalFramesPerSecond > 0
             && static_cast<std::uint64_t>(timecode.frameRate.numerator)
                    == static_cast<std::uint64_t>(
                           timecode.nominalFramesPerSecond) * 1'000U
             && timecode.nominalFramesPerSecond % 30U == 0U)) {
        addIssue(result, TimelineValidationCode::InvalidTimecode, path,
                 "drop-frame timecode requires an exact 1000/1001 rate with a 30-frame multiple nominal count");
    }
}

void validateColorDescription(const TimelineColorDescription &color,
                              const std::string &path,
                              TimelineValidationResult &result)
{
    validateProperties(color.metadata, path + ".metadata", result);
    if (color.masteringDisplay) {
        const TimelineMasteringDisplay &display = *color.masteringDisplay;
        const bool coordinatesValid = finite(display.red) && finite(display.green)
            && finite(display.blue) && finite(display.white)
            && display.red.x >= 0.0 && display.red.x <= 1.0
            && display.red.y >= 0.0 && display.red.y <= 1.0
            && display.green.x >= 0.0 && display.green.x <= 1.0
            && display.green.y >= 0.0 && display.green.y <= 1.0
            && display.blue.x >= 0.0 && display.blue.x <= 1.0
            && display.blue.y >= 0.0 && display.blue.y <= 1.0
            && display.white.x >= 0.0 && display.white.x <= 1.0
            && display.white.y >= 0.0 && display.white.y <= 1.0;
        if (!coordinatesValid
            || !finite(display.minimumLuminance)
            || !finite(display.maximumLuminance)
            || display.minimumLuminance < 0.0
            || display.maximumLuminance <= display.minimumLuminance) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     path + ".masteringDisplay",
                     "mastering-display chromaticities and luminance bounds must be finite and ordered");
        }
    }
    if (color.contentLight) {
        const TimelineContentLight &light = *color.contentLight;
        if (!finite(light.maximumContentLightLevel)
            || !finite(light.maximumFrameAverageLightLevel)
            || light.maximumContentLightLevel <= 0.0
            || light.maximumFrameAverageLightLevel <= 0.0
            || light.maximumFrameAverageLightLevel
                > light.maximumContentLightLevel) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     path + ".contentLight",
                     "content-light levels must be finite, positive, and ordered");
        }
    }
}

void validateContainer(const TimelineContainerDescriptor &container,
                       const std::string &path,
                       TimelineValidationResult &result,
                       TimelineValidationCode emptyCode)
{
    if (container.identifier.empty()) {
        addIssue(result, emptyCode, path + ".identifier",
                 "a container identifier is required");
    }
    validateProperties(container.options, path + ".options", result);
}

void validateCodec(const TimelineCodecDescriptor &codec,
                   const std::string &path,
                   TimelineValidationResult &result,
                   TimelineValidationCode code)
{
    if (codec.identifier.empty() && codec.name.empty() && codec.tag.empty()) {
        addIssue(result, code, path,
                 "a codec identifier, name, or tag is required");
    }
    if ((codec.bitRate && *codec.bitRate == 0)
        || (codec.bitsPerCodedSample && *codec.bitsPerCodedSample == 0)
        || (codec.bitsPerRawSample && *codec.bitsPerRawSample == 0)) {
        addIssue(result, code, path,
                 "present codec rates and sample depths must be positive");
    }
    constexpr std::size_t MaxInitializationDataBytes = 16U * 1024U * 1024U;
    if (codec.initializationData.size() > MaxInitializationDataBytes) {
        addIssue(result, code, path + ".initializationData",
                 "codec initialization data exceeds the structural safety limit");
    }
    validateProperties(codec.options, path + ".options", result);
}

std::string_view streamId(const TimelineMediaStream &stream) noexcept
{
    return std::visit([](const auto &value) -> std::string_view {
        return value.id;
    }, stream);
}

std::uint32_t streamIndex(const TimelineMediaStream &stream) noexcept
{
    return std::visit([](const auto &value) { return value.streamIndex; }, stream);
}

void validateVideoTiming(const TimelineVideoTiming &timing,
                         const TimelineTimeBase &timeBase,
                         std::int64_t streamStart,
                         std::int64_t streamDuration,
                         const std::string &path,
                         TimelineValidationResult &result)
{
    const auto validateOptionalRate = [&](const std::optional<TimelineFrameRate> &rate,
                                          const char *name) {
        if (rate) {
            validateFrameRate(*rate, path + "." + name, result);
        }
    };
    validateOptionalRate(timing.nominal, "nominal");
    validateOptionalRate(timing.average, "average");
    validateOptionalRate(timing.minimum, "minimum");
    validateOptionalRate(timing.maximum, "maximum");

    if (timing.mode == TimelineFrameRateMode::Constant && !timing.nominal) {
        addIssue(result, TimelineValidationCode::InvalidFrameRate, path,
                 "constant-rate video requires a nominal frame rate");
    }
    if (timing.mode == TimelineFrameRateMode::Variable
        && !timing.nominal && !timing.average && timing.samples.empty()) {
        addIssue(result, TimelineValidationCode::InvalidFrameRate, path,
                 "variable-rate video requires a nominal/average rate or sample timing index");
    }
    if (timing.minimum && timing.maximum
        && !frameRateLessOrEqual(*timing.minimum, *timing.maximum)) {
        addIssue(result, TimelineValidationCode::InvalidFrameRate, path,
                 "minimum frame rate must not exceed maximum frame rate");
    }
    if (timing.minimum && timing.average
        && !frameRateLessOrEqual(*timing.minimum, *timing.average)) {
        addIssue(result, TimelineValidationCode::InvalidFrameRate, path,
                 "average frame rate must not be below the minimum");
    }
    if (timing.average && timing.maximum
        && !frameRateLessOrEqual(*timing.average, *timing.maximum)) {
        addIssue(result, TimelineValidationCode::InvalidFrameRate, path,
                 "average frame rate must not exceed the maximum");
    }

    std::optional<std::int64_t> previousPresentation;
    std::optional<std::int64_t> previousDecoding;
    for (std::size_t index = 0; index < timing.samples.size(); ++index) {
        const TimelineVideoSampleTiming &sample = timing.samples[index];
        const std::string samplePath = path + ".samples[" + std::to_string(index) + "]";
        if (sample.durationTicks <= 0
            || (previousPresentation
                && sample.presentationTicks <= *previousPresentation)) {
            addIssue(result, TimelineValidationCode::InvalidStream, samplePath,
                     "sample presentation times must be strictly increasing and durations positive");
        }
        if (sample.decodingTicks && previousDecoding
            && *sample.decodingTicks <= *previousDecoding) {
            addIssue(result, TimelineValidationCode::InvalidStream,
                     samplePath + ".decodingTicks",
                     "present decoding timestamps must be strictly increasing");
        }
        const TimelineTickRange sampleRange{sample.presentationTicks,
                                            sample.durationTicks};
        if (!rangeEnd(sampleRange)) {
            addIssue(result, TimelineValidationCode::InvalidStream, samplePath,
                     "sample presentation ranges must not overflow signed ticks");
        }
        if (streamDuration > 0
            && !rangeInside(sampleRange, streamStart, streamDuration, false)) {
            addIssue(result, TimelineValidationCode::InvalidStream, samplePath,
                     "sample presentation range must fit its stream duration");
        }
        previousPresentation = sample.presentationTicks;
        if (sample.decodingTicks) {
            previousDecoding = sample.decodingTicks;
        }
    }
    (void) timeBase;
}

void validateStream(const TimelineMediaStream &stream,
                    const std::string &path,
                    TimelineValidationResult &result)
{
    std::visit([&](const auto &value) {
        using Stream = std::decay_t<decltype(value)>;
        if (value.id.empty()) {
            addIssue(result, TimelineValidationCode::InvalidId,
                     path + ".id", "stream ids must not be empty");
        }
        validateTimeBase(value.timeBase, path + ".timeBase", result);
        if (value.durationTicks < 0
            || (value.durationTicks > 0
                && value.startTicks
                    > std::numeric_limits<std::int64_t>::max()
                        - value.durationTicks)) {
            addIssue(result, TimelineValidationCode::InvalidStream,
                     path + ".durationTicks",
                     "stream timestamp ranges must not be negative or overflow");
        }
        validateCodec(value.codec, path + ".codec", result,
                      TimelineValidationCode::InvalidStream);
        validateProperties(value.metadata, path + ".metadata", result);

        if constexpr (std::is_same_v<Stream, TimelineVideoStream>) {
            if (value.codedExtent.width <= 0 || value.codedExtent.height <= 0
                || value.displayExtent.width <= 0 || value.displayExtent.height <= 0
                || !validRational(value.pixelAspectRatio)
                || value.pixelAspectRatio.numerator <= 0
                || value.bitDepth == 0 || value.pixelFormat.empty()) {
                addIssue(result, TimelineValidationCode::InvalidStream, path,
                         "video dimensions, pixel aspect, bit depth, and pixel format must be explicit and positive");
            }
            if (!finite(value.rotationDegrees)) {
                addIssue(result, TimelineValidationCode::InvalidNumericValue,
                         path + ".rotationDegrees", "video rotation must be finite");
            }
            if (value.scanMode == TimelineScanMode::Progressive
                && value.fieldOrder != TimelineFieldOrder::None) {
                addIssue(result, TimelineValidationCode::InvalidStream, path,
                         "progressive video must not declare an interlaced field order");
            }
            validateVideoTiming(value.timing, value.timeBase, value.startTicks,
                                value.durationTicks, path + ".timing", result);
            validateColorDescription(value.color, path + ".color", result);
        } else if constexpr (std::is_same_v<Stream, TimelineAudioStream>) {
            if (value.sampleRate == 0 || value.channelCount == 0
                || value.channelLayout.empty() || value.sampleFormat.empty()
                || value.bitDepth == 0) {
                addIssue(result, TimelineValidationCode::InvalidStream, path,
                         "audio sample rate, channels, layout, format, and bit depth must be explicit and positive");
            }
            if (!value.speakerLabels.empty()
                && value.speakerLabels.size() != value.channelCount) {
                addIssue(result, TimelineValidationCode::InvalidStream,
                         path + ".speakerLabels",
                         "speaker labels must match the declared channel count");
            }
            const TimelineAudioLoudness &loudness = value.loudness;
            for (const std::optional<double> *measurement : {
                     &loudness.integratedLufs,
                     &loudness.truePeakDbtp,
                     &loudness.loudnessRangeLu,
                     &loudness.maximumMomentaryLufs,
                     &loudness.maximumShortTermLufs}) {
                if (*measurement && !finite(**measurement)) {
                    addIssue(result, TimelineValidationCode::InvalidNumericValue,
                             path + ".loudness", "audio loudness measurements must be finite");
                    break;
                }
            }
        } else if constexpr (std::is_same_v<Stream, TimelineSubtitleStream>) {
            std::unordered_set<std::string> cueIds;
            std::optional<std::int64_t> previousStart;
            std::optional<TimelineTickRange> previousChapter;
            for (std::size_t index = 0; index < value.cues.size(); ++index) {
                const TimelineSubtitleCue &cue = value.cues[index];
                const std::string cuePath = path + ".cues[" + std::to_string(index) + "]";
                if (cue.id.empty() || !cueIds.insert(cue.id).second) {
                    addIssue(result, TimelineValidationCode::DuplicateId,
                             cuePath + ".id", "subtitle cue ids must be non-empty and unique");
                }
                if (cue.range.duration <= 0 || !rangeEnd(cue.range)
                    || (previousStart && cue.range.start < *previousStart)
                    || (value.durationTicks > 0
                        && !rangeInside(cue.range, value.startTicks,
                                        value.durationTicks, false))) {
                    addIssue(result, TimelineValidationCode::InvalidRange,
                             cuePath + ".range",
                             "subtitle cues must be ordered and have a positive in-stream range");
                }
                if (cue.text.empty() && cue.markup.empty()
                    && cue.imageResourceId.empty()) {
                    addIssue(result, TimelineValidationCode::InvalidStream,
                             cuePath, "subtitle cues require text, markup, or an image payload");
                }
                if ((cue.line && !finite(*cue.line))
                    || (cue.position && !finite(*cue.position))
                    || (cue.size && !finite(*cue.size))) {
                    addIssue(result, TimelineValidationCode::InvalidNumericValue,
                             cuePath,
                             "subtitle cue positioning values must be finite when present");
                }
                if (value.kind == TimelineSubtitleKind::Chapter
                    && previousChapter && rangesIntersect(*previousChapter, cue.range)) {
                    addIssue(result, TimelineValidationCode::InvalidRange,
                             cuePath + ".range", "chapter cues must not overlap");
                }
                validateProperties(cue.settings, cuePath + ".settings", result);
                previousStart = cue.range.start;
                if (value.kind == TimelineSubtitleKind::Chapter) {
                    previousChapter = cue.range;
                }
            }
        } else if constexpr (std::is_same_v<Stream, TimelineDataStream>) {
            if (value.kind.empty()) {
                addIssue(result, TimelineValidationCode::InvalidStream,
                         path + ".kind", "data streams require a kind identifier");
            }
        }
    }, stream);
}

void validateMarkerCollection(const std::vector<TimelineMarker> &markers,
                              std::int64_t ownerDuration,
                              const std::string &path,
                              TimelineValidationResult &result)
{
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < markers.size(); ++index) {
        const TimelineMarker &marker = markers[index];
        const std::string markerPath = path + "[" + std::to_string(index) + "]";
        if (marker.id.empty() || !ids.insert(marker.id).second) {
            addIssue(result, TimelineValidationCode::DuplicateId,
                     markerPath + ".id", "marker ids must be non-empty and unique");
        }
        if (marker.range.start < 0 || marker.range.duration < 0
            || (ownerDuration >= 0
                && !rangeInside(marker.range, 0, ownerDuration, true))) {
            addIssue(result, TimelineValidationCode::InvalidMarker,
                     markerPath + ".range", "marker ranges must fit their owner");
        }
        if (!normalized(marker.color)) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     markerPath + ".color", "marker colors must be finite normalized RGBA values");
        }
        validateProperties(marker.metadata, markerPath + ".metadata", result);
    }
}

void validateAutomation(const std::vector<TimelineAutomationCurve> &automation,
                        const std::vector<TimelineProperty> &parameters,
                        std::int64_t ownerDuration,
                        const std::string &path,
                        TimelineValidationResult &result)
{
    std::unordered_map<std::string, const TimelinePropertyValue *> parameterValues;
    for (const TimelineProperty &parameter : parameters) {
        parameterValues.emplace(parameter.key, &parameter.value);
    }
    std::unordered_set<std::string> automatedParameters;
    for (std::size_t curveIndex = 0; curveIndex < automation.size(); ++curveIndex) {
        const TimelineAutomationCurve &curve = automation[curveIndex];
        const std::string curvePath = path + "[" + std::to_string(curveIndex) + "]";
        const auto parameter = parameterValues.find(curve.parameterId);
        if (curve.parameterId.empty() || parameter == parameterValues.end()) {
            addIssue(result, TimelineValidationCode::InvalidAutomation,
                     curvePath + ".parameterId",
                     "automation must reference an existing effect parameter");
        } else if (!automatedParameters.insert(curve.parameterId).second) {
            addIssue(result, TimelineValidationCode::InvalidAutomation,
                     curvePath + ".parameterId",
                     "one parameter may have only one automation curve");
        } else if (curve.defaultValue.index() != parameter->second->index()) {
            addIssue(result, TimelineValidationCode::InvalidAutomation,
                     curvePath + ".defaultValue",
                     "automation and parameter values must have the same value kind");
        }
        if (!finite(curve.defaultValue)) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     curvePath + ".defaultValue", "automation values must be finite");
        }

        std::optional<std::int64_t> previousTime;
        for (std::size_t keyIndex = 0; keyIndex < curve.keyframes.size(); ++keyIndex) {
            const TimelineAutomationKeyframe &keyframe = curve.keyframes[keyIndex];
            const std::string keyPath = curvePath + ".keyframes["
                + std::to_string(keyIndex) + "]";
            if (keyframe.timeTicks < 0
                || keyframe.timeTicks > ownerDuration
                || (previousTime && keyframe.timeTicks <= *previousTime)) {
                addIssue(result, TimelineValidationCode::InvalidAutomation,
                         keyPath + ".timeTicks",
                         "automation times must be non-negative, strictly ordered, and inside the owner");
            }
            if (keyframe.value.index() != curve.defaultValue.index()) {
                addIssue(result, TimelineValidationCode::InvalidAutomation,
                         keyPath + ".value",
                         "all automation values must keep one value kind");
            }
            if (keyframe.interpolation != TimelineKeyframeInterpolation::Hold
                && !supportsContinuousInterpolation(curve.defaultValue)) {
                addIssue(result, TimelineValidationCode::InvalidAutomation,
                         keyPath + ".interpolation",
                         "non-numeric automation values support hold interpolation only");
            }
            if (!finite(keyframe.value) || !finite(keyframe.incoming)
                || !finite(keyframe.outgoing)) {
                addIssue(result, TimelineValidationCode::InvalidNumericValue,
                         keyPath, "automation values and tangents must be finite");
            }
            previousTime = keyframe.timeTicks;
        }
    }
}

void validateEffects(const std::vector<TimelineEffect> &effects,
                     std::int64_t ownerDuration,
                     const std::string &path,
                     TimelineValidationResult &result)
{
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < effects.size(); ++index) {
        const TimelineEffect &effect = effects[index];
        const std::string effectPath = path + "[" + std::to_string(index) + "]";
        if (effect.id.empty() || !ids.insert(effect.id).second) {
            addIssue(result, TimelineValidationCode::DuplicateId,
                     effectPath + ".id", "effect ids must be non-empty and unique");
        }
        if (effect.pluginId.empty()) {
            addIssue(result, TimelineValidationCode::InvalidEffect,
                     effectPath + ".pluginId", "effects require a plug-in/type identifier");
        }
        if (effect.activeRange
            && !rangeInside(*effect.activeRange, 0, ownerDuration, false)) {
            addIssue(result, TimelineValidationCode::InvalidEffect,
                     effectPath + ".activeRange", "effect ranges must fit their owner");
        }
        validateProperties(effect.parameters, effectPath + ".parameters", result);
        validateAutomation(effect.automation, effect.parameters, ownerDuration,
                           effectPath + ".automation", result);
        validateProperties(effect.metadata, effectPath + ".metadata", result);
    }
}

const TimelineMediaStream *resolveReferencedStream(
    const TimelineProject &project,
    const TimelineMediaReference &reference) noexcept
{
    const TimelineMediaSource *source = findTimelineMediaSource(
        project, reference.mediaSourceId);
    return source ? findTimelineMediaStream(*source, reference.streamId) : nullptr;
}

void validateTimeMap(const TimelineClipProperties &properties,
                     const TimelineMediaStream *sourceStream,
                     const std::string &path,
                     TimelineValidationResult &result)
{
    if (properties.timeMap.empty()) {
        return;
    }
    if (properties.timeMap.front().timelineTicks != 0
        || properties.timeMap.back().timelineTicks
            != properties.timelineRange.duration) {
        addIssue(result, TimelineValidationCode::InvalidClip,
                 path + ".timeMap",
                 "time maps must span from zero through the complete clip duration");
    }
    std::optional<std::int64_t> previousOffset;
    const auto sourceEnd = rangeEnd(properties.sourceRange);
    for (std::size_t index = 0; index < properties.timeMap.size(); ++index) {
        const TimelineTimeMapPoint &point = properties.timeMap[index];
        const std::string pointPath = path + ".timeMap[" + std::to_string(index) + "]";
        if (point.timelineTicks < 0
            || point.timelineTicks > properties.timelineRange.duration
            || (previousOffset && point.timelineTicks <= *previousOffset)) {
            addIssue(result, TimelineValidationCode::InvalidClip,
                     pointPath + ".timelineTicks",
                     "time-map timeline offsets must be non-negative and strictly ordered");
        }
        if (properties.loopMode == TimelineLoopMode::None
            && (!sourceEnd || point.sourceTicks < properties.sourceRange.start
                || point.sourceTicks > *sourceEnd)) {
            addIssue(result, TimelineValidationCode::InvalidClip,
                     pointPath + ".sourceTicks",
                     "non-looping time-map samples must stay inside the source trim");
        }
        if (!finite(point.incoming) || !finite(point.outgoing)) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     pointPath, "time-map Bezier tangents must be finite");
        }
        previousOffset = point.timelineTicks;
    }
    (void) sourceStream;
}

void validateClipProperties(const TimelineClipProperties &properties,
                            TimelineStreamKind expectedKind,
                            const TimelineProject &project,
                            const TimelineSequence &sequence,
                            const std::string &path,
                            TimelineValidationResult &result)
{
    if (properties.id.empty()) {
        addIssue(result, TimelineValidationCode::InvalidId,
                 path + ".id", "clip ids must not be empty");
    }
    if (!rangeInside(properties.timelineRange, sequence.startTicks,
                     sequence.durationTicks, false)) {
        addIssue(result, TimelineValidationCode::InvalidClip,
                 path + ".timelineRange",
                 "clip timeline ranges must be positive and fit the sequence");
    }
    if (properties.sourceRange.duration <= 0 || !rangeEnd(properties.sourceRange)) {
        addIssue(result, TimelineValidationCode::InvalidClip,
                 path + ".sourceRange", "clip source trims must have a positive safe range");
    }
    if (!validRational(properties.playbackRate)) {
        addIssue(result, TimelineValidationCode::InvalidClip,
                 path + ".playbackRate",
                 "playback-rate numerator must be non-zero and denominator positive");
    }
    if (!normalized(properties.colorLabel)) {
        addIssue(result, TimelineValidationCode::InvalidNumericValue,
                 path + ".colorLabel", "clip label colors must be finite normalized RGBA values");
    }

    const TimelineMediaStream *sourceStream = nullptr;
    std::optional<TimelineTimeBase> sourceTimeBase;
    std::visit([&](const auto &reference) {
        using Reference = std::decay_t<decltype(reference)>;
        if constexpr (std::is_same_v<Reference, TimelineMediaReference>) {
            const TimelineMediaSource *source = findTimelineMediaSource(
                project, reference.mediaSourceId);
            if (!source) {
                addIssue(result, TimelineValidationCode::MissingReference,
                         path + ".source.mediaSourceId",
                         "clip media source does not exist");
                return;
            }
            sourceStream = findTimelineMediaStream(*source, reference.streamId);
            if (!sourceStream) {
                addIssue(result, TimelineValidationCode::MissingReference,
                         path + ".source.streamId",
                         "clip media stream does not exist");
                return;
            }
            if (timelineStreamKind(*sourceStream) != expectedKind) {
                addIssue(result, TimelineValidationCode::StreamKindMismatch,
                         path + ".source.streamId",
                         "clip source kind must match its typed track");
            }
            std::visit([&](const auto &stream) {
                sourceTimeBase = stream.timeBase;
                if (stream.durationTicks > 0
                    && !rangeInside(properties.sourceRange, stream.startTicks,
                                    stream.durationTicks, false)) {
                    addIssue(result, TimelineValidationCode::InvalidClip,
                             path + ".sourceRange",
                             "clip source trim must fit the referenced stream");
                }
            }, *sourceStream);
        } else if constexpr (std::is_same_v<Reference, TimelineSequenceReference>) {
            const TimelineSequence *referencedSequence = findTimelineSequence(
                project, reference.sequenceId);
            if (!referencedSequence) {
                addIssue(result, TimelineValidationCode::MissingReference,
                         path + ".source.sequenceId",
                         "nested sequence source does not exist");
            } else {
                sourceTimeBase = referencedSequence->timeBase;
                if (!rangeInside(properties.sourceRange,
                                 referencedSequence->startTicks,
                                 referencedSequence->durationTicks,
                                 false)) {
                    addIssue(result, TimelineValidationCode::InvalidClip,
                             path + ".sourceRange",
                             "nested sequence trims must fit the referenced sequence");
                }
            }
            if (reference.kind != expectedKind) {
                addIssue(result, TimelineValidationCode::StreamKindMismatch,
                         path + ".source.kind",
                         "nested sequence source kind must match its typed track");
            }
        } else {
            if (reference.generatorId.empty()) {
                addIssue(result, TimelineValidationCode::MissingReference,
                         path + ".source.generatorId",
                         "generated sources require a generator identifier");
            }
            if (reference.kind != expectedKind) {
                addIssue(result, TimelineValidationCode::StreamKindMismatch,
                         path + ".source.kind",
                         "generated source kind must match its typed track");
            }
            validateTimeBase(reference.timeBase,
                             path + ".source.timeBase", result);
            sourceTimeBase = reference.timeBase;
            validateProperties(reference.parameters,
                               path + ".source.parameters", result);
        }
    }, properties.source);

    if (!properties.timeMap.empty()) {
        if (properties.playbackRate != TimelineRational{1, 1}) {
            addIssue(result, TimelineValidationCode::InvalidClip,
                     path + ".playbackRate",
                     "an explicit time map is authoritative and requires a unit playback rate");
        }
    } else if (properties.loopMode == TimelineLoopMode::None
               && sourceTimeBase
               && !clipDurationsMatch(properties.timelineRange.duration,
                                      sequence.timeBase,
                                      properties.sourceRange.duration,
                                      *sourceTimeBase,
                                      properties.playbackRate)) {
        addIssue(result, TimelineValidationCode::InvalidClip,
                 path + ".playbackRate",
                 "constant playback rate must exactly relate source and sequence durations");
    }

    validateTimeMap(properties, sourceStream, path, result);
    validateEffects(properties.effects, properties.timelineRange.duration,
                    path + ".effects", result);
    validateMarkerCollection(properties.markers,
                             properties.timelineRange.duration,
                             path + ".markers", result);
    validateProperties(properties.metadata, path + ".metadata", result);
}

template<typename Clip>
void validateTypedClip(const Clip &clip,
                       TimelineStreamKind expectedKind,
                       const TimelineProject &project,
                       const TimelineSequence &sequence,
                       const std::string &path,
                       TimelineValidationResult &result)
{
    validateClipProperties(clip.properties, expectedKind, project, sequence,
                           path + ".properties", result);
    if constexpr (std::is_same_v<Clip, TimelineVideoClip>) {
        const TimelineTransform &transform = clip.transform;
        if (!finite(transform.position) || !finite(transform.anchor)
            || !finite(transform.scale) || !finite(transform.skew)
            || !finite(transform.rotationDegrees) || !finite(transform.opacity)
            || transform.opacity < 0.0 || transform.opacity > 1.0) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     path + ".transform",
                     "video transforms must be finite and opacity normalized");
        }
        if (!finite(clip.crop.left) || !finite(clip.crop.top)
            || !finite(clip.crop.right) || !finite(clip.crop.bottom)
            || !finite(clip.crop.feather) || clip.crop.left < 0.0
            || clip.crop.top < 0.0 || clip.crop.right < 0.0
            || clip.crop.bottom < 0.0 || clip.crop.feather < 0.0) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     path + ".crop", "video crop values must be finite and non-negative");
        }
    } else if constexpr (std::is_same_v<Clip, TimelineAudioClip>) {
        const TimelineAudioMix &mix = clip.mix;
        if (!finite(mix.gainDb) || !finite(mix.pan)
            || mix.pan < -1.0 || mix.pan > 1.0
            || mix.fadeInTicks < 0 || mix.fadeOutTicks < 0
            || mix.fadeInTicks > clip.properties.timelineRange.duration
            || mix.fadeOutTicks > clip.properties.timelineRange.duration
            || !std::all_of(mix.channelMatrix.begin(), mix.channelMatrix.end(),
                            [](double value) { return finite(value); })) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     path + ".mix", "audio mix values and fades are invalid");
        }
    } else if constexpr (std::is_same_v<Clip, TimelineSubtitleClip>) {
        if (clip.text.empty() && clip.markup.empty()) {
            addIssue(result, TimelineValidationCode::InvalidClip, path,
                     "subtitle clips require text or markup");
        }
        const TimelineSubtitleStyle &style = clip.style;
        if (!finite(style.fontSize) || style.fontSize < 0.0
            || !finite(style.outlineWidth) || style.outlineWidth < 0.0
            || !normalized(style.fill) || !normalized(style.outline)
            || !normalized(style.background)) {
            addIssue(result, TimelineValidationCode::InvalidNumericValue,
                     path + ".style", "subtitle style values must be finite and valid");
        }
        validateProperties(style.properties, path + ".style.properties", result);
    } else if constexpr (std::is_same_v<Clip, TimelineDataClip>) {
        if (clip.format.empty() && clip.payload.empty()) {
            addIssue(result, TimelineValidationCode::InvalidClip, path,
                     "data clips require a format identifier or payload");
        }
    }
}

template<typename Clip>
const Clip *findTypedClip(const std::vector<Clip> &clips,
                          const std::string &id) noexcept
{
    const auto found = std::find_if(clips.begin(), clips.end(), [&](const Clip &clip) {
        return clip.properties.id == id;
    });
    return found == clips.end() ? nullptr : &*found;
}

template<typename Clip>
Clip *findTypedClip(std::vector<Clip> &clips, const std::string &id) noexcept
{
    return const_cast<Clip *>(findTypedClip(
        static_cast<const std::vector<Clip> &>(clips), id));
}

template<typename Track>
void validateTrack(const Track &track,
                   TimelineStreamKind kind,
                   const TimelineProject &project,
                   const TimelineSequence &sequence,
                   const std::string &path,
                   TimelineValidationResult &result)
{
    if (track.properties.id.empty()) {
        addIssue(result, TimelineValidationCode::InvalidId,
                 path + ".properties.id", "track ids must not be empty");
    }
    if (!finite(track.properties.opacity)
        || track.properties.opacity < 0.0 || track.properties.opacity > 1.0
        || !normalized(track.properties.colorLabel)) {
        addIssue(result, TimelineValidationCode::InvalidNumericValue,
                 path + ".properties",
                 "track opacity and color label must be finite normalized values");
    }
    validateEffects(track.properties.effects, sequence.durationTicks,
                    path + ".properties.effects", result);
    validateMarkerCollection(track.properties.markers, sequence.durationTicks,
                             path + ".properties.markers", result);
    validateProperties(track.properties.metadata,
                       path + ".properties.metadata", result);

    std::unordered_set<std::string> clipIds;
    for (std::size_t index = 0; index < track.clips.size(); ++index) {
        const auto &clip = track.clips[index];
        const std::string clipPath = path + ".clips[" + std::to_string(index) + "]";
        if (!clip.properties.id.empty()
            && !clipIds.insert(clip.properties.id).second) {
            addIssue(result, TimelineValidationCode::DuplicateId,
                     clipPath + ".properties.id",
                     "clip ids must be unique in their owning track");
        }
        validateTypedClip(clip, kind, project, sequence, clipPath, result);
    }

    std::unordered_set<std::string> transitionIds;
    for (std::size_t index = 0; index < track.transitions.size(); ++index) {
        const TimelineTransition &transition = track.transitions[index];
        const std::string transitionPath = path + ".transitions["
            + std::to_string(index) + "]";
        if (transition.id.empty() || !transitionIds.insert(transition.id).second) {
            addIssue(result, TimelineValidationCode::DuplicateId,
                     transitionPath + ".id",
                     "transition ids must be non-empty and unique");
        }
        if (transition.typeId.empty()
            || (transition.fromClipId.empty() && transition.toClipId.empty())
            || transition.fromClipId == transition.toClipId) {
            addIssue(result, TimelineValidationCode::InvalidTransition,
                     transitionPath,
                     "transitions require a type and at least one distinct clip endpoint");
        }
        const auto *from = transition.fromClipId.empty()
            ? nullptr : findTypedClip(track.clips, transition.fromClipId);
        const auto *to = transition.toClipId.empty()
            ? nullptr : findTypedClip(track.clips, transition.toClipId);
        if ((!transition.fromClipId.empty() && !from)
            || (!transition.toClipId.empty() && !to)) {
            addIssue(result, TimelineValidationCode::MissingReference,
                     transitionPath, "transition endpoints must exist in their owning track");
        }
        if (!rangeInside(transition.timelineRange, sequence.startTicks,
                         sequence.durationTicks, false)) {
            addIssue(result, TimelineValidationCode::InvalidTransition,
                     transitionPath + ".timelineRange",
                     "transition range must be positive and fit the sequence");
        }
        const std::optional<std::int64_t> transitionEnd
            = rangeEnd(transition.timelineRange);
        if (transitionEnd && from && to) {
            const std::optional<std::int64_t> fromEnd
                = rangeEnd(from->properties.timelineRange);
            const std::optional<std::int64_t> toEnd
                = rangeEnd(to->properties.timelineRange);
            bool aligned = fromEnd && toEnd
                && *fromEnd == to->properties.timelineRange.start;
            if (aligned) {
                const std::int64_t cut = *fromEnd;
                aligned = transition.timelineRange.start
                        >= from->properties.timelineRange.start
                    && transition.timelineRange.start <= cut
                    && *transitionEnd >= cut
                    && *transitionEnd <= *toEnd;
                if (aligned) {
                    const std::int64_t beforeCut
                        = cut - transition.timelineRange.start;
                    const std::int64_t afterCut = *transitionEnd - cut;
                    switch (transition.alignment) {
                    case TimelineTransitionAlignment::StartAtCut:
                        aligned = beforeCut == 0;
                        break;
                    case TimelineTransitionAlignment::CenteredOnCut:
                        aligned = beforeCut >= 0 && afterCut >= 0
                            && (beforeCut > afterCut
                                    ? beforeCut - afterCut
                                    : afterCut - beforeCut) <= 1;
                        break;
                    case TimelineTransitionAlignment::EndAtCut:
                        aligned = afterCut == 0;
                        break;
                    case TimelineTransitionAlignment::Custom:
                        aligned = beforeCut >= 0 && afterCut >= 0;
                        break;
                    }
                }
            }
            if (!aligned) {
                addIssue(result, TimelineValidationCode::InvalidTransition,
                         transitionPath + ".timelineRange",
                         "two-sided transition ranges must cover one adjacent cut according to their alignment");
            }
        } else if (transitionEnd && from && !to) {
            const std::optional<std::int64_t> fromEnd
                = rangeEnd(from->properties.timelineRange);
            if (!fromEnd || *transitionEnd != *fromEnd
                || transition.timelineRange.start
                    < from->properties.timelineRange.start
                || (transition.alignment != TimelineTransitionAlignment::EndAtCut
                    && transition.alignment
                        != TimelineTransitionAlignment::Custom)) {
                addIssue(result, TimelineValidationCode::InvalidTransition,
                         transitionPath + ".timelineRange",
                         "an outgoing one-sided transition must end at its clip cut");
            }
        } else if (transitionEnd && to && !from) {
            const std::optional<std::int64_t> toEnd
                = rangeEnd(to->properties.timelineRange);
            if (!toEnd
                || transition.timelineRange.start
                    != to->properties.timelineRange.start
                || *transitionEnd > *toEnd
                || (transition.alignment
                        != TimelineTransitionAlignment::StartAtCut
                    && transition.alignment
                        != TimelineTransitionAlignment::Custom)) {
                addIssue(result, TimelineValidationCode::InvalidTransition,
                         transitionPath + ".timelineRange",
                         "an incoming one-sided transition must start at its clip cut");
            }
        }
        validateProperties(transition.parameters,
                           transitionPath + ".parameters", result);
        validateAutomation(transition.automation, transition.parameters,
                           transition.timelineRange.duration,
                           transitionPath + ".automation", result);
        validateProperties(transition.metadata,
                           transitionPath + ".metadata", result);
    }
}

void validateSequence(const TimelineSequence &sequence,
                      const TimelineProject &project,
                      const std::string &path,
                      TimelineValidationResult &result)
{
    if (sequence.id.empty()) {
        addIssue(result, TimelineValidationCode::InvalidId,
                 path + ".id", "sequence ids must not be empty");
    }
    validateTimeBase(sequence.timeBase, path + ".timeBase", result);
    validateFrameRate(sequence.editingFrameRate,
                      path + ".editingFrameRate", result);
    if (sequence.durationTicks < 0
        || sequence.startTicks > std::numeric_limits<std::int64_t>::max()
            - sequence.durationTicks) {
        addIssue(result, TimelineValidationCode::InvalidSequence,
                 path + ".durationTicks", "sequence ranges must not be negative or overflow");
    }
    if (sequence.workArea
        && !rangeInside(*sequence.workArea, sequence.startTicks,
                        sequence.durationTicks, false)) {
        addIssue(result, TimelineValidationCode::InvalidRange,
                 path + ".workArea", "work area must fit the sequence");
    }
    if ((sequence.canvasExtent.width == 0) != (sequence.canvasExtent.height == 0)
        || sequence.canvasExtent.width < 0 || sequence.canvasExtent.height < 0
        || !validRational(sequence.pixelAspectRatio)
        || sequence.pixelAspectRatio.numerator <= 0) {
        addIssue(result, TimelineValidationCode::InvalidSequence, path,
                 "sequence canvas extent and pixel aspect ratio are invalid");
    }
    if (sequence.timecode) {
        validateTimecode(*sequence.timecode, path + ".timecode", result);
    }
    validateColorDescription(sequence.color, path + ".color", result);

    std::unordered_set<std::string> trackIds;
    std::unordered_set<std::string> sequenceClipIds;
    std::unordered_map<std::string, std::string> clipLinkGroups;
    bool hasVisualClips = false;
    bool hasAudioClips = false;
    for (std::size_t index = 0; index < sequence.tracks.size(); ++index) {
        const TimelineTrack &track = sequence.tracks[index];
        const TimelineTrackProperties &properties = timelineTrackProperties(track);
        const std::string trackPath = path + ".tracks[" + std::to_string(index) + "]";
        if (!properties.id.empty() && !trackIds.insert(properties.id).second) {
            addIssue(result, TimelineValidationCode::DuplicateId,
                     trackPath + ".properties.id",
                     "track ids must be unique in their sequence");
        }
        std::visit([&](const auto &typedTrack) {
            using Track = std::decay_t<decltype(typedTrack)>;
            if constexpr (std::is_same_v<Track, TimelineVideoTrack>
                          || std::is_same_v<Track, TimelineSubtitleTrack>) {
                hasVisualClips = hasVisualClips || !typedTrack.clips.empty();
            } else if constexpr (std::is_same_v<Track, TimelineAudioTrack>) {
                hasAudioClips = hasAudioClips || !typedTrack.clips.empty();
            }
            for (const auto &clip : typedTrack.clips) {
                if (!clip.properties.id.empty()
                    && !sequenceClipIds.insert(clip.properties.id).second) {
                    addIssue(result, TimelineValidationCode::DuplicateId,
                             trackPath + ".clips",
                             "clip ids must be unique across their sequence");
                }
                if (!clip.properties.id.empty()) {
                    clipLinkGroups.emplace(clip.properties.id,
                                           clip.properties.linkGroupId);
                }
            }
            validateTrack(typedTrack, timelineTrackKind(track), project,
                          sequence, trackPath, result);
        }, track);
    }

    if (hasVisualClips
        && (sequence.canvasExtent.width <= 0
            || sequence.canvasExtent.height <= 0)) {
        addIssue(result, TimelineValidationCode::InvalidSequence,
                 path + ".canvasExtent",
                 "sequences with visual clips require a positive canvas extent");
    }
    if (hasAudioClips
        && (sequence.audioSampleRate == 0
            || sequence.audioChannelLayout.empty())) {
        addIssue(result, TimelineValidationCode::InvalidSequence,
                 path + ".audioSampleRate",
                 "sequences with audio clips require an explicit mix sample rate and channel layout");
    }

    validateMarkerCollection(sequence.markers, sequence.durationTicks,
                             path + ".markers", result);
    std::unordered_set<std::string> linkIds;
    std::unordered_map<std::string, std::string> declaredGroupByClip;
    for (std::size_t index = 0; index < sequence.linkGroups.size(); ++index) {
        const TimelineLinkGroup &group = sequence.linkGroups[index];
        const std::string groupPath = path + ".linkGroups["
            + std::to_string(index) + "]";
        if (group.id.empty() || !linkIds.insert(group.id).second) {
            addIssue(result, TimelineValidationCode::DuplicateId,
                     groupPath + ".id", "link-group ids must be non-empty and unique");
        }
        if (group.clipIds.size() < 2) {
            addIssue(result, TimelineValidationCode::InvalidLinkGroup,
                     groupPath + ".clipIds", "link groups require at least two clips");
        }
        std::unordered_set<std::string> members;
        for (const std::string &clipId : group.clipIds) {
            const bool uniqueMember = members.insert(clipId).second;
            const auto clip = clipLinkGroups.find(clipId);
            if (!uniqueMember || clip == clipLinkGroups.end()) {
                addIssue(result, TimelineValidationCode::MissingReference,
                         groupPath + ".clipIds",
                         "link-group clip ids must be unique existing sequence clips");
                continue;
            }
            const auto [declared, inserted]
                = declaredGroupByClip.emplace(clipId, group.id);
            if (!inserted && declared->second != group.id) {
                addIssue(result, TimelineValidationCode::InvalidLinkGroup,
                         groupPath + ".clipIds",
                         "one clip cannot belong to multiple link groups");
            }
            if (clip->second != group.id) {
                addIssue(result, TimelineValidationCode::InvalidLinkGroup,
                         groupPath + ".clipIds",
                         "link-group members must declare the same group id on the clip");
            }
        }
    }
    for (const auto &[clipId, groupId] : clipLinkGroups) {
        if (groupId.empty()) {
            continue;
        }
        if (linkIds.find(groupId) == linkIds.end()) {
            addIssue(result, TimelineValidationCode::MissingReference,
                     path + ".tracks.clips.linkGroupId",
                     "clip link-group ids must reference an existing group");
            continue;
        }
        const auto declared = declaredGroupByClip.find(clipId);
        if (declared == declaredGroupByClip.end()
            || declared->second != groupId) {
            addIssue(result, TimelineValidationCode::InvalidLinkGroup,
                     path + ".tracks.clips.linkGroupId",
                     "a linked clip must be listed by its declared link group");
        }
    }
    validateProperties(sequence.metadata, path + ".metadata", result);
}

void validateVideoOutput(const TimelineVideoOutput &video,
                         const std::string &path,
                         TimelineValidationResult &result)
{
    validateCodec(video.codec, path + ".codec", result,
                  TimelineValidationCode::InvalidRenderProfile);
    validateFrameRate(video.frameRate, path + ".frameRate", result);
    if (video.extent.width <= 0 || video.extent.height <= 0
        || !validRational(video.pixelAspectRatio)
        || video.pixelAspectRatio.numerator <= 0
        || video.pixelFormat.empty() || video.bitDepth == 0) {
        addIssue(result, TimelineValidationCode::InvalidRenderProfile, path,
                 "video output dimensions, pixel aspect, format, and bit depth must be explicit and positive");
    }
    validateColorDescription(video.color, path + ".color", result);
    if ((video.minimumBitRate && *video.minimumBitRate == 0)
        || (video.averageBitRate && *video.averageBitRate == 0)
        || (video.maximumBitRate && *video.maximumBitRate == 0)
        || (video.minimumBitRate && video.averageBitRate
            && *video.minimumBitRate > *video.averageBitRate)
        || (video.averageBitRate && video.maximumBitRate
            && *video.averageBitRate > *video.maximumBitRate)
        || (video.minimumBitRate && video.maximumBitRate
            && *video.minimumBitRate > *video.maximumBitRate)) {
        addIssue(result, TimelineValidationCode::InvalidRenderProfile,
                 path + ".bitRate", "video output bitrate bounds must be positive and ordered");
    }
    if (video.quality && !finite(*video.quality)) {
        addIssue(result, TimelineValidationCode::InvalidNumericValue,
                 path + ".quality", "video quality must be finite");
    }
    validateProperties(video.options, path + ".options", result);
}

void validateAudioOutput(const TimelineAudioOutput &audio,
                         const std::string &path,
                         TimelineValidationResult &result)
{
    validateCodec(audio.codec, path + ".codec", result,
                  TimelineValidationCode::InvalidRenderProfile);
    if (audio.sampleRate == 0 || audio.channelCount == 0
        || audio.channelLayout.empty()) {
        addIssue(result, TimelineValidationCode::InvalidRenderProfile, path,
                 "audio output sample rate, channel count, and layout are required");
    }
    if ((audio.bitRate && *audio.bitRate == 0)
        || (audio.quality && !finite(*audio.quality))
        || (audio.targetIntegratedLufs && !finite(*audio.targetIntegratedLufs))
        || (audio.targetTruePeakDbtp && !finite(*audio.targetTruePeakDbtp))) {
        addIssue(result, TimelineValidationCode::InvalidNumericValue, path,
                 "audio output rate, quality, and loudness values must be positive/finite");
    }
    if (audio.normalizeLoudness && !audio.targetIntegratedLufs) {
        addIssue(result, TimelineValidationCode::InvalidRenderProfile,
                 path + ".targetIntegratedLufs",
                 "loudness normalization requires an integrated loudness target");
    }
    validateProperties(audio.options, path + ".options", result);
}

void validateRenderProfile(const TimelineRenderProfile &profile,
                           const TimelineProject &project,
                           const std::string &path,
                           TimelineValidationResult &result)
{
    if (profile.id.empty()) {
        addIssue(result, TimelineValidationCode::InvalidId,
                 path + ".id", "render profile ids must not be empty");
    }
    if (!findTimelineSequence(project, profile.sequenceId)) {
        addIssue(result, TimelineValidationCode::MissingReference,
                 path + ".sequenceId", "render profile sequence does not exist");
    }
    validateContainer(profile.container, path + ".container", result,
                      TimelineValidationCode::InvalidRenderProfile);
    const TimelineSequence *sequence = findTimelineSequence(
        project, profile.sequenceId);
    if (!profile.video && !profile.audio
        && std::none_of(profile.subtitles.begin(), profile.subtitles.end(),
                        [](const TimelineSubtitleOutput &subtitle) {
                            return subtitle.mode != TimelineSubtitleOutputMode::Omit;
                        })) {
        addIssue(result, TimelineValidationCode::InvalidRenderProfile, path,
                 "render profiles require at least one enabled output stream");
    }
    if (profile.rangeMode == TimelineRenderRangeMode::Custom
        && !profile.customRange) {
        addIssue(result, TimelineValidationCode::InvalidRenderProfile,
                 path + ".customRange", "custom render range mode requires a range");
    }
    if (profile.rangeMode == TimelineRenderRangeMode::WorkArea
        && (!sequence || !sequence->workArea)) {
        addIssue(result, TimelineValidationCode::InvalidRenderProfile,
                 path + ".rangeMode",
                 "work-area render mode requires a work area on its sequence");
    }
    if (profile.customRange) {
        if (!sequence
            || !rangeInside(*profile.customRange, sequence->startTicks,
                            sequence->durationTicks, false)) {
            addIssue(result, TimelineValidationCode::InvalidRenderProfile,
                     path + ".customRange", "custom render range must fit its sequence");
        }
    }
    if (profile.video) {
        validateVideoOutput(*profile.video, path + ".video", result);
    }
    if (profile.audio) {
        validateAudioOutput(*profile.audio, path + ".audio", result);
    }
    for (std::size_t index = 0; index < profile.subtitles.size(); ++index) {
        const TimelineSubtitleOutput &subtitle = profile.subtitles[index];
        const std::string subtitlePath = path + ".subtitles["
            + std::to_string(index) + "]";
        if (subtitle.mode != TimelineSubtitleOutputMode::Omit
            && subtitle.mode != TimelineSubtitleOutputMode::BurnIn) {
            validateCodec(subtitle.codec, subtitlePath + ".codec", result,
                          TimelineValidationCode::InvalidRenderProfile);
        }
        if (subtitle.mode == TimelineSubtitleOutputMode::BurnIn && !profile.video) {
            addIssue(result, TimelineValidationCode::InvalidRenderProfile,
                     subtitlePath, "burn-in subtitles require a video output");
        }
        if (subtitle.mode == TimelineSubtitleOutputMode::Sidecar
            && subtitle.sidecarUri.empty()) {
            addIssue(result, TimelineValidationCode::InvalidRenderProfile,
                     subtitlePath + ".sidecarUri",
                     "sidecar subtitle output requires a destination URI");
        }
        validateProperties(subtitle.options, subtitlePath + ".options", result);
    }
    if (profile.fragmentDurationTicks && *profile.fragmentDurationTicks <= 0) {
        addIssue(result, TimelineValidationCode::InvalidRenderProfile,
                 path + ".fragmentDurationTicks",
                 "fragment duration must be positive when present");
    }
    validateProperties(profile.muxOptions, path + ".muxOptions", result);
    validateProperties(profile.metadata, path + ".metadata", result);
}

template<typename Collection, typename IdAccessor>
void validateUniqueIds(const Collection &collection,
                       IdAccessor idAccessor,
                       const std::string &path,
                       TimelineValidationResult &result)
{
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < collection.size(); ++index) {
        const std::string &id = idAccessor(collection[index]);
        if (id.empty()) {
            addIssue(result, TimelineValidationCode::InvalidId,
                     path + "[" + std::to_string(index) + "].id",
                     "stable ids must not be empty");
        } else if (!ids.insert(id).second) {
            addIssue(result, TimelineValidationCode::DuplicateId,
                     path + "[" + std::to_string(index) + "].id",
                     "stable ids must be unique in their scope");
        }
    }
}

bool sequenceGraphHasCycle(const TimelineProject &project)
{
    std::unordered_map<std::string, std::vector<std::string>> edges;
    for (const TimelineSequence &sequence : project.sequences) {
        auto &targets = edges[sequence.id];
        for (const TimelineTrack &track : sequence.tracks) {
            std::visit([&](const auto &typedTrack) {
                for (const auto &clip : typedTrack.clips) {
                    if (const auto *reference = std::get_if<TimelineSequenceReference>(
                            &clip.properties.source)) {
                        targets.push_back(reference->sequenceId);
                    }
                }
            }, track);
        }
    }

    enum class Visit : std::uint8_t { None, Visiting, Complete };
    std::unordered_map<std::string, Visit> visits;
    const auto visit = [&](const auto &self, const std::string &id) -> bool {
        Visit &state = visits[id];
        if (state == Visit::Visiting) {
            return true;
        }
        if (state == Visit::Complete) {
            return false;
        }
        state = Visit::Visiting;
        for (const std::string &target : edges[id]) {
            if (self(self, target)) {
                return true;
            }
        }
        state = Visit::Complete;
        return false;
    };
    for (const auto &[id, targets] : edges) {
        (void) targets;
        if (visit(visit, id)) {
            return true;
        }
    }
    return false;
}

bool binGraphHasCycle(const TimelineProject &project)
{
    std::unordered_map<std::string, std::string> parents;
    for (const TimelineBin &bin : project.bins) {
        parents.emplace(bin.id, bin.parentBinId);
    }

    enum class Visit : std::uint8_t { None, Visiting, Complete };
    std::unordered_map<std::string, Visit> visits;
    const auto visit = [&](const auto &self, const std::string &id) -> bool {
        Visit &state = visits[id];
        if (state == Visit::Visiting) {
            return true;
        }
        if (state == Visit::Complete) {
            return false;
        }
        state = Visit::Visiting;
        const auto parent = parents.find(id);
        if (parent != parents.end() && !parent->second.empty()
            && parents.find(parent->second) != parents.end()
            && self(self, parent->second)) {
            return true;
        }
        state = Visit::Complete;
        return false;
    };
    for (const auto &[id, parent] : parents) {
        (void) parent;
        if (visit(visit, id)) {
            return true;
        }
    }
    return false;
}

} // namespace

TimelineClipView::TimelineClipView(TimelineVideoClip *clip) noexcept
{
    if (clip) {
        m_pointer = clip;
    }
}

TimelineClipView::TimelineClipView(TimelineAudioClip *clip) noexcept
{
    if (clip) {
        m_pointer = clip;
    }
}

TimelineClipView::TimelineClipView(TimelineSubtitleClip *clip) noexcept
{
    if (clip) {
        m_pointer = clip;
    }
}

TimelineClipView::TimelineClipView(TimelineDataClip *clip) noexcept
{
    if (clip) {
        m_pointer = clip;
    }
}

TimelineClipView::operator bool() const noexcept
{
    return m_pointer.index() != 0;
}

TimelineClipView TimelineClipView::operator*() const noexcept
{
    return *this;
}

bool TimelineClipView::operator==(std::nullptr_t) const noexcept
{
    return !static_cast<bool>(*this);
}

bool TimelineClipView::operator!=(std::nullptr_t) const noexcept
{
    return static_cast<bool>(*this);
}

TimelineConstClipView::TimelineConstClipView(const TimelineVideoClip *clip) noexcept
{
    if (clip) {
        m_pointer = clip;
    }
}

TimelineConstClipView::TimelineConstClipView(const TimelineAudioClip *clip) noexcept
{
    if (clip) {
        m_pointer = clip;
    }
}

TimelineConstClipView::TimelineConstClipView(const TimelineSubtitleClip *clip) noexcept
{
    if (clip) {
        m_pointer = clip;
    }
}

TimelineConstClipView::TimelineConstClipView(const TimelineDataClip *clip) noexcept
{
    if (clip) {
        m_pointer = clip;
    }
}

TimelineConstClipView::operator bool() const noexcept
{
    return m_pointer.index() != 0;
}

TimelineConstClipView TimelineConstClipView::operator*() const noexcept
{
    return *this;
}

bool TimelineConstClipView::operator==(std::nullptr_t) const noexcept
{
    return !static_cast<bool>(*this);
}

bool TimelineConstClipView::operator!=(std::nullptr_t) const noexcept
{
    return static_cast<bool>(*this);
}

std::optional<double> timelineTicksToSeconds(
    std::int64_t ticks,
    TimelineTimeBase timeBase) noexcept
{
    if (!validTimeBase(timeBase)) {
        return std::nullopt;
    }
    const long double seconds = static_cast<long double>(ticks)
        * static_cast<long double>(timeBase.numerator)
        / static_cast<long double>(timeBase.denominator);
    if (!std::isfinite(seconds)
        || seconds > std::numeric_limits<double>::max()
        || seconds < -std::numeric_limits<double>::max()) {
        return std::nullopt;
    }
    return static_cast<double>(seconds);
}

TimelineStreamKind timelineStreamKind(const TimelineMediaStream &stream) noexcept
{
    return std::visit([](const auto &value) {
        using Stream = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Stream, TimelineVideoStream>) {
            return TimelineStreamKind::Video;
        } else if constexpr (std::is_same_v<Stream, TimelineAudioStream>) {
            return TimelineStreamKind::Audio;
        } else if constexpr (std::is_same_v<Stream, TimelineSubtitleStream>) {
            return TimelineStreamKind::Subtitle;
        } else {
            return TimelineStreamKind::Data;
        }
    }, stream);
}

TimelineStreamKind timelineTrackKind(const TimelineTrack &track) noexcept
{
    return std::visit([](const auto &value) {
        using Track = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Track, TimelineVideoTrack>) {
            return TimelineStreamKind::Video;
        } else if constexpr (std::is_same_v<Track, TimelineAudioTrack>) {
            return TimelineStreamKind::Audio;
        } else if constexpr (std::is_same_v<Track, TimelineSubtitleTrack>) {
            return TimelineStreamKind::Subtitle;
        } else {
            return TimelineStreamKind::Data;
        }
    }, track);
}

TimelineStreamKind timelineClipKind(const TimelineClip &clip) noexcept
{
    return std::visit([](const auto &value) {
        using Clip = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Clip, TimelineVideoClip>) {
            return TimelineStreamKind::Video;
        } else if constexpr (std::is_same_v<Clip, TimelineAudioClip>) {
            return TimelineStreamKind::Audio;
        } else if constexpr (std::is_same_v<Clip, TimelineSubtitleClip>) {
            return TimelineStreamKind::Subtitle;
        } else {
            return TimelineStreamKind::Data;
        }
    }, clip);
}

std::optional<TimelineStreamKind> timelineClipKind(TimelineClipView clip) noexcept
{
    if (!clip) {
        return std::nullopt;
    }
    return std::visit([](const auto &pointer) -> TimelineStreamKind {
        using Pointer = std::decay_t<decltype(pointer)>;
        if constexpr (std::is_same_v<Pointer, TimelineVideoClip *>) {
            return TimelineStreamKind::Video;
        } else if constexpr (std::is_same_v<Pointer, TimelineAudioClip *>) {
            return TimelineStreamKind::Audio;
        } else if constexpr (std::is_same_v<Pointer, TimelineSubtitleClip *>) {
            return TimelineStreamKind::Subtitle;
        } else {
            return TimelineStreamKind::Data;
        }
    }, clip.m_pointer);
}

std::optional<TimelineStreamKind> timelineClipKind(
    TimelineConstClipView clip) noexcept
{
    if (!clip) {
        return std::nullopt;
    }
    return std::visit([](const auto &pointer) -> TimelineStreamKind {
        using Pointer = std::decay_t<decltype(pointer)>;
        if constexpr (std::is_same_v<Pointer, const TimelineVideoClip *>) {
            return TimelineStreamKind::Video;
        } else if constexpr (std::is_same_v<Pointer, const TimelineAudioClip *>) {
            return TimelineStreamKind::Audio;
        } else if constexpr (std::is_same_v<Pointer, const TimelineSubtitleClip *>) {
            return TimelineStreamKind::Subtitle;
        } else {
            return TimelineStreamKind::Data;
        }
    }, clip.m_pointer);
}

TimelineTrackProperties &timelineTrackProperties(TimelineTrack &track) noexcept
{
    return std::visit([](auto &value) -> TimelineTrackProperties & {
        return value.properties;
    }, track);
}

const TimelineTrackProperties &timelineTrackProperties(
    const TimelineTrack &track) noexcept
{
    return std::visit([](const auto &value) -> const TimelineTrackProperties & {
        return value.properties;
    }, track);
}

TimelineClipProperties &timelineClipProperties(TimelineClip &clip) noexcept
{
    return std::visit([](auto &value) -> TimelineClipProperties & {
        return value.properties;
    }, clip);
}

const TimelineClipProperties &timelineClipProperties(
    const TimelineClip &clip) noexcept
{
    return std::visit([](const auto &value) -> const TimelineClipProperties & {
        return value.properties;
    }, clip);
}

TimelineClipProperties &timelineClipProperties(TimelineClipView clip) noexcept
{
    TimelineClipProperties *properties = std::visit([](auto pointer)
        -> TimelineClipProperties * {
        using Pointer = decltype(pointer);
        if constexpr (std::is_same_v<Pointer, std::monostate>) {
            return nullptr;
        } else {
            return pointer ? &pointer->properties : nullptr;
        }
    }, clip.m_pointer);
    if (!properties) {
        std::terminate();
    }
    return *properties;
}

const TimelineClipProperties &timelineClipProperties(
    TimelineConstClipView clip) noexcept
{
    const TimelineClipProperties *properties = std::visit([](auto pointer)
        -> const TimelineClipProperties * {
        using Pointer = decltype(pointer);
        if constexpr (std::is_same_v<Pointer, std::monostate>) {
            return nullptr;
        } else {
            return pointer ? &pointer->properties : nullptr;
        }
    }, clip.m_pointer);
    if (!properties) {
        std::terminate();
    }
    return *properties;
}

TimelineClipProperties &timelineClipProperties(
    TimelineClipProperties &properties) noexcept
{
    return properties;
}

const TimelineClipProperties &timelineClipProperties(
    const TimelineClipProperties &properties) noexcept
{
    return properties;
}

TimelineMediaSource *findTimelineMediaSource(
    TimelineProject &project,
    const std::string &id) noexcept
{
    return const_cast<TimelineMediaSource *>(findTimelineMediaSource(
        static_cast<const TimelineProject &>(project), id));
}

const TimelineMediaSource *findTimelineMediaSource(
    const TimelineProject &project,
    const std::string &id) noexcept
{
    const auto found = std::find_if(project.mediaSources.begin(),
                                    project.mediaSources.end(),
                                    [&](const TimelineMediaSource &source) {
        return source.id == id;
    });
    return found == project.mediaSources.end() ? nullptr : &*found;
}

TimelineMediaRepresentation *findTimelineMediaRepresentation(
    TimelineMediaSource &source,
    const std::string &id) noexcept
{
    return const_cast<TimelineMediaRepresentation *>(
        findTimelineMediaRepresentation(
            static_cast<const TimelineMediaSource &>(source), id));
}

const TimelineMediaRepresentation *findTimelineMediaRepresentation(
    const TimelineMediaSource &source,
    const std::string &id) noexcept
{
    const auto found = std::find_if(source.representations.begin(),
                                    source.representations.end(),
                                    [&](const TimelineMediaRepresentation &representation) {
        return representation.id == id;
    });
    return found == source.representations.end() ? nullptr : &*found;
}

TimelineMediaStream *findTimelineMediaStream(
    TimelineMediaSource &source,
    const std::string &id) noexcept
{
    return const_cast<TimelineMediaStream *>(findTimelineMediaStream(
        static_cast<const TimelineMediaSource &>(source), id));
}

const TimelineMediaStream *findTimelineMediaStream(
    const TimelineMediaSource &source,
    const std::string &id) noexcept
{
    const auto findInRepresentation = [&](const std::string &representationId)
        -> const TimelineMediaStream * {
        const TimelineMediaRepresentation *representation
            = findTimelineMediaRepresentation(source, representationId);
        if (!representation) {
            return nullptr;
        }
        const auto found = std::find_if(representation->streams.begin(),
                                        representation->streams.end(),
                                        [&](const TimelineMediaStream &stream) {
            return streamId(stream) == id;
        });
        return found == representation->streams.end() ? nullptr : &*found;
    };

    if (const TimelineMediaStream *active
            = findInRepresentation(source.activeRepresentationId)) {
        return active;
    }
    if (source.originalRepresentationId != source.activeRepresentationId) {
        if (const TimelineMediaStream *original
                = findInRepresentation(source.originalRepresentationId)) {
            return original;
        }
    }
    for (const TimelineMediaRepresentation &representation : source.representations) {
        const auto found = std::find_if(representation.streams.begin(),
                                        representation.streams.end(),
                                        [&](const TimelineMediaStream &stream) {
            return streamId(stream) == id;
        });
        if (found != representation.streams.end()) {
            return &*found;
        }
    }
    return nullptr;
}

TimelineSequence *findTimelineSequence(TimelineProject &project,
                                       const std::string &id) noexcept
{
    return const_cast<TimelineSequence *>(findTimelineSequence(
        static_cast<const TimelineProject &>(project), id));
}

const TimelineSequence *findTimelineSequence(const TimelineProject &project,
                                             const std::string &id) noexcept
{
    const auto found = std::find_if(project.sequences.begin(),
                                    project.sequences.end(),
                                    [&](const TimelineSequence &sequence) {
        return sequence.id == id;
    });
    return found == project.sequences.end() ? nullptr : &*found;
}

TimelineTrack *findTimelineTrack(TimelineSequence &sequence,
                                 const std::string &id) noexcept
{
    return const_cast<TimelineTrack *>(findTimelineTrack(
        static_cast<const TimelineSequence &>(sequence), id));
}

const TimelineTrack *findTimelineTrack(const TimelineSequence &sequence,
                                       const std::string &id) noexcept
{
    const auto found = std::find_if(sequence.tracks.begin(), sequence.tracks.end(),
                                    [&](const TimelineTrack &track) {
        return timelineTrackProperties(track).id == id;
    });
    return found == sequence.tracks.end() ? nullptr : &*found;
}

TimelineClipView findTimelineClip(TimelineSequence &sequence,
                                  const std::string &id) noexcept
{
    for (TimelineTrack &track : sequence.tracks) {
        TimelineClipView found = std::visit([&](auto &typedTrack) -> TimelineClipView {
            using Track = std::decay_t<decltype(typedTrack)>;
            if constexpr (std::is_same_v<Track, TimelineVideoTrack>) {
                return TimelineClipView(findTypedClip(typedTrack.clips, id));
            } else if constexpr (std::is_same_v<Track, TimelineAudioTrack>) {
                return TimelineClipView(findTypedClip(typedTrack.clips, id));
            } else {
                return TimelineClipView(findTypedClip(typedTrack.clips, id));
            }
        }, track);
        if (found) {
            return found;
        }
    }
    return {};
}

TimelineConstClipView findTimelineClip(const TimelineSequence &sequence,
                                       const std::string &id) noexcept
{
    for (const TimelineTrack &track : sequence.tracks) {
        TimelineConstClipView found = std::visit(
            [&](const auto &typedTrack) -> TimelineConstClipView {
                return TimelineConstClipView(findTypedClip(typedTrack.clips, id));
            }, track);
        if (found) {
            return found;
        }
    }
    return {};
}

TimelineVideoClip *findTimelineVideoClip(TimelineSequence &sequence,
                                         const std::string &id) noexcept
{
    for (TimelineTrack &track : sequence.tracks) {
        if (auto *video = std::get_if<TimelineVideoTrack>(&track)) {
            if (TimelineVideoClip *clip = findTypedClip(video->clips, id)) {
                return clip;
            }
        }
    }
    return nullptr;
}

TimelineAudioClip *findTimelineAudioClip(TimelineSequence &sequence,
                                         const std::string &id) noexcept
{
    for (TimelineTrack &track : sequence.tracks) {
        if (auto *audio = std::get_if<TimelineAudioTrack>(&track)) {
            if (TimelineAudioClip *clip = findTypedClip(audio->clips, id)) {
                return clip;
            }
        }
    }
    return nullptr;
}

TimelineSubtitleClip *findTimelineSubtitleClip(TimelineSequence &sequence,
                                               const std::string &id) noexcept
{
    for (TimelineTrack &track : sequence.tracks) {
        if (auto *subtitle = std::get_if<TimelineSubtitleTrack>(&track)) {
            if (TimelineSubtitleClip *clip = findTypedClip(subtitle->clips, id)) {
                return clip;
            }
        }
    }
    return nullptr;
}

TimelineDataClip *findTimelineDataClip(TimelineSequence &sequence,
                                       const std::string &id) noexcept
{
    for (TimelineTrack &track : sequence.tracks) {
        if (auto *data = std::get_if<TimelineDataTrack>(&track)) {
            if (TimelineDataClip *clip = findTypedClip(data->clips, id)) {
                return clip;
            }
        }
    }
    return nullptr;
}

TimelineRenderProfile *findTimelineRenderProfile(
    TimelineProject &project,
    const std::string &id) noexcept
{
    return const_cast<TimelineRenderProfile *>(findTimelineRenderProfile(
        static_cast<const TimelineProject &>(project), id));
}

const TimelineRenderProfile *findTimelineRenderProfile(
    const TimelineProject &project,
    const std::string &id) noexcept
{
    const auto found = std::find_if(project.renderProfiles.begin(),
                                    project.renderProfiles.end(),
                                    [&](const TimelineRenderProfile &profile) {
        return profile.id == id;
    });
    return found == project.renderProfiles.end() ? nullptr : &*found;
}

TimelineValidationResult validateTimelineProject(const TimelineProject &project)
{
    TimelineValidationResult result;
    if (project.id.empty()) {
        addIssue(result, TimelineValidationCode::InvalidProject,
                 "project.id", "timeline project id must not be empty");
    }
    validateProperties(project.metadata, "project.metadata", result);
    validateUniqueIds(project.mediaSources,
                      [](const TimelineMediaSource &source) -> const std::string & {
        return source.id;
    }, "project.mediaSources", result);
    validateUniqueIds(project.sequences,
                      [](const TimelineSequence &sequence) -> const std::string & {
        return sequence.id;
    }, "project.sequences", result);
    validateUniqueIds(project.bins,
                      [](const TimelineBin &bin) -> const std::string & {
        return bin.id;
    }, "project.bins", result);
    validateUniqueIds(project.renderProfiles,
                      [](const TimelineRenderProfile &profile) -> const std::string & {
        return profile.id;
    }, "project.renderProfiles", result);

    if (!project.activeSequenceId.empty()
        && !findTimelineSequence(project, project.activeSequenceId)) {
        addIssue(result, TimelineValidationCode::MissingReference,
                 "project.activeSequenceId",
                 "active sequence must reference an existing sequence");
    }

    for (std::size_t sourceIndex = 0;
         sourceIndex < project.mediaSources.size(); ++sourceIndex) {
        const TimelineMediaSource &source = project.mediaSources[sourceIndex];
        const std::string sourcePath = "project.mediaSources["
            + std::to_string(sourceIndex) + "]";
        validateProperties(source.metadata, sourcePath + ".metadata", result);
        validateUniqueIds(source.representations,
                          [](const TimelineMediaRepresentation &representation)
                              -> const std::string & {
            return representation.id;
        }, sourcePath + ".representations", result);
        const TimelineMediaRepresentation *original
            = findTimelineMediaRepresentation(source,
                                              source.originalRepresentationId);
        const TimelineMediaRepresentation *active
            = findTimelineMediaRepresentation(source,
                                              source.activeRepresentationId);
        if (!original || original->role != TimelineMediaRepresentationRole::Original) {
            addIssue(result, TimelineValidationCode::MissingReference,
                     sourcePath + ".originalRepresentationId",
                     "original representation must exist and have the Original role");
        }
        if (!active) {
            addIssue(result, TimelineValidationCode::MissingReference,
                     sourcePath + ".activeRepresentationId",
                     "active representation must exist");
        }

        struct LogicalStreamContract {
            TimelineStreamKind kind = TimelineStreamKind::Data;
            TimelineTimeBase timeBase;
            std::int64_t startTicks = 0;
            std::int64_t durationTicks = 0;
        };
        std::unordered_map<std::string, LogicalStreamContract> logicalStreams;
        for (std::size_t representationIndex = 0;
             representationIndex < source.representations.size();
             ++representationIndex) {
            const TimelineMediaRepresentation &representation
                = source.representations[representationIndex];
            const std::string representationPath = sourcePath + ".representations["
                + std::to_string(representationIndex) + "]";
            if (representation.role != TimelineMediaRepresentationRole::OfflinePlaceholder
                && representation.uri.empty()) {
                addIssue(result, TimelineValidationCode::InvalidRepresentation,
                         representationPath + ".uri",
                         "online representations require a URI");
            }
            validateContainer(representation.container,
                              representationPath + ".container", result,
                              TimelineValidationCode::InvalidRepresentation);
            validateTimeBase(representation.timeBase,
                             representationPath + ".timeBase", result);
            if (representation.durationTicks < 0) {
                addIssue(result, TimelineValidationCode::InvalidRepresentation,
                         representationPath + ".durationTicks",
                         "representation duration must not be negative");
            }
            if (representation.fileSizeBytes
                && *representation.fileSizeBytes == 0) {
                addIssue(result, TimelineValidationCode::InvalidRepresentation,
                         representationPath + ".fileSizeBytes",
                         "present representation file size must be positive");
            }
            if (!representation.checksum.empty()
                && representation.checksumAlgorithm.empty()) {
                addIssue(result, TimelineValidationCode::InvalidRepresentation,
                         representationPath + ".checksumAlgorithm",
                         "a checksum requires an algorithm identifier");
            }
            if (representation.timecode) {
                validateTimecode(*representation.timecode,
                                 representationPath + ".timecode", result);
            }
            validateUniqueIds(representation.streams,
                              [](const TimelineMediaStream &stream)
                                  -> const std::string & {
                return std::visit([](const auto &value)
                    -> const std::string & { return value.id; }, stream);
            }, representationPath + ".streams", result);
            std::unordered_set<std::uint32_t> streamIndices;
            for (std::size_t streamPosition = 0;
                 streamPosition < representation.streams.size(); ++streamPosition) {
                const TimelineMediaStream &stream
                    = representation.streams[streamPosition];
                const std::string streamPath = representationPath + ".streams["
                    + std::to_string(streamPosition) + "]";
                if (!streamIndices.insert(streamIndex(stream)).second) {
                    addIssue(result, TimelineValidationCode::DuplicateId,
                             streamPath + ".streamIndex",
                             "container stream indexes must be unique per representation");
                }
                const std::string id(streamId(stream));
                const TimelineStreamKind kind = timelineStreamKind(stream);
                const LogicalStreamContract contract = std::visit(
                    [kind](const auto &value) {
                        return LogicalStreamContract{
                            kind, value.timeBase, value.startTicks,
                            value.durationTicks};
                    }, stream);
                const auto [position, inserted]
                    = logicalStreams.emplace(id, contract);
                if (!inserted && position->second.kind != kind) {
                    addIssue(result, TimelineValidationCode::StreamKindMismatch,
                             streamPath + ".id",
                             "a logical stream id must keep one kind across representations");
                }
                if (!inserted
                    && (position->second.timeBase != contract.timeBase
                        || position->second.startTicks != contract.startTicks
                        || position->second.durationTicks
                            != contract.durationTicks)) {
                    addIssue(result,
                             TimelineValidationCode::InvalidRepresentation,
                             streamPath,
                             "alternate representations must preserve logical stream time base and range");
                }
                validateStream(stream, streamPath, result);
                if (const auto *video = std::get_if<TimelineVideoStream>(&stream)) {
                    for (std::size_t sampleIndex = 0;
                         sampleIndex < video->timing.samples.size();
                         ++sampleIndex) {
                        const TimelineVideoSampleTiming &sample
                            = video->timing.samples[sampleIndex];
                        const std::string samplePath = streamPath + ".timing.samples["
                            + std::to_string(sampleIndex) + "]";
                        bool invalidBytes = sample.byteSize && *sample.byteSize == 0;
                        if (sample.byteOffset && sample.byteSize) {
                            invalidBytes = invalidBytes
                                || *sample.byteOffset
                                    > std::numeric_limits<std::uint64_t>::max()
                                        - *sample.byteSize;
                            if (!invalidBytes && representation.fileSizeBytes) {
                                invalidBytes = *sample.byteOffset + *sample.byteSize
                                    > *representation.fileSizeBytes;
                            }
                        } else if (sample.byteOffset
                                   && representation.fileSizeBytes) {
                            invalidBytes = *sample.byteOffset
                                >= *representation.fileSizeBytes;
                        } else if (sample.byteSize
                                   && representation.fileSizeBytes) {
                            invalidBytes = *sample.byteSize
                                > *representation.fileSizeBytes;
                        }
                        if (invalidBytes) {
                            addIssue(result, TimelineValidationCode::InvalidStream,
                                     samplePath,
                                     "sample byte ranges must be positive, safe, and fit the known file size");
                        }
                    }
                }
            }
            std::unordered_set<std::string> attachmentIds;
            for (std::size_t attachmentIndex = 0;
                 attachmentIndex < representation.attachments.size();
                 ++attachmentIndex) {
                const TimelineAttachment &attachment
                    = representation.attachments[attachmentIndex];
                if (attachment.id.empty()
                    || !attachmentIds.insert(attachment.id).second
                    || (attachment.uri.empty() && attachment.data.empty())) {
                    addIssue(result, TimelineValidationCode::InvalidRepresentation,
                             representationPath + ".attachments["
                                 + std::to_string(attachmentIndex) + "]",
                             "attachments require a unique id and URI or owned data");
                }
            }
            for (std::size_t streamPosition = 0;
                 streamPosition < representation.streams.size();
                 ++streamPosition) {
                const auto *subtitle = std::get_if<TimelineSubtitleStream>(
                    &representation.streams[streamPosition]);
                if (!subtitle) {
                    continue;
                }
                for (std::size_t cueIndex = 0;
                     cueIndex < subtitle->cues.size(); ++cueIndex) {
                    const TimelineSubtitleCue &cue = subtitle->cues[cueIndex];
                    if (!cue.imageResourceId.empty()
                        && attachmentIds.find(cue.imageResourceId)
                            == attachmentIds.end()) {
                        addIssue(result, TimelineValidationCode::MissingReference,
                                 representationPath + ".streams["
                                     + std::to_string(streamPosition)
                                     + "].cues[" + std::to_string(cueIndex)
                                     + "].imageResourceId",
                                 "subtitle image resources must reference an attachment in their representation");
                    }
                }
            }
            validateProperties(representation.metadata,
                               representationPath + ".metadata", result);
        }
    }

    for (std::size_t index = 0; index < project.sequences.size(); ++index) {
        validateSequence(project.sequences[index], project,
                         "project.sequences[" + std::to_string(index) + "]",
                         result);
    }
    if (sequenceGraphHasCycle(project)) {
        addIssue(result, TimelineValidationCode::InvalidSequence,
                 "project.sequences", "nested sequence references must be acyclic");
    }

    std::unordered_set<std::string> binIds;
    for (const TimelineBin &bin : project.bins) {
        binIds.insert(bin.id);
    }
    for (std::size_t index = 0; index < project.bins.size(); ++index) {
        const TimelineBin &bin = project.bins[index];
        const std::string binPath = "project.bins[" + std::to_string(index) + "]";
        if (!bin.parentBinId.empty()
            && binIds.find(bin.parentBinId) == binIds.end()) {
            addIssue(result, TimelineValidationCode::MissingReference,
                     binPath + ".parentBinId", "parent bin does not exist");
        }
        for (const std::string &sourceId : bin.mediaSourceIds) {
            if (!findTimelineMediaSource(project, sourceId)) {
                addIssue(result, TimelineValidationCode::MissingReference,
                         binPath + ".mediaSourceIds",
                         "bin media source does not exist");
            }
        }
        for (const std::string &sequenceId : bin.sequenceIds) {
            if (!findTimelineSequence(project, sequenceId)) {
                addIssue(result, TimelineValidationCode::MissingReference,
                         binPath + ".sequenceIds", "bin sequence does not exist");
            }
        }
        validateProperties(bin.metadata, binPath + ".metadata", result);
    }
    if (binGraphHasCycle(project)) {
        addIssue(result, TimelineValidationCode::InvalidBin,
                 "project.bins",
                 "bin parent relationships must remain acyclic");
    }

    for (std::size_t index = 0; index < project.renderProfiles.size(); ++index) {
        validateRenderProfile(project.renderProfiles[index], project,
                              "project.renderProfiles["
                                  + std::to_string(index) + "]",
                              result);
    }
    return result;
}

} // namespace iiSharedCanvas
