#pragma once

#include "Timeline/TimelineProject.h"
#include "iiSharedCanvas/Export.h"

#include <cstdint>
#include <exception>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iiSharedCanvas {

// A detached candidate. It owns its project, does not publish changes, and
// is populated only when the requested operation and validation both succeed.
struct TimelineProjectPlan {
    std::optional<TimelineProject> project;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty() && project.has_value(); }
};

// Absent settings remain unchanged. A sequence's time base and contents are
// deliberately separate: changing tick units requires a dedicated conversion.
struct SequenceSettingsChange {
    std::optional<std::string> name;
    std::optional<TimelineFrameRate> editingFrameRate;
    std::optional<TimelineExtent> canvasExtent;
    std::optional<TimelineRational> pixelAspectRatio;
    std::optional<std::uint32_t> audioSampleRate;
    std::optional<std::string> audioChannelLayout;
    std::optional<std::optional<TimelineTimecode>> timecode;
};

// Computes a validated replacement without retaining the borrowed project or
// accessing external state. The caller publishes it through document history.
// Calls may run concurrently when inputs are not being mutated by another owner.
class IISHAREDCANVAS_EXPORT SequenceSettingsEditor final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
                                                  const SequenceSettingsChange &) const;
};

// One unoccupied interval on a track, in its sequence's tick units.
struct TimelineTrackGap {
    std::string trackId;
    TimelineTickRange range;
};
struct TimelineGapIndexResult {
    std::vector<TimelineTrackGap> gaps;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Indexes the complement of all clip ranges, including disabled clips, inside
// the sequence. Overlapping and touching occupied ranges are merged. Track IDs
// must be unique and present; on error no partial index is returned.
class IISHAREDCANVAS_EXPORT TimelineGapIndex final {
public:
    [[nodiscard]] TimelineGapIndexResult operator()(const TimelineSequence &,
                                                     const std::vector<std::string> &trackIds) const;
};

// Uniform changes to explicitly selected tracks. A locked track may only be
// unlocked by itself; applying other changes requires a later operation.
struct TimelineTrackChanges {
    std::optional<bool> enabled, locked, muted, solo, syncLocked;
    std::optional<std::string> role, language, blendMode, outputBusId;
    std::optional<double> opacity;
};

// Applies the entire batch to a detached candidate. All identities, track
// locks and resulting project invariants are checked before returning it.
class IISHAREDCANVAS_EXPORT TimelineTrackBatchEdit final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
                                                  const std::vector<std::string> &trackIds,
                                                  const TimelineTrackChanges &) const;
};

enum class CaptionTrackAction { Upsert, Remove, SetLanguage };
// A caption clip is a complete value: its style, timing and source are checked
// by the existing typed timeline validator. SetLanguage changes the track only.
struct CaptionTrackChange {
    CaptionTrackAction action = CaptionTrackAction::Upsert;
    TimelineSubtitleClip clip;
    std::string clipId;
    std::optional<std::string> language;
};

// Produces a validated caption-track replacement. A locked track is rejected.
// Removal that would leave a transition or link group dangling is rejected;
// callers can explicitly plan related changes before retrying the operation.
class IISHAREDCANVAS_EXPORT CaptionTrackEditor final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
                                                  const std::string &trackId, const CaptionTrackChange &) const;
};

struct ThroughEdit {
    std::string trackId, fromClipId, toClipId;
    std::int64_t cutTicks = 0;
};
struct ThroughEditResult {
    std::vector<ThroughEdit> edits;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Finds safe adjacent constant-rate video/audio cuts. Reverse source order is
// respected. Time maps, boundary transitions and animated clip effects are
// intentionally not asserted to be continuous by this conservative query.
class IISHAREDCANVAS_EXPORT ThroughEditDetector final {
public:
    [[nodiscard]] ThroughEditResult operator()(const TimelineProject &, const std::string &sequenceId,
                                                const std::vector<std::string> &trackIds) const;
};

// Transition identity and duration only; placement and media handles are a
// separate decision. Type strings can name caller-provided implementations.
struct DefaultTransitionResult {
    std::string typeId;
    std::int64_t durationTicks = 0;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Returns the configured type, or the video/audio built-in defaults. The
// caller supplies duration in the destination sequence's time base.
class IISHAREDCANVAS_EXPORT DefaultTransitionPolicy final {
public:
    [[nodiscard]] DefaultTransitionResult operator()(TimelineStreamKind, std::int64_t durationTicks,
                                                       std::string typeOverride = {}) const;
};

// Places a caller-described transition on a detached project. Both endpoints,
// alignment, track lock, overlapping transitions and finite source handles are
// checked before publication. Unknown source duration or non-linear time maps
// cannot establish required exterior handles and are rejected explicitly.
class IISHAREDCANVAS_EXPORT TimelineTransitionPlacement final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
                                                  const std::string &trackId, const TimelineTransition &) const;
};

struct TimelineSplitTarget { std::string clipId, rightClipId; };
struct TimelineSplitLink { std::string linkGroupId, rightLinkGroupId; };

// Splits selected clips at one sequence tick. Right-hand clip and link IDs are
// explicitly provided by the caller. A link group is split only when all its
// members are selected. No project, source, or history state is mutated.
// Dynamic effects, loops, fractional source ticks and time maps without an
// existing boundary are preserved through the original content window.
class IISHAREDCANVAS_EXPORT TimelineSplitPlan final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
        const std::vector<TimelineSplitTarget> &, const std::vector<TimelineSplitLink> &,
        std::int64_t cutTicks) const;
};

struct TimelineClipRangeTarget { std::string clipId; TimelineTickRange range; };
enum class TimelineClipEdge { Start, End };

// Trims explicitly identified clips to contained, nonempty ranges. Linked
// members must be included with equal edge displacements. Exact source ticks,
// track locks, effects, markers and transition references are validated.
class IISHAREDCANVAS_EXPORT TimelineTrimPlan final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
        const std::vector<TimelineClipRangeTarget> &) const;
};

// Extends one edge of each selected clip to the caller's playhead. Existing
// source handles and newly occupied track intervals are checked before return.
class IISHAREDCANVAS_EXPORT TimelineExtendToPlayheadPlan final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
        const std::vector<std::string> &clipIds, TimelineClipEdge, std::int64_t playheadTicks) const;
};

// Closes a common unoccupied range on the requested tracks, plus sync-locked
// and downstream linked tracks. All linked members must remain synchronized;
// occupied ranges, required locked tracks or ambiguous automation reject the
// complete plan. Sequence duration and sequence-level markers are unchanged.
class IISHAREDCANVAS_EXPORT TimelineGapClosePlan final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
        const std::vector<std::string> &trackIds, TimelineTickRange gap) const;
};

struct TimelineTrackEffectTimeResult {
    std::optional<TimelineRational> sourceTime;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Maps a current track-local rational time to the untouched original effect
// curve domain. A successful empty result denotes a removed time interval.
// Inputs are borrowed for the call, results own their values, and no state or
// external resources are accessed. Invalid/over-budget rationals fail.
class IISHAREDCANVAS_EXPORT TrackEffectTimeMapper final {
public:
    [[nodiscard]] TimelineTrackEffectTimeResult operator()(const TimelineTrackProperties &,TimelineRational currentTime) const;
};

struct SequenceSimplificationOptions {
    bool removeEmptyTracks = false;
    bool removeDisabledClips = false;
    bool removeGaps = false;
    bool flattenNestedSequences = false;
    std::optional<std::string> outputSequenceId;
};

// Each cleanup is opt-in. A new sequence identity preserves the original;
// otherwise the chosen sequence is edited in the returned detached project.
// Neutral nested tracks can be expanded without changing composition. When a
// track cannot be expanded losslessly, the caller may use the separate
// SequenceRasterFlatten preparation/render/publication workflow instead.
// Locked tracks and ambiguous relationships never cause partial edits.
class IISHAREDCANVAS_EXPORT SequenceSimplificationPlan final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
                                                  const SequenceSimplificationOptions &) const;
};

// Times are offsets from the clip's start. Center coordinates use source
// pixels and scale is source width divided by the crop width.
struct AutoReframeKeyframe {
    std::int64_t timeTicks = 0;
    TimelineVector2 center;
    double scale = 1.0;
};
// A detector/tracker supplied by the application owns subject identity and
// bounding boxes. Values are detached; no detector, model or frame is retained.
struct AutoReframeObservation {
    std::int64_t timeTicks = 0;
    std::string trackId;
    TimelineVector2 origin, extent;
};
// One result of a caller-injected detector. Labels and class IDs are supplied
// by that detector; the SDK does not download or choose a model.
struct AutoReframeDetection {
    std::int64_t timeTicks = 0;
    std::int32_t classId = -1;
    std::string label;
    double confidence = 0.0;
    TimelineVector2 origin, extent;
};
struct AutoReframeTrackingOptions {
    std::string idPrefix;
    std::int64_t maximumGapTicks = 0;
    double minimumConfidence = 0.0, minimumOverlap = 0.25;
    std::size_t maximumDetections = 100000, maximumTracks = 4096;
};
struct AutoReframeSubjectTrack {
    std::string id;
    std::int32_t classId = -1;
    std::string label;
    std::vector<AutoReframeObservation> observations;
};
struct AutoReframeTrackingResult {
    std::vector<AutoReframeSubjectTrack> tracks;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Associates caller-provided detections by class and maximum overlap, with a
// bounded temporal gap. A detector is invoked externally and injected through
// its results, so this object cannot select a model or access frames itself.
// The caller chooses an output track and supplies its observations to the
// separate keyframe applier. Invalid input returns no partial associations.
class IISHAREDCANVAS_EXPORT AutoReframeSubjectTracker final {
public:
    [[nodiscard]] AutoReframeTrackingResult operator()(std::span<const AutoReframeDetection>,
                                                       const AutoReframeTrackingOptions &) const;
};

// The controller supplies each frame by read-only pointer and retains its
// lifetime for this call. The tick is the frame's position within the clip.
template<class Frame> struct AutoReframeAnalysisFrame {
    std::int64_t timeTicks = 0;
    const Frame *frame = nullptr;
};

// External orchestration for an explicitly injected detector callable. The
// callable receives one borrowed frame and returns expected<detections,string>;
// timestamps are assigned by this controller, not inferred from a clock. Model
// construction, decoding, ownership, and concurrency remain with the caller.
// No partial tracks escape a detector error or a configured detection limit.
template<class Frame, class Detector>
[[nodiscard]] AutoReframeTrackingResult analyzeAutoReframeFrames(
    std::span<const AutoReframeAnalysisFrame<Frame>> frames, Detector &&detector,
    const AutoReframeTrackingOptions &options) {
    if (frames.size() > options.maximumDetections) return {{}, "analysis frame budget exceeded"};
    try {
        auto policy = AutoReframeSubjectTracker{}(std::span<const AutoReframeDetection>{}, options);
        if (!policy.ok()) return policy;
        std::vector<AutoReframeDetection> detections;
        for (const auto &frame : frames) {
            if (!frame.frame || frame.timeTicks < 0) return {{}, "invalid analysis frame"};
            std::expected<std::vector<AutoReframeDetection>, std::string> found = detector(*frame.frame);
            if (!found) return {{}, found.error().empty() ? "detector failed" : found.error()};
            if (found->size() > options.maximumDetections - detections.size())
                return {{}, "detection budget exceeded"};
            for (auto &item : *found) {
                item.timeTicks = frame.timeTicks;
                detections.push_back(std::move(item));
            }
        }
        return AutoReframeSubjectTracker{}(detections, options);
    } catch (const std::exception &error) {
        const std::string message = error.what();
        return {{}, message.empty() ? "detector failed" : message};
    }
      catch (...) { return {{}, "detector failed"}; }
}
struct AutoReframeRequest {
    std::string effectId;
    std::vector<AutoReframeKeyframe> keyframes;
    std::vector<AutoReframeObservation> observations;
    std::string selectedTrackId;
    TimelineExtent sourceExtent, targetExtent;
    double paddingFraction = 0.0;
};

// Applies explicitly supplied keyframes or converts boxes already associated
// by a caller-controlled detector/tracker. Reframe curves live in one ordinary
// editable timeline effect. This object never opens media or invokes a model;
// model selection, subject tracking and execution remain external stages.
class IISHAREDCANVAS_EXPORT AutoReframeKeyframeApplier final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
        const std::string &clipId, const AutoReframeRequest &) const;
};

// Lift removes the interval and keeps its time gap. Extract additionally
// closes the gap on selected, sync-locked and linked tracks. idPrefix is an
// explicit namespace for any clip and link identities created at boundaries.
// Locked required tracks and relationships that cannot be preserved fail
// atomically; no original content or history is mutated by planning.
class IISHAREDCANVAS_EXPORT TimelineLiftPlan final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
        const std::vector<std::string> &trackIds, TimelineTickRange, const std::string &idPrefix) const;
};
class IISHAREDCANVAS_EXPORT TimelineExtractPlan final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
        const std::vector<std::string> &trackIds, TimelineTickRange, const std::string &idPrefix) const;
};

enum class SubsequenceMode { Copy, Move };
// Copies a selected interval to a new sequence with local time beginning at
// zero. Move additionally extracts that interval from the source sequence.
// Both the destination ID and boundary-ID namespace are supplied explicitly.
class IISHAREDCANVAS_EXPORT SubsequenceExtractor final {
public:
    [[nodiscard]] TimelineProjectPlan operator()(const TimelineProject &, const std::string &sequenceId,
        TimelineTickRange, const std::string &newSequenceId, SubsequenceMode,
        const std::string &idPrefix) const;
};

} // namespace iiSharedCanvas
