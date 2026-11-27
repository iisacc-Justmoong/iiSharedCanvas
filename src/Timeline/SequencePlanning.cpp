#include "Timeline/SequencePlanning.h"
#include "Timeline/ExactBezier_p.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace iiSharedCanvas {
namespace {
TimelineProjectPlan failed(std::string message) { return {{}, std::move(message)}; }

TimelineProjectPlan checked(TimelineProject value) {
    auto validation = validateTimelineProject(value);
    if (!validation.ok()) return failed(validation.issues.front().path + ": " + validation.issues.front().message);
    return {std::move(value), {}};
}

bool safeRange(TimelineTickRange range, std::int64_t first, std::int64_t end) {
    return range.duration > 0 && range.start >= first && range.start <= end &&
           range.duration <= end - range.start;
}

bool onlyUnlock(const TimelineTrackChanges &value) {
    return value.locked && !*value.locked && !value.enabled && !value.muted && !value.solo &&
           !value.syncLocked && !value.role && !value.language && !value.blendMode &&
           !value.outputBusId && !value.opacity;
}

bool staticEffects(const std::vector<TimelineEffect> &effects) {
    return std::all_of(effects.begin(), effects.end(), [](const auto &effect) {
        return !effect.activeRange && effect.automation.empty();
    });
}

TimelineTrackEffectTimeMap effectMap(const TimelineTrackProperties &track,std::int64_t duration) {
    if(track.effectTimeMap)return *track.effectTimeMap;
    return {duration,{{{0,duration},0}}};
}
// Preserve original effect time while translating a surviving output slice.
// Adjacent equivalent pieces are coalesced to bound repeated ripple edits.
void appendEffectSlice(TimelineTrackEffectTimeMap &output,const TimelineTrackEffectTimeSegment &segment,
                       std::int64_t low,std::int64_t high,std::int64_t shift) {
    const auto begin=std::max(low,segment.outputRange.start);
    const auto end=std::min(high,segment.outputRange.start+segment.outputRange.duration);
    if(begin>=end)return;
    TimelineTrackEffectTimeSegment part{{begin-shift,end-begin},segment.sourceStartTicks+(begin-segment.outputRange.start)};
    if(!output.segments.empty()){
        auto &last=output.segments.back();
        if(last.outputRange.start+last.outputRange.duration==part.outputRange.start &&
           last.sourceStartTicks+last.outputRange.duration==part.sourceStartTicks){
            last.outputRange.duration+=part.outputRange.duration;return;
        }
    }
    output.segments.push_back(part);
}
TimelineTrackEffectTimeMap removeEffectInterval(const TimelineTrackProperties &track,std::int64_t duration,
                                                 std::int64_t begin,std::int64_t end) {
    const auto input=effectMap(track,duration);
    TimelineTrackEffectTimeMap output{input.sourceDurationTicks,{}};
    for(const auto &segment:input.segments){
        appendEffectSlice(output,segment,0,begin,0);
        appendEffectSlice(output,segment,end,duration,end-begin);
    }
    return output;
}
TimelineTrackEffectTimeMap sliceEffectInterval(const TimelineTrackProperties &track,std::int64_t duration,
                                                std::int64_t begin,std::int64_t end) {
    const auto input=effectMap(track,duration);
    TimelineTrackEffectTimeMap output{input.sourceDurationTicks,{}};
    for(const auto &segment:input.segments)appendEffectSlice(output,segment,begin,end,begin);
    return output;
}

bool continuousProperties(const TimelineClipProperties &first, const TimelineClipProperties &second) {
    if (!first.enabled || !second.enabled || first.source != second.source ||
        first.loopMode != TimelineLoopMode::None || second.loopMode != TimelineLoopMode::None ||
        !first.timeMap.empty() || !second.timeMap.empty() || first.playbackRate != second.playbackRate ||
        first.videoSampling != second.videoSampling || first.audioStretchMode != second.audioStretchMode ||
        first.preservePitch != second.preservePitch || first.effects != second.effects || !staticEffects(first.effects) ||
        first.timelineRange.start + first.timelineRange.duration != second.timelineRange.start)
        return false;
    return first.playbackRate.numerator > 0
        ? first.sourceRange.start + first.sourceRange.duration == second.sourceRange.start
        : second.sourceRange.start + second.sourceRange.duration == first.sourceRange.start;
}

std::optional<std::uint64_t> requiredSourceTicks(std::int64_t duration, TimelineTimeBase sequence,
                                                  TimelineTimeBase source, TimelineRational rate,
                                                  bool roundUp = true) {
    if (duration < 0 || !sequence.numerator || !sequence.denominator || !source.numerator ||
        !source.denominator || !rate.numerator || !rate.denominator) return {};
    const auto magnitude = rate.numerator < 0
        ? std::uint64_t(-(rate.numerator + 1)) + 1 : std::uint64_t(rate.numerator);
    std::array<std::uint64_t, 4> numerators{std::uint64_t(duration), sequence.numerator, source.denominator, magnitude};
    std::array<std::uint64_t, 3> denominators{sequence.denominator, source.numerator, rate.denominator};
    for (auto &a : numerators) for (auto &b : denominators) {
        const auto divisor = std::gcd(a, b);
        a /= divisor; b /= divisor;
    }
    std::uint64_t numerator = 1, denominator = 1;
    const auto multiply = [](std::uint64_t &accumulator, std::uint64_t factor) {
        if (factor && accumulator > std::numeric_limits<std::uint64_t>::max() / factor) return false;
        accumulator *= factor;
        return true;
    };
    for (auto factor : numerators) if (!multiply(numerator, factor)) return {};
    for (auto factor : denominators) if (!multiply(denominator, factor)) return {};
    const auto quotient = numerator / denominator;
    if (!roundUp && numerator % denominator) return {};
    if (numerator % denominator && quotient == std::numeric_limits<std::uint64_t>::max()) return {};
    return quotient + (numerator % denominator != 0);
}

struct HandleSource {
    TimelineTimeBase timeBase;
    std::int64_t start = 0, duration = 0;
    bool unbounded = false;
};

std::optional<HandleSource> handleSource(const TimelineProject &project, const TimelineClipProperties &clip) {
    return std::visit([&](const auto &reference) -> std::optional<HandleSource> {
        using Reference = std::decay_t<decltype(reference)>;
        if constexpr (std::is_same_v<Reference, TimelineGeneratedReference>) {
            return HandleSource{reference.timeBase, 0, 0, true};
        } else if constexpr (std::is_same_v<Reference, TimelineSequenceReference>) {
            const auto *sequence = findTimelineSequence(project, reference.sequenceId);
            if (!sequence) return {};
            return HandleSource{sequence->timeBase, sequence->startTicks, sequence->durationTicks, false};
        } else {
            const auto *media = findTimelineMediaSource(project, reference.mediaSourceId);
            const auto *stream = media ? findTimelineMediaStream(*media, reference.streamId) : nullptr;
            if (!stream) return {};
            return std::visit([](const auto &value) {
                return HandleSource{value.timeBase, value.startTicks, value.durationTicks, false};
            }, *stream);
        }
    }, clip.source);
}

bool hasHandle(const TimelineProject &project, const TimelineSequence &sequence,
               const TimelineClipProperties &clip, std::int64_t duration, bool afterClip) {
    if (!duration) return true;
    if (clip.loopMode != TimelineLoopMode::None && clip.timeMap.empty()) return true;
    if (clip.contentWindow) {
        const auto &window=*clip.contentWindow;
        const auto available=afterClip ? window.durationTicks-window.offsetTicks-clip.timelineRange.duration : window.offsetTicks;
        return available>=0 && duration<=available;
    }
    if (!clip.timeMap.empty()) return false;
    const auto source = handleSource(project, clip);
    if (!source) return false;
    if (source->unbounded) return true;
    if (source->duration <= 0) return false;
    const auto required = requiredSourceTicks(duration, sequence.timeBase, source->timeBase, clip.playbackRate);
    if (!required) return false;
    const auto sourceEnd = source->start + source->duration;
    const auto clipEnd = clip.sourceRange.start + clip.sourceRange.duration;
    const bool towardsEnd = afterClip == (clip.playbackRate.numerator > 0);
    const auto available = towardsEnd ? sourceEnd - clipEnd : clip.sourceRange.start - source->start;
    return available >= 0 && *required <= static_cast<std::uint64_t>(available);
}

enum class RangeEdit { Trim, ExtendStart, ExtendEnd };

TimelineProjectPlan rangePlan(const TimelineProject &input, const std::string &sequenceId,
                             const std::vector<TimelineClipRangeTarget> &targets, RangeEdit mode) {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    TimelineProject result = input;
    auto *sequence = findTimelineSequence(result, sequenceId);
    if (!sequence) return failed("unknown sequence");
    if (targets.empty()) return failed("at least one clip is required");
    std::unordered_map<std::string, TimelineTickRange> ranges;
    std::unordered_map<std::string, TimelineTickRange> originals;
    const auto sequenceEnd = sequence->startTicks + sequence->durationTicks;
    for (const auto &target : targets) {
        if (!ranges.emplace(target.clipId, target.range).second) return failed("duplicate clip identity");
        const auto clip = findTimelineClip(*sequence, target.clipId);
        if (!clip) return failed("unknown clip");
        const auto old = timelineClipProperties(clip).timelineRange;
        if (!safeRange(target.range, sequence->startTicks, sequenceEnd)) return failed("invalid clip range");
        const auto oldEnd = old.start + old.duration, newEnd = target.range.start + target.range.duration;
        if ((mode == RangeEdit::Trim && (target.range.start < old.start || newEnd > oldEnd)) ||
            (mode == RangeEdit::ExtendStart && (target.range.start > old.start || newEnd != oldEnd)) ||
            (mode == RangeEdit::ExtendEnd && (target.range.start != old.start || newEnd < oldEnd)))
            return failed("requested range is not a valid edge edit");
        originals.emplace(target.clipId, old);
    }
    for (const auto &group : sequence->linkGroups) {
        const bool affected = std::any_of(group.clipIds.begin(), group.clipIds.end(),
            [&](const auto &id) { return ranges.contains(id); });
        if (!affected) continue;
        if (!std::all_of(group.clipIds.begin(), group.clipIds.end(),
                [&](const auto &id) { return ranges.contains(id); }))
            return failed("all linked clips must be edited together");
        std::optional<std::pair<std::int64_t, std::int64_t>> displacement;
        for (const auto &id : group.clipIds) {
            const auto old = originals.at(id), next = ranges.at(id);
            const auto change = std::pair{next.start - old.start,
                (next.start + next.duration) - (old.start + old.duration)};
            if (displacement && *displacement != change) return failed("linked clip edge displacements must match");
            displacement = change;
        }
    }
    std::string error;
    for (auto &track : sequence->tracks) {
        std::visit([&](auto &typed) {
            using Track = std::decay_t<decltype(typed)>;
            for (auto &clip : typed.clips) {
                const auto target = ranges.find(clip.properties.id);
                if (target == ranges.end()) continue;
                if (typed.properties.locked) { error = "selected track is locked"; return; }
                auto &properties = clip.properties;
                const auto old = properties.timelineRange, next = target->second;
                if(old==next)continue;
                const auto oldEnd = old.start + old.duration, nextEnd = next.start + next.duration;
                const auto removedStart = mode == RangeEdit::Trim ? next.start - old.start : 0;
                const auto removedEnd = mode == RangeEdit::Trim ? oldEnd - nextEnd : 0;
                const auto addedStart = mode == RangeEdit::ExtendStart ? old.start - next.start : 0;
                const auto addedEnd = mode == RangeEdit::ExtendEnd ? nextEnd - oldEnd : 0;
                const bool unalignedMap=mode==RangeEdit::Trim && !properties.timeMap.empty() &&
                    (std::none_of(properties.timeMap.begin(),properties.timeMap.end(),[&](const auto &key){return key.timelineTicks==removedStart;}) ||
                     std::none_of(properties.timeMap.begin(),properties.timeMap.end(),[&](const auto &key){return key.timelineTicks==old.duration-removedEnd;}));
                bool preserve=properties.contentWindow.has_value() || !staticEffects(properties.effects) ||
                              properties.loopMode!=TimelineLoopMode::None || unalignedMap;
                if constexpr (std::is_same_v<Track, TimelineAudioTrack>)
                    preserve |= (removedStart && removedStart<clip.mix.fadeInTicks) ||
                                (removedEnd && removedEnd<clip.mix.fadeOutTicks);
                if(mode==RangeEdit::Trim)for(const auto &marker:properties.markers)
                    preserve |= marker.range.start<removedStart || marker.range.start+marker.range.duration>old.duration-removedEnd;
                if(preserve){
                    auto window=properties.contentWindow.value_or(TimelineClipContentWindow{0,old.duration});
                    if(addedStart && window.offsetTicks<std::numeric_limits<std::int64_t>::min()+addedStart){
                        error="content-window offset overflows";return;
                    }
                    const auto shifted=window.offsetTicks-addedStart;
                    if(removedStart && shifted>std::numeric_limits<std::int64_t>::max()-removedStart){
                        error="content-window offset overflows";return;
                    }
                    const auto nextOffset=shifted+removedStart;
                    const bool unbounded=properties.loopMode!=TimelineLoopMode::None && properties.timeMap.empty();
                    const bool outside=!unbounded && (nextOffset<0 || nextOffset>window.durationTicks ||
                        (nextOffset>=0 && nextOffset<=window.durationTicks && next.duration>window.durationTicks-nextOffset));
                    if(outside || nextOffset>std::numeric_limits<std::int64_t>::max()-next.duration){
                        error="requested extension lies outside the original content window";return;
                    }
                    window.offsetTicks=nextOffset;
                    properties.contentWindow=window;
                    properties.timelineRange=next;
                    continue;
                }
                if constexpr (std::is_same_v<Track, TimelineAudioTrack>) {
                    if ((removedStart && removedStart < clip.mix.fadeInTicks) ||
                        (removedEnd && removedEnd < clip.mix.fadeOutTicks) ||
                        (addedStart && clip.mix.fadeInTicks) || (addedEnd && clip.mix.fadeOutTicks)) {
                        error = "audio fade crosses the edited edge"; return;
                    }
                    if (removedStart) clip.mix.fadeInTicks = 0;
                    if (removedEnd) clip.mix.fadeOutTicks = 0;
                }
                for (auto &marker : properties.markers) {
                    if (mode == RangeEdit::Trim) {
                        if (marker.range.start < removedStart ||
                            marker.range.start + marker.range.duration > old.duration - removedEnd) {
                            error = "trim would remove a clip marker"; return;
                        }
                        marker.range.start -= removedStart;
                    } else marker.range.start += addedStart;
                }
                if (!properties.timeMap.empty()) {
                    if (mode != RangeEdit::Trim) { error = "time maps cannot be extrapolated"; return; }
                    const auto begin = std::find_if(properties.timeMap.begin(), properties.timeMap.end(),
                        [&](const auto &key) { return key.timelineTicks == removedStart; });
                    const auto end = std::find_if(properties.timeMap.begin(), properties.timeMap.end(),
                        [&](const auto &key) { return key.timelineTicks == old.duration - removedEnd; });
                    if (begin == properties.timeMap.end() || end == properties.timeMap.end() || begin >= end) {
                        error = "time map has no exact trim boundaries"; return;
                    }
                    std::vector<TimelineTimeMapPoint> points(begin, end + 1);
                    for (auto &point : points) point.timelineTicks -= removedStart;
                    properties.timeMap = std::move(points);
                } else {
                    const auto source = handleSource(input, properties);
                    if (!source) { error = "unknown clip source"; return; }
                    const auto left = requiredSourceTicks(removedStart + addedStart, sequence->timeBase,
                                                           source->timeBase, properties.playbackRate, false);
                    const auto right = requiredSourceTicks(removedEnd + addedEnd, sequence->timeBase,
                                                            source->timeBase, properties.playbackRate, false);
                    if (!left || !right || *left > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
                        *right > std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
                        error = "range edge does not fall on an exact source tick"; return;
                    }
                    const auto l = static_cast<std::int64_t>(*left), r = static_cast<std::int64_t>(*right);
                    auto &sourceRange = properties.sourceRange;
                    const bool forward = properties.playbackRate.numerator > 0;
                    if (mode == RangeEdit::Trim) {
                        if (l >= sourceRange.duration || r >= sourceRange.duration - l) {
                            error = "trim leaves no source duration"; return;
                        }
                        sourceRange.start += forward ? l : r;
                        sourceRange.duration -= l + r;
                    } else {
                        if (!hasHandle(input, *sequence, properties, addedStart, false) ||
                            !hasHandle(input, *sequence, properties, addedEnd, true) ||
                            l > std::numeric_limits<std::int64_t>::max() - sourceRange.duration ||
                            r > std::numeric_limits<std::int64_t>::max() - sourceRange.duration - l) {
                            error = "insufficient or unrepresentable source handles"; return;
                        }
                        const auto before = forward ? l : r;
                        if (sourceRange.start < std::numeric_limits<std::int64_t>::min() + before) {
                            error = "extended source range overflows"; return;
                        }
                        sourceRange.start -= before;
                        sourceRange.duration += l + r;
                        if (sourceRange.start > std::numeric_limits<std::int64_t>::max() - sourceRange.duration) {
                            error = "extended source range overflows"; return;
                        }
                    }
                }
                if (mode != RangeEdit::Trim) for (const auto &other : typed.clips) {
                    if (other.properties.id == properties.id) continue;
                    const auto otherRange = other.properties.timelineRange;
                    const auto otherEnd = otherRange.start + otherRange.duration;
                    if ((addedStart && otherRange.start < old.start && otherEnd > next.start) ||
                        (addedEnd && otherRange.start < nextEnd && otherEnd > oldEnd)) {
                        error = "extension would overlap another clip"; return;
                    }
                }
                properties.timelineRange = next;
            }
        }, track);
        if (!error.empty()) return failed(std::move(error));
    }
    return checked(std::move(result));
}
} // namespace

TimelineProjectPlan SequenceSettingsEditor::operator()(const TimelineProject &input,
    const std::string &sequenceId, const SequenceSettingsChange &changes) const {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    TimelineProject result = input;
    auto *sequence = findTimelineSequence(result, sequenceId);
    if (!sequence) return failed("unknown sequence");
    if (changes.name) sequence->name = *changes.name;
    if (changes.editingFrameRate) sequence->editingFrameRate = *changes.editingFrameRate;
    if (changes.canvasExtent) sequence->canvasExtent = *changes.canvasExtent;
    if (changes.pixelAspectRatio) sequence->pixelAspectRatio = *changes.pixelAspectRatio;
    if (changes.audioSampleRate) sequence->audioSampleRate = *changes.audioSampleRate;
    if (changes.audioChannelLayout) sequence->audioChannelLayout = *changes.audioChannelLayout;
    if (changes.timecode) sequence->timecode = *changes.timecode;
    return checked(std::move(result));
}

TimelineGapIndexResult TimelineGapIndex::operator()(const TimelineSequence &sequence,
    const std::vector<std::string> &trackIds) const {
    if (sequence.durationTicks < 0 ||
        sequence.startTicks > std::numeric_limits<std::int64_t>::max() - sequence.durationTicks)
        return {{}, "invalid sequence range"};
    const auto end = sequence.startTicks + sequence.durationTicks;
    std::unordered_set<std::string> seen;
    TimelineGapIndexResult result;
    for (const auto &id : trackIds) {
        if (!seen.insert(id).second) return {{}, "duplicate track identity"};
        if (std::count_if(sequence.tracks.begin(), sequence.tracks.end(), [&](const auto &item) {
                return timelineTrackProperties(item).id == id;
            }) > 1) return {{}, "ambiguous track identity"};
        const auto *track = findTimelineTrack(sequence, id);
        if (!track) return {{}, "unknown track"};
        std::vector<TimelineTickRange> occupied;
        bool invalid = false;
        std::visit([&](const auto &typed) {
            occupied.reserve(typed.clips.size());
            for (const auto &clip : typed.clips) {
                if (!safeRange(clip.properties.timelineRange, sequence.startTicks, end)) {
                    invalid = true;
                    return;
                }
                occupied.push_back(clip.properties.timelineRange);
            }
        }, *track);
        if (invalid) return {{}, "clip range lies outside the sequence"};
        std::sort(occupied.begin(), occupied.end(), [](auto a, auto b) { return a.start < b.start; });
        auto cursor = sequence.startTicks;
        for (const auto range : occupied) {
            if (range.start > cursor) result.gaps.push_back({id, {cursor, range.start - cursor}});
            cursor = std::max(cursor, range.start + range.duration);
        }
        if (cursor < end) result.gaps.push_back({id, {cursor, end - cursor}});
    }
    return result;
}

TimelineProjectPlan TimelineTrackBatchEdit::operator()(const TimelineProject &input,
    const std::string &sequenceId, const std::vector<std::string> &trackIds,
    const TimelineTrackChanges &changes) const {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    TimelineProject result = input;
    auto *sequence = findTimelineSequence(result, sequenceId);
    if (!sequence) return failed("unknown sequence");
    std::unordered_set<std::string> seen;
    for (const auto &id : trackIds) {
        if (!seen.insert(id).second) return failed("duplicate track identity");
        auto *track = findTimelineTrack(*sequence, id);
        if (!track) return failed("unknown track");
        auto &properties = timelineTrackProperties(*track);
        if (properties.locked && !onlyUnlock(changes)) return failed("selected track is locked");
        if (changes.enabled) properties.enabled = *changes.enabled;
        if (changes.locked) properties.locked = *changes.locked;
        if (changes.muted) properties.muted = *changes.muted;
        if (changes.solo) properties.solo = *changes.solo;
        if (changes.syncLocked) properties.syncLocked = *changes.syncLocked;
        if (changes.role) properties.role = *changes.role;
        if (changes.language) properties.language = *changes.language;
        if (changes.blendMode) properties.blendMode = *changes.blendMode;
        if (changes.outputBusId) properties.outputBusId = *changes.outputBusId;
        if (changes.opacity) properties.opacity = *changes.opacity;
    }
    return checked(std::move(result));
}

TimelineProjectPlan CaptionTrackEditor::operator()(const TimelineProject &input,
    const std::string &sequenceId, const std::string &trackId, const CaptionTrackChange &change) const {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    TimelineProject result = input;
    auto *sequence = findTimelineSequence(result, sequenceId);
    if (!sequence) return failed("unknown sequence");
    auto *entry = findTimelineTrack(*sequence, trackId);
    auto *track = entry ? std::get_if<TimelineSubtitleTrack>(entry) : nullptr;
    if (!track) return failed("unknown caption track");
    if (track->properties.locked) return failed("caption track is locked");
    if (change.language) track->properties.language = *change.language;
    switch (change.action) {
    case CaptionTrackAction::SetLanguage:
        if (!change.language) return failed("caption language is required");
        break;
    case CaptionTrackAction::Upsert: {
        if (change.clip.properties.id.empty()) return failed("caption clip identity is required");
        auto current = std::find_if(track->clips.begin(), track->clips.end(), [&](const auto &clip) {
            return clip.properties.id == change.clip.properties.id;
        });
        if (current == track->clips.end()) track->clips.push_back(change.clip);
        else *current = change.clip;
        break;
    }
    case CaptionTrackAction::Remove: {
        auto current = std::find_if(track->clips.begin(), track->clips.end(), [&](const auto &clip) {
            return clip.properties.id == change.clipId;
        });
        if (current == track->clips.end()) return failed("unknown caption clip");
        if (!current->properties.linkGroupId.empty() ||
            std::any_of(track->transitions.begin(), track->transitions.end(), [&](const auto &transition) {
                return transition.fromClipId == change.clipId || transition.toClipId == change.clipId;
            })) return failed("caption clip has dependent links or transitions");
        track->clips.erase(current);
        break;
    }
    default: return failed("unknown caption operation");
    }
    return checked(std::move(result));
}

ThroughEditResult ThroughEditDetector::operator()(const TimelineProject &project,
    const std::string &sequenceId, const std::vector<std::string> &trackIds) const {
    if (!validateTimelineProject(project).ok()) return {{}, "invalid input project"};
    const auto *sequence = findTimelineSequence(project, sequenceId);
    if (!sequence) return {{}, "unknown sequence"};
    ThroughEditResult result;
    std::unordered_set<std::string> seen;
    for (const auto &id : trackIds) {
        if (!seen.insert(id).second) return {{}, "duplicate track identity"};
        const auto *track = findTimelineTrack(*sequence, id);
        if (!track) return {{}, "unknown track"};
        std::visit([&](const auto &typed) {
            using Track = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Track, TimelineVideoTrack> || std::is_same_v<Track, TimelineAudioTrack>) {
                using Clip = typename std::decay_t<decltype(typed.clips)>::value_type;
                std::vector<const Clip *> clips;
                clips.reserve(typed.clips.size());
                for (const auto &clip : typed.clips) clips.push_back(&clip);
                std::stable_sort(clips.begin(), clips.end(), [](const auto *a, const auto *b) {
                    return a->properties.timelineRange.start < b->properties.timelineRange.start;
                });
                for (std::size_t i = 1; i < clips.size(); ++i) {
                    const auto &a = *clips[i - 1];
                    const auto &b = *clips[i];
                    if (!continuousProperties(a.properties, b.properties)) continue;
                    if constexpr (std::is_same_v<Track, TimelineVideoTrack>) {
                        if (a.transform != b.transform || a.crop != b.crop) continue;
                    } else {
                        if (a.mix != b.mix || a.mix.fadeOutTicks || b.mix.fadeInTicks) continue;
                    }
                    const bool transition = std::any_of(typed.transitions.begin(), typed.transitions.end(),
                        [&](const auto &value) { return value.enabled &&
                            (value.fromClipId == a.properties.id || value.toClipId == a.properties.id ||
                             value.fromClipId == b.properties.id || value.toClipId == b.properties.id); });
                    if (!transition) result.edits.push_back({id, a.properties.id, b.properties.id,
                                                              b.properties.timelineRange.start});
                }
            }
        }, *track);
    }
    return result;
}

DefaultTransitionResult DefaultTransitionPolicy::operator()(TimelineStreamKind kind,
    std::int64_t durationTicks, std::string typeOverride) const {
    if (durationTicks <= 0) return {{}, 0, "transition duration must be positive"};
    if (kind != TimelineStreamKind::Video && kind != TimelineStreamKind::Audio)
        return {{}, 0, "default transitions require video or audio"};
    if (typeOverride.empty()) typeOverride = kind == TimelineStreamKind::Video
        ? "builtin.cross-dissolve" : "builtin.equal-power-fade";
    return {std::move(typeOverride), durationTicks, {}};
}

TimelineProjectPlan TimelineTransitionPlacement::operator()(const TimelineProject &input,
    const std::string &sequenceId, const std::string &trackId, const TimelineTransition &transition) const {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    TimelineProject result = input;
    auto *sequence = findTimelineSequence(result, sequenceId);
    auto *track = sequence ? findTimelineTrack(*sequence, trackId) : nullptr;
    if (!track) return failed("unknown sequence or track");
    if (timelineTrackProperties(*track).locked) return failed("track is locked");
    const auto sequenceEnd = sequence->startTicks + sequence->durationTicks;
    if (!safeRange(transition.timelineRange, sequence->startTicks, sequenceEnd))
        return failed("transition range lies outside the sequence");
    std::string error;
    std::visit([&](auto &typed) {
        const auto clip = [&](const std::string &id) -> const TimelineClipProperties * {
            if (id.empty()) return nullptr;
            const auto found = std::find_if(typed.clips.begin(), typed.clips.end(), [&](const auto &item) {
                return item.properties.id == id;
            });
            return found == typed.clips.end() ? nullptr : &found->properties;
        };
        const auto *from = clip(transition.fromClipId), *to = clip(transition.toClipId);
        if ((!transition.fromClipId.empty() && !from) || (!transition.toClipId.empty() && !to) || (!from && !to)) {
            error = "unknown transition endpoint"; return;
        }
        const auto end = transition.timelineRange.start + transition.timelineRange.duration;
        for (const auto &existing : typed.transitions) {
            const auto existingEnd = existing.timelineRange.start + existing.timelineRange.duration;
            if (existing.id == transition.id || (transition.timelineRange.start < existingEnd && existing.timelineRange.start < end)) {
                error = "transition identity or range conflicts with an existing transition"; return;
            }
        }
        if (from && to) {
            const auto cut = from->timelineRange.start + from->timelineRange.duration;
            if (cut != to->timelineRange.start || transition.timelineRange.start > cut || end < cut ||
                !hasHandle(input, *sequence, *from, end - cut, true) ||
                !hasHandle(input, *sequence, *to, cut - transition.timelineRange.start, false)) {
                error = "transition endpoints or source handles are invalid"; return;
            }
        }
        typed.transitions.push_back(transition);
    }, *track);
    if (!error.empty()) return failed(std::move(error));
    return checked(std::move(result));
}

namespace {
TimelineProjectPlan splitPlan(const TimelineProject &input, const std::string &sequenceId,
    const std::vector<TimelineSplitTarget> &targets, const std::vector<TimelineSplitLink> &linkTargets,
    std::int64_t cutTicks) {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    TimelineProject result = input;
    auto *sequence = findTimelineSequence(result, sequenceId);
    if (!sequence) return failed("unknown sequence");
    if (targets.empty()) return failed("at least one clip is required");
    std::unordered_map<std::string, std::string> rightIds, rightLinks;
    std::unordered_set<std::string> rightIdSet, foundIds, usedLinks;
    for (const auto &target : targets) {
        if (target.clipId.empty() || target.rightClipId.empty() ||
            !rightIds.emplace(target.clipId, target.rightClipId).second || !rightIdSet.insert(target.rightClipId).second)
            return failed("split clip identities must be nonempty and unique");
    }
    for (const auto &link : linkTargets) {
        if (link.linkGroupId.empty() || link.rightLinkGroupId.empty() ||
            !rightLinks.emplace(link.linkGroupId, link.rightLinkGroupId).second)
            return failed("split link identities must be nonempty and unique");
    }
    for (const auto &track : sequence->tracks) std::visit([&](const auto &typed) {
        for (const auto &clip : typed.clips) {
            if (rightIds.contains(clip.properties.id)) foundIds.insert(clip.properties.id);
            if (rightIdSet.contains(clip.properties.id)) foundIds.insert("");
        }
    }, track);
    if (foundIds.contains("") || foundIds.size() != targets.size())
        return failed("split clip is missing or a new clip identity already exists");
    std::vector<TimelineLinkGroup> newGroups;
    for (const auto &group : sequence->linkGroups) {
        const bool selected = std::any_of(group.clipIds.begin(), group.clipIds.end(),
            [&](const auto &id) { return rightIds.contains(id); });
        if (!selected) continue;
        const auto right = rightLinks.find(group.id);
        if (right == rightLinks.end() || !std::all_of(group.clipIds.begin(), group.clipIds.end(),
                [&](const auto &id) { return rightIds.contains(id); }))
            return failed("all linked clips and a new link identity are required");
        TimelineLinkGroup copy = group;
        copy.id = right->second;
        for (auto &id : copy.clipIds) id = rightIds.at(id);
        newGroups.push_back(std::move(copy));
        usedLinks.insert(group.id);
    }
    if (usedLinks.size() != rightLinks.size()) return failed("unknown or unaffected link identity");

    std::string error;
    for (auto &track : sequence->tracks) {
        std::visit([&](auto &typed) {
            using Track = std::decay_t<decltype(typed)>;
            for (std::size_t i = 0; i < typed.clips.size() && error.empty(); ++i) {
                auto &left = typed.clips[i];
                const auto target = rightIds.find(left.properties.id);
                if (target == rightIds.end()) continue;
                if (typed.properties.locked) { error = "selected track is locked"; break; }
                const auto start = left.properties.timelineRange.start;
                const auto duration = left.properties.timelineRange.duration;
                const auto end = start + duration;
                if (cutTicks <= start || cutTicks >= end) { error = "split must be inside every selected clip"; break; }
                const auto leftDuration = cutTicks - start, rightDuration = end - cutTicks;
                for (const auto &transition : typed.transitions) {
                    const auto transitionEnd = transition.timelineRange.start + transition.timelineRange.duration;
                    if ((transition.fromClipId == left.properties.id || transition.toClipId == left.properties.id) &&
                        transition.timelineRange.start < cutTicks && transitionEnd > cutTicks) {
                        error = "split intersects a transition"; break;
                    }
                }
                if (!error.empty()) break;
                auto right = left;
                bool preserve=left.properties.contentWindow.has_value() || !staticEffects(left.properties.effects) ||
                              left.properties.loopMode!=TimelineLoopMode::None;
                if constexpr (std::is_same_v<Track, TimelineAudioTrack>)
                    preserve |= left.mix.fadeInTicks>leftDuration || left.mix.fadeOutTicks>rightDuration;
                for(const auto &marker:left.properties.markers)
                    preserve |= marker.range.start<leftDuration && marker.range.start+marker.range.duration>leftDuration;
                if(!left.properties.timeMap.empty())
                    preserve |= std::none_of(left.properties.timeMap.begin(),left.properties.timeMap.end(),
                        [&](const auto &key){return key.timelineTicks==leftDuration;});
                else if(!preserve){
                    const auto source=handleSource(input,left.properties);
                    const auto amount=source ? requiredSourceTicks(leftDuration,sequence->timeBase,source->timeBase,
                        left.properties.playbackRate,false) : std::nullopt;
                    preserve=!amount || !*amount || *amount>=std::uint64_t(left.properties.sourceRange.duration);
                }
                if(preserve){
                    auto window=left.properties.contentWindow.value_or(TimelineClipContentWindow{0,duration});
                    left.properties.contentWindow=window;
                    window.offsetTicks+=leftDuration;
                    right.properties.contentWindow=window;
                } else {
                if constexpr (std::is_same_v<Track, TimelineAudioTrack>) {
                    if (left.mix.fadeInTicks > leftDuration || left.mix.fadeOutTicks > rightDuration) {
                        error = "audio fade crosses the split"; break;
                    }
                    left.mix.fadeOutTicks = 0;
                    right.mix.fadeInTicks = 0;
                }
                std::vector<TimelineMarker> leftMarkers, rightMarkers;
                for (auto marker : left.properties.markers) {
                    const auto markerEnd = marker.range.start + marker.range.duration;
                    if (marker.range.start >= leftDuration) {
                        marker.range.start -= leftDuration; rightMarkers.push_back(std::move(marker));
                    } else if (markerEnd <= leftDuration) leftMarkers.push_back(std::move(marker));
                    else { error = "clip marker crosses the split"; break; }
                }
                if (!error.empty()) break;
                left.properties.markers = std::move(leftMarkers);
                right.properties.markers = std::move(rightMarkers);
                if (!left.properties.timeMap.empty()) {
                    const auto point = std::find_if(left.properties.timeMap.begin(), left.properties.timeMap.end(),
                        [&](const auto &key) { return key.timelineTicks == leftDuration; });
                    if (point == left.properties.timeMap.end()) {
                        error = "time map has no exact split boundary"; break;
                    }
                    const auto index = std::size_t(point - left.properties.timeMap.begin());
                    right.properties.timeMap.assign(left.properties.timeMap.begin() + index, left.properties.timeMap.end());
                    for (auto &key : right.properties.timeMap) key.timelineTicks -= leftDuration;
                    left.properties.timeMap.erase(left.properties.timeMap.begin() + index + 1, left.properties.timeMap.end());
                } else {
                    const auto source = handleSource(input, left.properties);
                    const auto amount = source ? requiredSourceTicks(leftDuration, sequence->timeBase,
                        source->timeBase, left.properties.playbackRate, false) : std::nullopt;
                    const auto total = left.properties.sourceRange.duration;
                    if (!amount || !*amount || *amount >= std::uint64_t(total)) {
                        error = "split does not fall on an exact source tick"; break;
                    }
                    const auto sourceLeft = static_cast<std::int64_t>(*amount);
                    if (left.properties.playbackRate.numerator > 0) {
                        right.properties.sourceRange.start += sourceLeft;
                        right.properties.sourceRange.duration -= sourceLeft;
                        left.properties.sourceRange.duration = sourceLeft;
                    } else {
                        left.properties.sourceRange.start += total - sourceLeft;
                        left.properties.sourceRange.duration = sourceLeft;
                        right.properties.sourceRange.duration = total - sourceLeft;
                    }
                }
                }
                const auto originalId = left.properties.id;
                left.properties.timelineRange.duration = leftDuration;
                right.properties.id = target->second;
                right.properties.timelineRange = {cutTicks, rightDuration};
                if (!right.properties.linkGroupId.empty()) right.properties.linkGroupId = rightLinks.at(right.properties.linkGroupId);
                for (auto &transition : typed.transitions)
                    if (transition.fromClipId == originalId) transition.fromClipId = right.properties.id;
                typed.clips.insert(typed.clips.begin() + static_cast<std::ptrdiff_t>(i + 1), std::move(right));
                ++i;
            }
        }, track);
        if (!error.empty()) return failed(std::move(error));
    }
    sequence->linkGroups.insert(sequence->linkGroups.end(),
                                std::make_move_iterator(newGroups.begin()), std::make_move_iterator(newGroups.end()));
    return checked(std::move(result));
}
} // namespace

TimelineProjectPlan TimelineSplitPlan::operator()(const TimelineProject &input, const std::string &sequenceId,
    const std::vector<TimelineSplitTarget> &targets, const std::vector<TimelineSplitLink> &linkTargets,
    std::int64_t cutTicks) const {
    return splitPlan(input, sequenceId, targets, linkTargets, cutTicks);
}

TimelineProjectPlan TimelineTrimPlan::operator()(const TimelineProject &input, const std::string &sequenceId,
    const std::vector<TimelineClipRangeTarget> &targets) const {
    return rangePlan(input, sequenceId, targets, RangeEdit::Trim);
}

TimelineProjectPlan TimelineExtendToPlayheadPlan::operator()(const TimelineProject &input,
    const std::string &sequenceId, const std::vector<std::string> &clipIds, TimelineClipEdge edge,
    std::int64_t playheadTicks) const {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    const auto *sequence = findTimelineSequence(input, sequenceId);
    if (!sequence) return failed("unknown sequence");
    const auto sequenceEnd = sequence->startTicks + sequence->durationTicks;
    if (playheadTicks < sequence->startTicks || playheadTicks > sequenceEnd)
        return failed("playhead lies outside the sequence");
    std::vector<TimelineClipRangeTarget> targets;
    targets.reserve(clipIds.size());
    for (const auto &id : clipIds) {
        const auto clip = findTimelineClip(*sequence, id);
        if (!clip) return failed("unknown clip");
        const auto range = timelineClipProperties(clip).timelineRange;
        if (range.duration <= 0 || range.start > std::numeric_limits<std::int64_t>::max() - range.duration)
            return failed("invalid source clip range");
        const auto end = range.start + range.duration;
        if (edge == TimelineClipEdge::Start) {
            if (playheadTicks > range.start || playheadTicks < sequence->startTicks)
                return failed("playhead does not extend the clip start");
            targets.push_back({id, {playheadTicks, end - playheadTicks}});
        } else if (edge == TimelineClipEdge::End) {
            if (playheadTicks < end || playheadTicks < range.start ||
                playheadTicks - range.start < range.duration)
                return failed("playhead does not extend the clip end");
            targets.push_back({id, {range.start, playheadTicks - range.start}});
        } else return failed("unknown clip edge");
    }
    return rangePlan(input, sequenceId, targets, edge == TimelineClipEdge::Start ? RangeEdit::ExtendStart : RangeEdit::ExtendEnd);
}

namespace {
TimelineProjectPlan gapClosePlan(const TimelineProject &input, const std::string &sequenceId,
    const std::vector<std::string> &trackIds, TimelineTickRange gap) {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    TimelineProject result = input;
    auto *sequence = findTimelineSequence(result, sequenceId);
    if (!sequence) return failed("unknown sequence");
    if (trackIds.empty() || !safeRange(gap, sequence->startTicks, sequence->startTicks + sequence->durationTicks))
        return failed("gap and at least one track are required");
    const auto end = gap.start + gap.duration;
    struct ClipLocation { std::size_t track; TimelineTickRange range; std::string sync; };
    std::unordered_map<std::string, ClipLocation> clips;
    std::unordered_map<std::string, std::vector<std::string>> syncGroups;
    std::unordered_set<std::size_t> affected;
    std::unordered_set<std::string> selected;
    for (std::size_t index = 0; index < sequence->tracks.size(); ++index) {
        const auto &track = sequence->tracks[index];
        if (timelineTrackProperties(track).syncLocked) affected.insert(index);
        std::visit([&](const auto &typed) {
            for (const auto &clip : typed.clips) {
                const auto &p = clip.properties;
                clips.emplace(p.id, ClipLocation{index, p.timelineRange, p.syncGroupId});
                if (!p.syncGroupId.empty()) syncGroups[p.syncGroupId].push_back(p.id);
            }
        }, track);
    }
    for (const auto &id : trackIds) {
        if (!selected.insert(id).second) return failed("duplicate track identity");
        const auto found = std::find_if(sequence->tracks.begin(), sequence->tracks.end(), [&](const auto &track) {
            return timelineTrackProperties(track).id == id;
        });
        if (found == sequence->tracks.end()) return failed("unknown track");
        affected.insert(static_cast<std::size_t>(found - sequence->tracks.begin()));
    }
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (auto index : affected) {
            const auto &track = sequence->tracks[index];
            if (timelineTrackProperties(track).locked) return failed("required track is locked");
            bool occupied = false;
            std::visit([&](const auto &typed) {
                for (const auto &clip : typed.clips) {
                    const auto range = clip.properties.timelineRange;
                    if (range.start < end && range.start + range.duration > gap.start) occupied = true;
                }
            }, track);
            if (occupied) return failed("gap is occupied on a required track");
        }
        const auto includeGroup = [&](const std::vector<std::string> &ids) -> bool {
            const bool moves = std::any_of(ids.begin(), ids.end(), [&](const auto &id) {
                const auto &location = clips.at(id);
                return affected.contains(location.track) && location.range.start >= end;
            });
            if (!moves) return true;
            for (const auto &id : ids) {
                const auto &location = clips.at(id);
                if (location.range.start < end) return false;
                expanded |= affected.insert(location.track).second;
            }
            return true;
        };
        for (const auto &group : sequence->linkGroups)
            if (!includeGroup(group.clipIds)) return failed("linked clips cannot remain synchronized");
        for (const auto &[id, members] : syncGroups) {
            (void)id;
            if (!includeGroup(members)) return failed("sync-group clips cannot remain synchronized");
        }
    }
    const auto gapStartOffset = gap.start - sequence->startTicks;
    const auto gapEndOffset = end - sequence->startTicks;
    for (auto index : affected) {
        auto &track = sequence->tracks[index];
        std::string error;
        std::visit([&](auto &typed) {
            const bool moves = std::any_of(typed.clips.begin(), typed.clips.end(),
                [&](const auto &clip) { return clip.properties.timelineRange.start >= end; });
            if(moves && (!staticEffects(typed.properties.effects) || typed.properties.effectTimeMap))
                typed.properties.effectTimeMap=removeEffectInterval(typed.properties,sequence->durationTicks,
                                                                    gapStartOffset,gapEndOffset);
            for (auto &transition : typed.transitions) {
                const auto start = transition.timelineRange.start;
                const auto transitionEnd = start + transition.timelineRange.duration;
                if (start < end && transitionEnd > gap.start) { error = "transition intersects the gap"; return; }
                if (start >= end) transition.timelineRange.start -= gap.duration;
            }
            for (auto &marker : typed.properties.markers) {
                const auto start = marker.range.start;
                const auto markerEnd = start + marker.range.duration;
                if ((start >= gapStartOffset && start < gapEndOffset) ||
                    (start < gapEndOffset && markerEnd > gapStartOffset)) {
                    error = "track marker intersects the gap"; return;
                }
                if (start >= gapEndOffset) marker.range.start -= gap.duration;
            }
            for (auto &clip : typed.clips)
                if (clip.properties.timelineRange.start >= end) clip.properties.timelineRange.start -= gap.duration;
        }, track);
        if (!error.empty()) return failed(std::move(error));
    }
    return checked(std::move(result));
}
} // namespace

TimelineProjectPlan TimelineGapClosePlan::operator()(const TimelineProject &input, const std::string &sequenceId,
    const std::vector<std::string> &trackIds, TimelineTickRange gap) const {
    return gapClosePlan(input, sequenceId, trackIds, gap);
}

TimelineTrackEffectTimeResult TrackEffectTimeMapper::operator()(const TimelineTrackProperties &track,TimelineRational current) const {
    using exact_timeline::Integer;
    using exact_timeline::Rational;
    if(!current.denominator || current.numerator<0)return {{},"track-local time must be a nonnegative rational"};
    const Rational time(Integer(current.numerator),Integer(current.denominator));
    const auto project=[](const Rational &value)->TimelineTrackEffectTimeResult {
        const Integer n=numerator(value),d=denominator(value);
        if(n<std::numeric_limits<std::int64_t>::min() || n>std::numeric_limits<std::int64_t>::max() ||
           d>std::numeric_limits<std::uint64_t>::max())return {{},"effect time exceeds its integer budget"};
        return {TimelineRational{n.convert_to<std::int64_t>(),d.convert_to<std::uint64_t>()},{}};
    };
    if(!track.effectTimeMap)return project(time);
    const auto &map=*track.effectTimeMap;
    if(map.sourceDurationTicks<=0 || map.segments.size()>1000000)return {{},"invalid track-effect time map"};
    std::int64_t previousEnd=0;
    std::optional<Rational> mapped;
    for(const auto &segment:map.segments){
        const auto start=segment.outputRange.start,duration=segment.outputRange.duration,source=segment.sourceStartTicks;
        if(start<previousEnd || duration<=0 || start>std::numeric_limits<std::int64_t>::max()-duration ||
           source<0 || source>map.sourceDurationTicks || duration>map.sourceDurationTicks-source)
            return {{},"invalid track-effect time segment"};
        const auto end=start+duration;previousEnd=end;
        if(time>=start && time<end)mapped=Rational(source)+(time-start);
    }
    return mapped ? project(*mapped) : TimelineTrackEffectTimeResult{std::nullopt,{}};
}

TimelineProjectPlan SequenceSimplificationPlan::operator()(const TimelineProject &input,
    const std::string &sequenceId, const SequenceSimplificationOptions &options) const {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    TimelineProject result = input;
    auto *sequence = findTimelineSequence(result, sequenceId);
    if (!sequence) return failed("unknown sequence");
    auto targetId = sequenceId;
    if (options.outputSequenceId) {
        if (options.outputSequenceId->empty() || findTimelineSequence(result, *options.outputSequenceId))
            return failed("new sequence identity is empty or already exists");
        auto copy = *sequence;
        copy.id = *options.outputSequenceId;
        targetId = copy.id;
        result.sequences.push_back(std::move(copy));
        sequence = findTimelineSequence(result, targetId);
    }
    if (options.removeDisabledClips) {
        std::unordered_set<std::string> removed;
        for (const auto &track : sequence->tracks) std::visit([&](const auto &typed) {
            for (const auto &clip : typed.clips) if (!clip.properties.enabled) removed.insert(clip.properties.id);
        }, track);
        for (const auto &track : sequence->tracks) {
            bool rejected = false;
            std::visit([&](const auto &typed) {
                rejected = (typed.properties.locked && std::any_of(typed.clips.begin(), typed.clips.end(),
                    [&](const auto &clip) { return removed.contains(clip.properties.id); })) ||
                    std::any_of(typed.transitions.begin(), typed.transitions.end(), [&](const auto &transition) {
                        return removed.contains(transition.fromClipId) || removed.contains(transition.toClipId);
                    });
            }, track);
            if (rejected) return failed("disabled clips have locked or transition-dependent content");
        }
        for (auto &group : sequence->linkGroups) {
            std::erase_if(group.clipIds, [&](const auto &id) { return removed.contains(id); });
            if (group.clipIds.size() == 1) {
                const auto remaining = findTimelineClip(*sequence, group.clipIds.front());
                for (const auto &track : sequence->tracks) {
                    bool owns = false;
                    std::visit([&](const auto &typed) { owns = std::any_of(typed.clips.begin(), typed.clips.end(),
                        [&](const auto &clip) { return clip.properties.id == group.clipIds.front(); }); }, track);
                    if (owns && timelineTrackProperties(track).locked)
                        return failed("removing a disabled linked clip would modify a locked track");
                }
                if (remaining) timelineClipProperties(remaining).linkGroupId.clear();
            }
        }
        std::erase_if(sequence->linkGroups, [](const auto &group) { return group.clipIds.size() < 2; });
        for (auto &track : sequence->tracks) std::visit([&](auto &typed) {
            std::erase_if(typed.clips, [&](const auto &clip) { return removed.contains(clip.properties.id); });
        }, track);
    }
    if (options.flattenNestedSequences) {
        std::size_t expansions = 0;
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t trackIndex=0;trackIndex<sequence->tracks.size();++trackIndex) {
                auto &track=sequence->tracks[trackIndex];
                std::vector<TimelineTrack> additionalTracks;
                std::string error;
                std::visit([&](auto &typed) {
                    using Track = std::decay_t<decltype(typed)>;
                    // Video flattening retains the original editable audio,
                    // subtitle and data tracks, including nested references.
                    if constexpr (!std::is_same_v<Track,TimelineVideoTrack>) return;
                    for (std::size_t i = 0; i < typed.clips.size(); ++i) {
                        const auto &parent = typed.clips[i];
                        const auto *reference = std::get_if<TimelineSequenceReference>(&parent.properties.source);
                        if (!reference) continue;
                        if (++expansions > 65536) { error = "nested expansion exceeds the configured bound"; return; }
                        const auto *nested = findTimelineSequence(result, reference->sequenceId);
                        if (typed.properties.locked || !nested ||
                            nested->timeBase != sequence->timeBase || nested->editingFrameRate != sequence->editingFrameRate ||
                            nested->pixelAspectRatio != sequence->pixelAspectRatio || nested->color != sequence->color ||
                            nested->audioSampleRate != sequence->audioSampleRate ||
                            nested->audioChannelLayout != sequence->audioChannelLayout ||
                            nested->canvasExtent != sequence->canvasExtent || !nested->markers.empty() || !nested->linkGroups.empty() ||
                            parent.properties.sourceRange != TimelineTickRange{nested->startTicks, nested->durationTicks}) {
                            error = "nested sequence cannot be flattened without changing its composition"; return;
                        }
                        std::vector<const Track *> sourceTracks;
                        for(const auto &nestedTrack:nested->tracks)
                            if(const auto *source=std::get_if<Track>(&nestedTrack))sourceTracks.push_back(source);
                        auto neutralClip = parent.properties;
                        neutralClip.id.clear(); neutralClip.source = TimelineMediaReference{};
                        neutralClip.timelineRange = {}; neutralClip.sourceRange = {};
                        bool hasEditableOriginal=false;
                        if constexpr(std::is_same_v<Track,TimelineVideoTrack>)hasEditableOriginal=parent.editableOriginal.has_value();
                        if (neutralClip != TimelineClipProperties{} || hasEditableOriginal ||
                            std::any_of(typed.transitions.begin(), typed.transitions.end(), [&](const auto &transition) {
                                return transition.fromClipId == parent.properties.id || transition.toClipId == parent.properties.id;
                            })) {
                            error = "nested processing or transitions cannot be flattened losslessly"; return;
                        }
                        if(sourceTracks.size()>1){
                            auto outer=typed.properties;outer.id.clear();outer.name.clear();
                            const auto parentEnd=parent.properties.timelineRange.start+parent.properties.timelineRange.duration;
                            const auto intersects=[&](TimelineTickRange range){
                                return range.start<parentEnd && parent.properties.timelineRange.start<range.start+range.duration;
                            };
                            if(outer!=TimelineTrackProperties{} ||
                               std::any_of(typed.clips.begin(),typed.clips.end(),[&](const auto &other){
                                   return other.properties.id!=parent.properties.id && intersects(other.properties.timelineRange);
                               }) || std::any_of(typed.transitions.begin(),typed.transitions.end(),[&](const auto &t){return intersects(t.timelineRange);})){
                                error="nested tracks cannot be expanded across overlapping or processed outer content";return;
                            }
                        }
                        for(const auto *sourceTrack:sourceTracks){
                            auto neutralTrack=sourceTrack->properties;neutralTrack.id.clear();neutralTrack.name.clear();
                            if(neutralTrack!=TimelineTrackProperties{}){
                                error="nested track processing cannot be flattened losslessly";return;
                            }
                        }
                        if constexpr (std::is_same_v<Track, TimelineVideoTrack>) {
                            if (parent.transform != TimelineTransform{} || parent.crop != TimelineCrop{}) {
                                error = "nested video transform cannot be flattened losslessly"; return;
                            }
                            for(const auto *sourceTrack:sourceTracks)if (std::any_of(sourceTrack->clips.begin(), sourceTrack->clips.end(), [](const auto &child) {
                                    return child.transform.blendMode != "normal";
                                })) { error = "nested blend depends on the isolated background"; return; }
                        } else if constexpr (std::is_same_v<Track, TimelineAudioTrack>) {
                            if (parent.mix != TimelineAudioMix{}) { error = "nested audio mix cannot be flattened losslessly"; return; }
                            for(const auto *sourceTrack:sourceTracks)if (std::any_of(sourceTrack->clips.begin(), sourceTrack->clips.end(), [](const auto &child) {
                                    return !child.mix.outputBusId.empty();
                                })) { error = "nested audio bus cannot be flattened losslessly"; return; }
                        } else { error = "nested caption or data tracks cannot be flattened losslessly"; return; }
                        for(const auto *sourceTrack:sourceTracks)if (std::any_of(sourceTrack->clips.begin(), sourceTrack->clips.end(), [](const auto &child) {
                                return !child.properties.syncGroupId.empty() ||
                                    std::any_of(child.properties.effects.begin(),child.properties.effects.end(),
                                                [](const auto &effect){return !effect.inputIds.empty();});
                            })) { error = "nested effects or sync identities cannot be remapped losslessly"; return; }
                        const auto parentId=parent.properties.id;
                        const auto parentStart=parent.properties.timelineRange.start;
                        const auto remapClip=[&](auto &child) {
                            child.properties.id=parentId+":"+child.properties.id;
                            child.properties.timelineRange.start=parentStart+(child.properties.timelineRange.start-nested->startTicks);
                        };
                        const auto remapTransition=[&](auto &transition) {
                            transition.id=parentId+":"+transition.id;
                            if(!transition.fromClipId.empty())transition.fromClipId=parentId+":"+transition.fromClipId;
                            if(!transition.toClipId.empty())transition.toClipId=parentId+":"+transition.toClipId;
                            transition.timelineRange.start=parentStart+(transition.timelineRange.start-nested->startTicks);
                        };
                        typed.clips.erase(typed.clips.begin()+static_cast<std::ptrdiff_t>(i));
                        for(std::size_t lane=0;lane<sourceTracks.size();++lane){
                            auto children=sourceTracks[lane]->clips;
                            auto transitions=sourceTracks[lane]->transitions;
                            for(auto &child:children)remapClip(child);
                            for(auto &transition:transitions)remapTransition(transition);
                            if(!lane){
                                typed.clips.insert(typed.clips.begin()+static_cast<std::ptrdiff_t>(i),
                                    std::make_move_iterator(children.begin()),std::make_move_iterator(children.end()));
                                typed.transitions.insert(typed.transitions.end(),
                                    std::make_move_iterator(transitions.begin()),std::make_move_iterator(transitions.end()));
                            }else{
                                Track extra;extra.properties.id=typed.properties.id+":"+parentId+":"+sourceTracks[lane]->properties.id;
                                extra.properties.name=sourceTracks[lane]->properties.name;
                                extra.clips=std::move(children);extra.transitions=std::move(transitions);
                                additionalTracks.emplace_back(std::move(extra));
                            }
                        }
                        changed = true;
                        return;
                    }
                }, track);
                if (!error.empty()) return failed(std::move(error));
                if (changed) {
                    sequence->tracks.insert(sequence->tracks.begin()+static_cast<std::ptrdiff_t>(trackIndex+1),
                        std::make_move_iterator(additionalTracks.begin()),std::make_move_iterator(additionalTracks.end()));
                    break;
                }
            }
        }
    }
    if (options.removeEmptyTracks) {
        for (const auto &track : sequence->tracks) {
            bool empty = false;
            std::visit([&](const auto &typed) { empty = typed.clips.empty(); }, track);
            if (empty && timelineTrackProperties(track).locked) return failed("empty track is locked");
        }
        std::erase_if(sequence->tracks, [](const auto &track) {
            return std::visit([](const auto &typed) { return typed.clips.empty(); }, track);
        });
    }
    if (options.removeGaps && !sequence->tracks.empty()) {
        std::vector<TimelineTickRange> occupied;
        std::vector<std::string> ids;
        for (const auto &track : sequence->tracks) {
            ids.push_back(timelineTrackProperties(track).id);
            std::visit([&](const auto &typed) {
                for (const auto &clip : typed.clips) occupied.push_back(clip.properties.timelineRange);
            }, track);
        }
        std::sort(occupied.begin(), occupied.end(), [](auto a, auto b) { return a.start < b.start; });
        auto cursor = sequence->startTicks;
        std::vector<TimelineTickRange> gaps;
        for (const auto range : occupied) {
            if (range.start > cursor) gaps.push_back({cursor, range.start - cursor});
            cursor = std::max(cursor, range.start + range.duration);
        }
        std::int64_t removed = 0;
        for (auto gap : gaps) {
            gap.start -= removed;
            auto next = gapClosePlan(result, targetId, ids, gap);
            if (!next.ok()) return next;
            result = std::move(*next.project);
            removed += gap.duration;
        }
    }
    return checked(std::move(result));
}

TimelineProjectPlan AutoReframeKeyframeApplier::operator()(const TimelineProject &input,
    const std::string &sequenceId, const std::string &clipId, const AutoReframeRequest &request) const {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    if (request.effectId.empty() || (!request.keyframes.empty() && !request.observations.empty()))
        return failed("effect identity and exactly one keyframe source are required");
    TimelineProject result = input;
    auto *sequence = findTimelineSequence(result, sequenceId);
    if (!sequence) return failed("unknown sequence");
    TimelineVideoClip *clip = nullptr;
    for (auto &track : sequence->tracks) {
        auto *video = std::get_if<TimelineVideoTrack>(&track);
        if (!video) continue;
        auto found = std::find_if(video->clips.begin(), video->clips.end(),
            [&](const auto &item) { return item.properties.id == clipId; });
        if (found == video->clips.end()) continue;
        if (video->properties.locked) return failed("video track is locked");
        clip = &*found;
        break;
    }
    if (!clip) return failed("unknown video clip");
    auto keys = request.keyframes;
    if (!request.observations.empty()) {
        if (request.selectedTrackId.empty() || request.sourceExtent.width <= 0 || request.sourceExtent.height <= 0 ||
            request.targetExtent.width <= 0 || request.targetExtent.height <= 0 ||
            !std::isfinite(request.paddingFraction) || request.paddingFraction < 0 || request.paddingFraction > 10)
            return failed("invalid reframe analysis settings");
        const double width = request.sourceExtent.width, height = request.sourceExtent.height;
        const double aspect = double(request.targetExtent.width) / request.targetExtent.height;
        for (const auto &observation : request.observations) {
            if (observation.trackId != request.selectedTrackId) continue;
            const auto &origin = observation.origin, &extent = observation.extent;
            if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(extent.x) ||
                !std::isfinite(extent.y) || origin.x < 0 || origin.y < 0 || extent.x <= 0 || extent.y <= 0 ||
                origin.x + extent.x > width || origin.y + extent.y > height)
                return failed("invalid tracked subject bounds");
            const auto cropWidth = std::max(extent.x, extent.y * aspect) * (1 + 2 * request.paddingFraction);
            const auto cropHeight = cropWidth / aspect;
            if (!std::isfinite(cropWidth) || cropWidth <= 0 || cropWidth > width || cropHeight > height)
                return failed("tracked subject cannot fit the requested crop");
            const auto x = std::clamp(origin.x + extent.x / 2, cropWidth / 2, width - cropWidth / 2);
            const auto y = std::clamp(origin.y + extent.y / 2, cropHeight / 2, height - cropHeight / 2);
            keys.push_back({observation.timeTicks, {x, y}, width / cropWidth});
        }
    }
    if (keys.empty()) return failed("no keyframes for the selected subject");
    std::sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) { return a.timeTicks < b.timeTicks; });
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto &key = keys[i];
        if (key.timeTicks < 0 || key.timeTicks > clip->properties.timelineRange.duration ||
            (i && keys[i - 1].timeTicks == key.timeTicks) || !std::isfinite(key.center.x) ||
            !std::isfinite(key.center.y) || !std::isfinite(key.scale) || key.scale <= 0)
            return failed("reframe keyframe times or values are invalid");
    }
    TimelineEffect effect;
    effect.id = request.effectId;
    effect.name = "Auto reframe";
    effect.category = "transform";
    effect.pluginId = "builtin.auto-reframe";
    const std::array<std::string, 3> parameters{"centerX", "centerY", "scale"};
    for (std::size_t component = 0; component < parameters.size(); ++component) {
        const auto value = [&](const AutoReframeKeyframe &key) {
            return component == 0 ? key.center.x : component == 1 ? key.center.y : key.scale;
        };
        effect.parameters.push_back({parameters[component], value(keys.front())});
        TimelineAutomationCurve curve;
        curve.parameterId = parameters[component];
        curve.defaultValue = value(keys.front());
        for (const auto &key : keys)
            curve.keyframes.push_back({key.timeTicks, value(key), TimelineKeyframeInterpolation::Linear, {}, {}});
        effect.automation.push_back(std::move(curve));
    }
    auto &effects = clip->properties.effects;
    const auto existing = std::find_if(effects.begin(), effects.end(),
        [&](const auto &value) { return value.id == effect.id; });
    if (existing != effects.end()) {
        if (existing->pluginId != effect.pluginId) return failed("effect identity belongs to another effect");
        *existing = std::move(effect);
    } else effects.push_back(std::move(effect));
    return checked(std::move(result));
}

AutoReframeTrackingResult AutoReframeSubjectTracker::operator()(std::span<const AutoReframeDetection> input,
    const AutoReframeTrackingOptions &options) const {
    if (options.idPrefix.empty() || options.maximumGapTicks < 0 || !std::isfinite(options.minimumConfidence) ||
        options.minimumConfidence < 0 || options.minimumConfidence > 1 || !std::isfinite(options.minimumOverlap) ||
        options.minimumOverlap < 0 || options.minimumOverlap > 1 || input.size() > options.maximumDetections ||
        !options.maximumTracks) return {{}, "invalid tracking options or detection budget"};
    std::vector<const AutoReframeDetection *> sorted;
    sorted.reserve(input.size());
    for (const auto &detection : input) {
        if (detection.timeTicks < 0 || !std::isfinite(detection.confidence) || detection.confidence < 0 ||
            detection.confidence > 1 || !std::isfinite(detection.origin.x) || !std::isfinite(detection.origin.y) ||
            !std::isfinite(detection.extent.x) || !std::isfinite(detection.extent.y) ||
            detection.origin.x < 0 || detection.origin.y < 0 || detection.extent.x <= 0 || detection.extent.y <= 0 ||
            !std::isfinite(detection.origin.x + detection.extent.x) || !std::isfinite(detection.origin.y + detection.extent.y) ||
            !std::isfinite(detection.extent.x * detection.extent.y))
            return {{}, "invalid detector result"};
        if (detection.confidence >= options.minimumConfidence) sorted.push_back(&detection);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto *a, const auto *b) {
        return a->timeTicks != b->timeTicks ? a->timeTicks < b->timeTicks : a->confidence > b->confidence;
    });
    AutoReframeTrackingResult result;
    const auto overlap = [](const AutoReframeObservation &prior, const AutoReframeDetection &next) {
        const auto left = std::max(prior.origin.x, next.origin.x), top = std::max(prior.origin.y, next.origin.y);
        const auto right = std::min(prior.origin.x + prior.extent.x, next.origin.x + next.extent.x);
        const auto bottom = std::min(prior.origin.y + prior.extent.y, next.origin.y + next.extent.y);
        const auto intersection = std::max(0.0, right - left) * std::max(0.0, bottom - top);
        const auto total = prior.extent.x * prior.extent.y + next.extent.x * next.extent.y - intersection;
        return std::isfinite(total) && total > 0 ? intersection / total : 0.0;
    };
    for (const auto *detection : sorted) {
        std::size_t best = result.tracks.size();
        auto bestOverlap = options.minimumOverlap;
        for (std::size_t index = 0; index < result.tracks.size(); ++index) {
            const auto &track = result.tracks[index];
            const auto &last = track.observations.back();
            if (track.classId != detection->classId || track.label != detection->label ||
                last.timeTicks >= detection->timeTicks || detection->timeTicks - last.timeTicks > options.maximumGapTicks)
                continue;
            const auto score = overlap(last, *detection);
            if (score >= bestOverlap && (best == result.tracks.size() || score > bestOverlap)) {
                best = index; bestOverlap = score;
            }
        }
        if (best == result.tracks.size()) {
            if (result.tracks.size() >= options.maximumTracks) return {{}, "subject track budget exceeded"};
            result.tracks.push_back({options.idPrefix + "/" + std::to_string(result.tracks.size()),
                                     detection->classId, detection->label, {}});
        }
        auto &track = result.tracks[best];
        track.observations.push_back({detection->timeTicks, track.id, detection->origin, detection->extent});
    }
    return result;
}

namespace {
struct TrackSelection { std::vector<std::string> ids; std::string error; };

TrackSelection removalTracks(const TimelineSequence &sequence, const std::vector<std::string> &requested,
                             TimelineTickRange range) {
    if (requested.empty()) return {{}, "at least one track is required"};
    const auto end = range.start + range.duration;
    struct Location { std::size_t track; TimelineTickRange range; };
    std::unordered_map<std::string, Location> clips;
    std::unordered_set<std::size_t> included;
    std::unordered_set<std::string> unique;
    for (std::size_t i = 0; i < sequence.tracks.size(); ++i) {
        const auto &track = sequence.tracks[i];
        if (timelineTrackProperties(track).syncLocked) included.insert(i);
        std::visit([&](const auto &typed) {
            for (const auto &clip : typed.clips) clips.emplace(clip.properties.id, Location{i, clip.properties.timelineRange});
        }, track);
    }
    for (const auto &id : requested) {
        if (!unique.insert(id).second) return {{}, "duplicate track identity"};
        const auto found = std::find_if(sequence.tracks.begin(), sequence.tracks.end(), [&](const auto &track) {
            return timelineTrackProperties(track).id == id;
        });
        if (found == sequence.tracks.end()) return {{}, "unknown track"};
        included.insert(static_cast<std::size_t>(found - sequence.tracks.begin()));
    }
    const auto intersects = [&](TimelineTickRange value) {
        return value.start < end && value.start + value.duration > range.start;
    };
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const auto &group : sequence.linkGroups) {
            const bool edits = std::any_of(group.clipIds.begin(), group.clipIds.end(), [&](const auto &id) {
                const auto &location = clips.at(id);
                return included.contains(location.track) && intersects(location.range);
            });
            if (!edits) continue;
            for (const auto &id : group.clipIds) {
                const auto &location = clips.at(id);
                if (!intersects(location.range)) return {{}, "linked clips do not share the selected interval"};
                expanded |= included.insert(location.track).second;
            }
        }
    }
    TrackSelection result;
    for (std::size_t i = 0; i < sequence.tracks.size(); ++i) if (included.contains(i)) {
        if (timelineTrackProperties(sequence.tracks[i]).locked) return {{}, "required track is locked"};
        result.ids.push_back(timelineTrackProperties(sequence.tracks[i]).id);
    }
    return result;
}

TimelineProjectPlan splitBoundary(const TimelineProject &input, const std::string &sequenceId,
                                  const std::vector<std::string> &trackIds, std::int64_t cut,
                                  const std::string &prefix, const char *boundary) {
    const auto *sequence = findTimelineSequence(input, sequenceId);
    if (!sequence) return failed("unknown sequence");
    const std::unordered_set<std::string> tracks(trackIds.begin(), trackIds.end());
    std::vector<TimelineSplitTarget> targets;
    std::unordered_set<std::string> selected;
    for (const auto &track : sequence->tracks) {
        if (!tracks.contains(timelineTrackProperties(track).id)) continue;
        std::visit([&](const auto &typed) {
            for (const auto &clip : typed.clips) {
                const auto range = clip.properties.timelineRange;
                if (range.start < cut && range.start + range.duration > cut) {
                    selected.insert(clip.properties.id);
                    targets.push_back({clip.properties.id, prefix + "/" + boundary + "/clip/" + clip.properties.id});
                }
            }
        }, track);
    }
    if (targets.empty()) return {input, {}};
    std::vector<TimelineSplitLink> links;
    for (const auto &group : sequence->linkGroups) {
        if (std::any_of(group.clipIds.begin(), group.clipIds.end(), [&](const auto &id) { return selected.contains(id); }))
            links.push_back({group.id, prefix + "/" + boundary + "/link/" + group.id});
    }
    return splitPlan(input, sequenceId, targets, links, cut);
}

TimelineProjectPlan splitRange(const TimelineProject &input, const std::string &sequenceId,
                               const std::vector<std::string> &trackIds, TimelineTickRange range,
                               const std::string &prefix) {
    auto start = splitBoundary(input, sequenceId, trackIds, range.start, prefix, "start");
    if (!start.ok()) return start;
    return splitBoundary(*start.project, sequenceId, trackIds, range.start + range.duration, prefix, "end");
}

TimelineProjectPlan removeRange(const TimelineProject &input, const std::string &sequenceId,
    const std::vector<std::string> &trackIds, TimelineTickRange range, const std::string &prefix, bool extract) {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    const auto *original = findTimelineSequence(input, sequenceId);
    if (!original || prefix.empty() || !safeRange(range, original->startTicks, original->startTicks + original->durationTicks))
        return failed("valid sequence, range and identity namespace are required");
    const auto selection = removalTracks(*original, trackIds, range);
    if (!selection.error.empty()) return failed(selection.error);
    auto split = splitRange(input, sequenceId, selection.ids, range, prefix);
    if (!split.ok()) return split;
    auto &result = *split.project;
    auto *sequence = findTimelineSequence(result, sequenceId);
    const std::unordered_set<std::string> tracks(selection.ids.begin(), selection.ids.end());
    const auto end = range.start + range.duration;
    std::unordered_set<std::string> removed;
    for (const auto &track : sequence->tracks) {
        if (!tracks.contains(timelineTrackProperties(track).id)) continue;
        std::visit([&](const auto &typed) {
            for (const auto &clip : typed.clips) {
                const auto value = clip.properties.timelineRange;
                if (value.start >= range.start && value.start + value.duration <= end) removed.insert(clip.properties.id);
            }
        }, track);
    }
    for (const auto &group : sequence->linkGroups) {
        const auto count = std::count_if(group.clipIds.begin(), group.clipIds.end(),
            [&](const auto &id) { return removed.contains(id); });
        if (count && static_cast<std::size_t>(count) != group.clipIds.size())
            return failed("removal would break a linked group");
    }
    for (auto &track : sequence->tracks) {
        if (!tracks.contains(timelineTrackProperties(track).id)) continue;
        std::string error;
        std::visit([&](auto &typed) {
            for (const auto &transition : typed.transitions) {
                if (!removed.contains(transition.fromClipId) && !removed.contains(transition.toClipId)) continue;
                if (transition.timelineRange.start < range.start ||
                    transition.timelineRange.start + transition.timelineRange.duration > end) {
                    error = "transition crosses the removed interval"; return;
                }
            }
            std::erase_if(typed.transitions, [&](const auto &transition) {
                return removed.contains(transition.fromClipId) || removed.contains(transition.toClipId);
            });
            std::erase_if(typed.clips, [&](const auto &clip) { return removed.contains(clip.properties.id); });
        }, track);
        if (!error.empty()) return failed(std::move(error));
    }
    std::erase_if(sequence->linkGroups, [&](const auto &group) {
        return !group.clipIds.empty() && removed.contains(group.clipIds.front());
    });
    auto lifted = checked(std::move(result));
    if (!lifted.ok() || !extract) return lifted;
    return gapClosePlan(*lifted.project, sequenceId, selection.ids, range);
}

// Markers are local offsets for sequence and track properties. Values wholly
// outside the selection are not part of the extracted subsequence.
bool sliceMarkers(std::vector<TimelineMarker> &markers, std::int64_t start, std::int64_t end) {
    std::vector<TimelineMarker> kept;
    for (auto marker : markers) {
        const auto markerEnd = marker.range.start + marker.range.duration;
        const bool point = marker.range.duration == 0;
        if ((point && (marker.range.start < start || marker.range.start >= end)) ||
            (!point && (markerEnd <= start || marker.range.start >= end))) continue;
        if (marker.range.start < start || markerEnd > end) return false;
        marker.range.start -= start;
        kept.push_back(std::move(marker));
    }
    markers = std::move(kept);
    return true;
}
} // namespace

TimelineProjectPlan TimelineLiftPlan::operator()(const TimelineProject &input, const std::string &sequenceId,
    const std::vector<std::string> &trackIds, TimelineTickRange range, const std::string &prefix) const {
    return removeRange(input, sequenceId, trackIds, range, prefix, false);
}

TimelineProjectPlan TimelineExtractPlan::operator()(const TimelineProject &input, const std::string &sequenceId,
    const std::vector<std::string> &trackIds, TimelineTickRange range, const std::string &prefix) const {
    return removeRange(input, sequenceId, trackIds, range, prefix, true);
}

TimelineProjectPlan SubsequenceExtractor::operator()(const TimelineProject &input, const std::string &sequenceId,
    TimelineTickRange range, const std::string &newSequenceId, SubsequenceMode mode, const std::string &prefix) const {
    if (!validateTimelineProject(input).ok()) return failed("invalid input project");
    const auto *original = findTimelineSequence(input, sequenceId);
    if (!original || newSequenceId.empty() || prefix.empty() || findTimelineSequence(input, newSequenceId) ||
        !safeRange(range, original->startTicks, original->startTicks + original->durationTicks) ||
        (mode != SubsequenceMode::Copy && mode != SubsequenceMode::Move))
        return failed("valid sequence, range and new identities are required");
    std::vector<std::string> trackIds;
    std::unordered_map<std::string, bool> locks;
    auto temporary = input;
    auto *temporarySequence = findTimelineSequence(temporary, sequenceId);
    for (auto &track : temporarySequence->tracks) {
        auto &properties = timelineTrackProperties(track);
        trackIds.push_back(properties.id);
        locks.emplace(properties.id, properties.locked);
        properties.locked = false;
    }
    auto split = splitRange(temporary, sequenceId, trackIds, range, prefix + "/destination");
    if (!split.ok()) return split;
    auto subsequence = *findTimelineSequence(*split.project, sequenceId);
    const auto end = range.start + range.duration;
    const auto localStart = range.start - original->startTicks;
    const auto localEnd = end - original->startTicks;
    std::unordered_set<std::string> kept;
    for (const auto &track : subsequence.tracks) std::visit([&](const auto &typed) {
        for (const auto &clip : typed.clips) {
            const auto value = clip.properties.timelineRange;
            if (value.start >= range.start && value.start + value.duration <= end) kept.insert(clip.properties.id);
        }
    }, track);
    for (const auto &group : subsequence.linkGroups) {
        const auto count = std::count_if(group.clipIds.begin(), group.clipIds.end(),
            [&](const auto &id) { return kept.contains(id); });
        if (count && static_cast<std::size_t>(count) != group.clipIds.size())
            return failed("subsequence would break a linked group");
    }
    std::erase_if(subsequence.linkGroups, [&](const auto &group) {
        return !group.clipIds.empty() && !kept.contains(group.clipIds.front());
    });
    for (auto &track : subsequence.tracks) {
        std::string error;
        std::visit([&](auto &typed) {
            if(!staticEffects(typed.properties.effects) || typed.properties.effectTimeMap)
                typed.properties.effectTimeMap=sliceEffectInterval(typed.properties,original->durationTicks,localStart,localEnd);
            if(!sliceMarkers(typed.properties.markers, localStart, localEnd)) {
                error = "track markers cannot be sliced losslessly"; return;
            }
            typed.properties.locked = locks.at(typed.properties.id);
            std::erase_if(typed.clips, [&](const auto &clip) { return !kept.contains(clip.properties.id); });
            for (auto &clip : typed.clips) clip.properties.timelineRange.start -= range.start;
            for (const auto &transition : typed.transitions) {
                const auto transitionEnd = transition.timelineRange.start + transition.timelineRange.duration;
                if (transition.timelineRange.start < end && transitionEnd > range.start &&
                    (transition.timelineRange.start < range.start || transitionEnd > end)) {
                    error = "transition crosses the subsequence boundary"; return;
                }
            }
            std::erase_if(typed.transitions, [&](const auto &transition) {
                return transition.timelineRange.start < range.start ||
                       transition.timelineRange.start + transition.timelineRange.duration > end;
            });
            for (auto &transition : typed.transitions) transition.timelineRange.start -= range.start;
        }, track);
        if (!error.empty()) return failed(std::move(error));
    }
    if (!sliceMarkers(subsequence.markers, localStart, localEnd)) return failed("sequence marker crosses the selected range");
    if (subsequence.workArea) {
        const auto work = *subsequence.workArea;
        if (work.start >= range.start && work.start + work.duration <= end)
            subsequence.workArea->start -= range.start;
        else if (work.start + work.duration <= range.start || work.start >= end) subsequence.workArea.reset();
        else return failed("work area crosses the selected range");
    }
    if (subsequence.timecode) {
        const auto &rate = subsequence.timecode->frameRate;
        const auto frames = requiredSourceTicks(localStart, original->timeBase,
            {rate.denominator, rate.numerator}, {1, 1}, false);
        if (!frames || *frames > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
            subsequence.timecode->startFrame > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t>(*frames))
            return failed("subsequence timecode does not fall on an exact frame");
        subsequence.timecode->startFrame += static_cast<std::int64_t>(*frames);
    }
    subsequence.id = newSequenceId;
    subsequence.startTicks = 0;
    subsequence.durationTicks = range.duration;
    TimelineProject result = input;
    if (mode == SubsequenceMode::Move && !trackIds.empty()) {
        auto moved = removeRange(input, sequenceId, trackIds, range, prefix + "/source", true);
        if (!moved.ok()) return moved;
        result = std::move(*moved.project);
    }
    result.sequences.push_back(std::move(subsequence));
    return checked(std::move(result));
}
} // namespace iiSharedCanvas
