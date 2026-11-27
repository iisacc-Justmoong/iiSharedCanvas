#pragma once

#include "Timeline/TimelineProject.h"
#include "iiSharedCanvas/Export.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iiSharedCanvas {

// Arbitrary-size normalized rational, represented with decimal integers so the
// public API does not depend on a particular multiprecision implementation.
struct TimelineExactRational {
    std::string numerator, denominator;
};

// Exact source position B_y(u), where u is the unique root in [0,1] of
// B_x(u)=target. Controls and target are exact rationals in timeline ticks;
// no rounded approximation is used to represent an irrational source time.
struct TimelineBezierSourcePosition {
    std::array<TimelineExactRational,4> timeControl, valueControl;
    TimelineExactRational target;
};

// One interpolation in a nested clip path. The interpolation phase is exactly
// (source position - lowerSampleTime) / (upperSampleTime - lowerSampleTime).
// For a Bezier source position, sourcePositionBezier contains the expression;
// otherwise sourcePosition has a nonzero denominator. The upper flag identifies
// the dependency represented by this particular source-frame match.
struct TimelineFrameInterpolation {
    std::string clipId;
    TimelineVideoSampling mode = TimelineVideoSampling::Blend;
    std::uint64_t lowerFrameIndex = 0, upperFrameIndex = 0;
    TimelineExactRational lowerSampleTime, upperSampleTime;
    TimelineRational sourcePosition;
    std::optional<TimelineBezierSourcePosition> sourcePositionBezier;
    bool upper = false;
};

// A source video frame reached through a path of one or more sequence clips.
// sourcePosition is exact when its denominator is nonzero. Otherwise the
// sourcePositionBezier expression owns the exact value in stream tick units.
struct TimelineSourceFrameMatch {
    std::string mediaSourceId, representationId, streamId;
    std::uint64_t sourceFrameIndex = 0;
    TimelineRational sourcePosition;
    std::vector<std::string> clipPath;
    std::optional<TimelineBezierSourcePosition> sourcePositionBezier;
    std::vector<TimelineFrameInterpolation> interpolationPath;
};
struct TimelineSourceFrameResult {
    std::vector<TimelineSourceFrameMatch> matches;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};
struct TimelineFrameOccurrence {
    std::uint64_t sequenceFrameIndex = 0;
    TimelineSourceFrameMatch source;
};
struct TimelineFrameOccurrencesResult {
    std::vector<TimelineFrameOccurrence> occurrences;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Resolves the start of a sequence frame through enabled video clips, rate and
// exact Bezier, linear/hold time maps, loops and nesting. Explicit VFR sample intervals take
// precedence over nominal rates. Previous and nearest sampling are supported;
// blend and optical-flow expose all contributing frames and exact interpolation
// intervals. Arithmetic budget failures return an error without partial matches.
// Inputs are borrowed only for the call, and returned metadata owns its data.
class IISHAREDCANVAS_EXPORT SourceFrameMatcher final {
public:
    [[nodiscard]] TimelineSourceFrameResult operator()(const TimelineProject &, const std::string &sequenceId,
        std::uint64_t sequenceFrameIndex, std::size_t maximumMatches = 65536) const;
};

// Exhaustively enumerates sequence frame positions up to the caller's explicit
// budget. If the sequence exceeds that budget it returns an error, not a partial
// answer. Source, stream and frame identity are matched after nested traversal.
class IISHAREDCANVAS_EXPORT ReverseFrameMatcher final {
public:
    [[nodiscard]] TimelineFrameOccurrencesResult operator()(const TimelineProject &, const std::string &sequenceId,
        const std::string &mediaSourceId, const std::string &streamId, std::uint64_t sourceFrameIndex,
        std::uint64_t maximumSequenceFrames, std::size_t maximumMatches = 65536) const;
};
} // namespace iiSharedCanvas
