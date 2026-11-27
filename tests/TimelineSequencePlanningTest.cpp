#include <Timeline/SequencePlanning.h>

#include <iostream>
#include <string>
#include <vector>

using namespace iiSharedCanvas;

namespace {
int failures = 0;
void expect(bool condition, const char *message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

TimelineProject project() {
    TimelineProject project;
    project.id = "p";
    project.activeSequenceId = "sequence";
    TimelineSequence sequence;
    sequence.id = "sequence";
    sequence.timeBase = {1, 24};
    sequence.durationTicks = 100;
    sequence.canvasExtent = {64, 64};
    TimelineVideoTrack track;
    track.properties.id = "video";
    for (const auto &[id, start] : std::vector<std::pair<std::string, std::int64_t>>{{"a", 10}, {"b", 40}}) {
        TimelineVideoClip clip;
        clip.properties.id = id;
        clip.properties.source = TimelineGeneratedReference{"generator", TimelineStreamKind::Video, {1, 24}, {}};
        clip.properties.timelineRange = {start, 20};
        clip.properties.sourceRange = {start, 20};
        track.clips.push_back(std::move(clip));
    }
    sequence.tracks.emplace_back(std::move(track));
    project.sequences.push_back(std::move(sequence));
    return project;
}

void gaps() {
    auto input = project();
    const auto result = TimelineGapIndex{}(input.sequences[0], {"video"});
    expect(result.ok() && result.gaps.size() == 3 &&
           result.gaps[0].range == TimelineTickRange{0, 10} &&
           result.gaps[1].range == TimelineTickRange{30, 10} &&
           result.gaps[2].range == TimelineTickRange{60, 40},
           "track gaps cover every unoccupied half-open interval");
    expect(!TimelineGapIndex{}(input.sequences[0], {"missing"}).ok(),
           "unknown track is an error, not a silently empty gap index");
    auto ambiguous = input.sequences[0];
    ambiguous.tracks.push_back(ambiguous.tracks[0]);
    expect(!TimelineGapIndex{}(ambiguous, {"video"}).ok(),
           "duplicate track identities are rejected instead of choosing an arbitrary track");
    auto &clips = std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]).clips;
    clips[1].properties.timelineRange = {20, 20};
    auto overlap = TimelineGapIndex{}(input.sequences[0], {"video"});
    expect(overlap.ok() && overlap.gaps.size() == 2 && overlap.gaps[1].range == TimelineTickRange{40, 60},
           "overlapping occupied ranges are merged before gaps are reported");
}

void settingsAndTrackLocks() {
    auto input = project();
    expect(validateTimelineProject(input).ok(), "test project is valid");
    SequenceSettingsChange settings;
    settings.name = "edited";
    settings.editingFrameRate = TimelineFrameRate{30, 1};
    settings.canvasExtent = TimelineExtent{128, 72};
    auto changed = SequenceSettingsEditor{}(input, "sequence", settings);
    expect(changed.ok() && changed.project && changed.project->sequences[0].name == "edited" &&
           changed.project->sequences[0].editingFrameRate == TimelineFrameRate{30, 1} &&
           input.sequences[0].name.empty(), "settings are validated on a detached project");
    settings.editingFrameRate = TimelineFrameRate{0, 1};
    expect(!SequenceSettingsEditor{}(input, "sequence", settings).ok(),
           "invalid sequence settings are rejected without changing the input");
    TimelineTrackChanges change;
    change.muted = true;
    auto batch = TimelineTrackBatchEdit{}(input, "sequence", {"video"}, change);
    expect(batch.ok() && batch.project && timelineTrackProperties(batch.project->sequences[0].tracks[0]).muted &&
           !timelineTrackProperties(input.sequences[0].tracks[0]).muted,
           "track batch edits produce a detached, validated project");
    timelineTrackProperties(input.sequences[0].tracks[0]).locked = true;
    expect(!TimelineTrackBatchEdit{}(input, "sequence", {"video"}, change).ok(),
           "locked tracks reject property edits atomically");
}

void transitionDefaults() {
    const auto video = DefaultTransitionPolicy{}(TimelineStreamKind::Video, 12);
    const auto audio = DefaultTransitionPolicy{}(TimelineStreamKind::Audio, 24);
    const auto custom = DefaultTransitionPolicy{}(TimelineStreamKind::Video, 5, "custom.wipe");
    expect(video.ok() && video.typeId == "builtin.cross-dissolve" && video.durationTicks == 12,
           "video defaults to the requested-duration cross dissolve");
    expect(audio.ok() && audio.typeId == "builtin.equal-power-fade" && audio.durationTicks == 24,
           "audio defaults to equal-power fade");
    expect(custom.ok() && custom.typeId == "custom.wipe" &&
           !DefaultTransitionPolicy{}(TimelineStreamKind::Subtitle, 12).ok() &&
           !DefaultTransitionPolicy{}(TimelineStreamKind::Audio, 0).ok(),
           "caller overrides are preserved and unsupported kinds or lengths are rejected");
}

void captionEdits() {
    auto input = project();
    TimelineSubtitleTrack track;
    track.properties.id = "captions";
    input.sequences[0].tracks.emplace_back(track);
    TimelineSubtitleClip clip;
    clip.properties.id = "caption-a";
    clip.properties.source = TimelineGeneratedReference{"text", TimelineStreamKind::Subtitle, {1, 24}, {}};
    clip.properties.timelineRange = {5, 10};
    clip.properties.sourceRange = {0, 10};
    clip.text = "hello";
    clip.style.fontSize = 18;
    CaptionTrackChange change;
    change.action = CaptionTrackAction::Upsert;
    change.clip = clip;
    change.language = "ko";
    auto edited = CaptionTrackEditor{}(input, "sequence", "captions", change);
    expect(edited.ok() && edited.project &&
           std::get<TimelineSubtitleTrack>(edited.project->sequences[0].tracks[1]).clips[0].text == "hello" &&
           timelineTrackProperties(edited.project->sequences[0].tracks[1]).language == "ko" &&
           std::get<TimelineSubtitleTrack>(input.sequences[0].tracks[1]).clips.empty(),
           "caption content, language and style form a detached edit");
    if (!edited.ok()) return;
    change.action = CaptionTrackAction::Remove;
    change.clipId = "caption-a";
    change.language.reset();
    auto removed = CaptionTrackEditor{}(*edited.project, "sequence", "captions", change);
    expect(removed.ok() && std::get<TimelineSubtitleTrack>(removed.project->sequences[0].tracks[1]).clips.empty(),
           "caption removal produces a validated detached project");
    timelineTrackProperties(edited.project->sequences[0].tracks[1]).locked = true;
    expect(!CaptionTrackEditor{}(*edited.project, "sequence", "captions", change).ok(),
           "caption changes on locked tracks fail");
}

void throughEdits() {
    auto input = project();
    auto &track = std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]);
    track.clips[0].properties.timelineRange = {0, 20};
    track.clips[0].properties.sourceRange = {0, 20};
    track.clips[1].properties.timelineRange = {20, 20};
    track.clips[1].properties.sourceRange = {20, 20};
    auto found = ThroughEditDetector{}(input, "sequence", {"video"});
    expect(found.ok() && found.edits.size() == 1 && found.edits[0].cutTicks == 20 &&
           found.edits[0].fromClipId == "a" && found.edits[0].toClipId == "b",
           "continuous adjacent source and rendering properties identify a through edit");
    track.clips[1].transform.opacity = .5;
    expect(ThroughEditDetector{}(input, "sequence", {"video"}).edits.empty(),
           "a rendering discontinuity is not a through edit");
    track.clips[1].transform.opacity = 1;
    track.clips[0].properties.playbackRate = {-1, 1};
    track.clips[1].properties.playbackRate = {-1, 1};
    track.clips[0].properties.sourceRange = {20, 20};
    track.clips[1].properties.sourceRange = {0, 20};
    expect(ThroughEditDetector{}(input, "sequence", {"video"}).edits.size() == 1,
           "reverse playback uses reverse source continuity");
}

void transitions() {
    auto input = project();
    auto &track = std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]);
    track.clips[0].properties.timelineRange = {0, 20};
    track.clips[0].properties.sourceRange = {10, 20};
    track.clips[1].properties.timelineRange = {20, 20};
    track.clips[1].properties.sourceRange = {30, 20};
    TimelineTransition transition;
    transition.id = "dissolve";
    transition.typeId = "builtin.cross-dissolve";
    transition.fromClipId = "a";
    transition.toClipId = "b";
    transition.timelineRange = {15, 10};
    auto plan = TimelineTransitionPlacement{}(input, "sequence", "video", transition);
    expect(plan.ok() && plan.project &&
           std::get<TimelineVideoTrack>(plan.project->sequences[0].tracks[0]).transitions.size() == 1 &&
           track.transitions.empty(), "transition placement validates and detaches the candidate");
    track.properties.locked = true;
    expect(!TimelineTransitionPlacement{}(input, "sequence", "video", transition).ok(),
           "transition placement rejects locked tracks");
    track.properties.locked = false;
    TimelineVideoStream stream;
    stream.id = "stream"; stream.timeBase = {1, 24}; stream.durationTicks = 40;
    stream.codec.identifier = "test"; stream.codedExtent = stream.displayExtent = {4, 4};
    stream.pixelFormat = "rgba"; stream.bitDepth = 8; stream.timing.nominal = TimelineFrameRate{24, 1};
    TimelineMediaRepresentation representation;
    representation.id = "original"; representation.uri = "file:///source.mov";
    representation.container.identifier = "test"; representation.timeBase = {1, 24};
    representation.durationTicks = 40; representation.streams.emplace_back(stream);
    TimelineMediaSource source;
    source.id = "source"; source.originalRepresentationId = source.activeRepresentationId = "original";
    source.representations.push_back(representation);
    input.mediaSources.push_back(source);
    track.clips[0].properties.source = TimelineMediaReference{"source", "stream"};
    track.clips[0].properties.sourceRange = {20, 20};
    track.clips[1].properties.source = TimelineMediaReference{"source", "stream"};
    track.clips[1].properties.sourceRange = {0, 20};
    expect(validateTimelineProject(input).ok(), "finite transition fixture is valid");
    expect(!TimelineTransitionPlacement{}(input, "sequence", "video", transition).ok(),
           "transition placement rejects insufficient source handles");
}

void splitPlans() {
    auto input = project();
    auto result = TimelineSplitPlan{}(input, "sequence", {{"a", "a-right"}}, {}, 20);
    expect(result.ok() && result.project &&
           std::get<TimelineVideoTrack>(result.project->sequences[0].tracks[0]).clips.size() == 3,
           "splitting a clip creates two detached typed segments");
    if (result.ok()) {
        const auto &clips = std::get<TimelineVideoTrack>(result.project->sequences[0].tracks[0]).clips;
        expect(clips[0].properties.timelineRange == TimelineTickRange{10, 10} &&
               clips[0].properties.sourceRange == TimelineTickRange{10, 10} &&
               clips[1].properties.id == "a-right" &&
               clips[1].properties.timelineRange == TimelineTickRange{20, 10} &&
               clips[1].properties.sourceRange == TimelineTickRange{20, 10},
               "the split preserves exact timeline and source intervals");
    }
    auto &track = std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]);
    track.properties.locked = true;
    expect(!TimelineSplitPlan{}(input, "sequence", {{"a", "a-right"}}, {}, 20).ok(),
           "splitting a locked track is rejected atomically");
    track.properties.locked = false;
    track.clips[1].properties.timelineRange = {10, 20};
    track.clips[1].properties.sourceRange = {10, 20};
    track.clips[0].properties.linkGroupId = track.clips[1].properties.linkGroupId = "linked";
    input.sequences[0].linkGroups.push_back({"linked", {"a", "b"}, true});
    expect(!TimelineSplitPlan{}(input, "sequence", {{"a", "a-right"}}, {{"linked", "linked-right"}}, 20).ok(),
           "splitting only part of a linked group is rejected");
    auto linked = TimelineSplitPlan{}(input, "sequence", {{"a", "a-right"}, {"b", "b-right"}},
                                      {{"linked", "linked-right"}}, 20);
    expect(linked.ok() && linked.project && linked.project->sequences[0].linkGroups.size() == 2 &&
           linked.project->sequences[0].linkGroups[1].clipIds == std::vector<std::string>({"a-right", "b-right"}),
           "splitting an entire linked group preserves separate left and right relationships");

    auto animated = project();
    auto &animatedClip = std::get<TimelineVideoTrack>(animated.sequences[0].tracks[0]).clips[0];
    animatedClip.properties.loopMode = TimelineLoopMode::PingPong;
    animatedClip.properties.sourceRange.duration = 5;
    TimelineEffect effect; effect.id = "animated"; effect.pluginId = "example";
    effect.activeRange = TimelineTickRange{2, 16};
    effect.parameters.push_back({"amount", 0.0});
    TimelineAutomationCurve curve; curve.parameterId = "amount"; curve.defaultValue = 0.0;
    curve.keyframes = {{0, 0.0}, {20, 1.0}}; effect.automation.push_back(curve);
    animatedClip.properties.effects.push_back(effect);
    expect(validateTimelineProject(animated).ok(), "animated looping input is valid");
    const auto cut = TimelineSplitPlan{}(animated, "sequence", {{"a", "a-right"}}, {}, 20);
    expect(cut.ok() && cut.project && [&] {
        const auto &clips=std::get<TimelineVideoTrack>(cut.project->sequences[0].tracks[0]).clips;
        return clips[0].properties.contentWindow == TimelineClipContentWindow{0,20} &&
               clips[1].properties.contentWindow == TimelineClipContentWindow{10,20} &&
               clips[0].properties.effects == animatedClip.properties.effects &&
               clips[1].properties.effects == animatedClip.properties.effects &&
               clips[0].properties.sourceRange == clips[1].properties.sourceRange;
    }(), "splitting preserves original loop phase and effect automation in content windows");
}

void trimAndExtend() {
    auto input = project();
    auto trimmed = TimelineTrimPlan{}(input, "sequence", {{"a", {15, 10}}});
    expect(trimmed.ok() && trimmed.project &&
           std::get<TimelineVideoTrack>(trimmed.project->sequences[0].tracks[0]).clips[0].properties.sourceRange ==
               TimelineTickRange{15, 10},
           "trim keeps exact source mapping while retaining the input");
    const auto &original = std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]).clips[0];
    expect(original.properties.timelineRange == TimelineTickRange{10, 20}, "trim does not mutate input");
    auto animated = project();
    auto &animatedClip = std::get<TimelineVideoTrack>(animated.sequences[0].tracks[0]).clips[0];
    animatedClip.properties.loopMode = TimelineLoopMode::Repeat;
    animatedClip.properties.sourceRange.duration = 5;
    TimelineEffect effect; effect.id = "bounded"; effect.pluginId = "example";
    effect.activeRange = TimelineTickRange{1, 18}; animatedClip.properties.effects.push_back(effect);
    const auto preserved = TimelineTrimPlan{}(animated, "sequence", {{"a", {13, 11}}});
    expect(preserved.ok() && preserved.project && [&] {
        const auto &clip=std::get<TimelineVideoTrack>(preserved.project->sequences[0].tracks[0]).clips[0];
        return clip.properties.contentWindow == TimelineClipContentWindow{3,20} &&
               clip.properties.effects == animatedClip.properties.effects &&
               clip.properties.sourceRange == animatedClip.properties.sourceRange;
    }(), "trimming keeps the loop phase and animated effect in original content coordinates");
    const auto loopExtended=TimelineExtendToPlayheadPlan{}(animated,"sequence",{"a"},TimelineClipEdge::Start,5);
    expect(loopExtended.ok() && loopExtended.project && [&] {
        const auto &clip=std::get<TimelineVideoTrack>(loopExtended.project->sequences[0].tracks[0]).clips[0];
        return clip.properties.timelineRange==TimelineTickRange{5,25} &&
               clip.properties.contentWindow==TimelineClipContentWindow{-5,20} &&
               clip.properties.effects==animatedClip.properties.effects;
    }(),"a loop can extend before its original content while preserving its original phase and effects");
    auto extended = TimelineExtendToPlayheadPlan{}(input, "sequence", {"a"}, TimelineClipEdge::Start, 5);
    expect(extended.ok() && extended.project &&
           std::get<TimelineVideoTrack>(extended.project->sequences[0].tracks[0]).clips[0].properties.timelineRange ==
               TimelineTickRange{5, 25} &&
           std::get<TimelineVideoTrack>(extended.project->sequences[0].tracks[0]).clips[0].properties.sourceRange ==
               TimelineTickRange{5, 25},
           "extend to playhead adjusts timeline and source together");
    auto &track = std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]);
    track.properties.locked = true;
    expect(!TimelineTrimPlan{}(input, "sequence", {{"a", {15, 10}}}).ok() &&
           !TimelineExtendToPlayheadPlan{}(input, "sequence", {"a"}, TimelineClipEdge::Start, 5).ok(),
           "trim and extend respect track locks");
}

void closeGaps() {
    auto input = project();
    TimelineVideoTrack sync;
    sync.properties.id = "sync";
    sync.properties.syncLocked = true;
    TimelineVideoClip clip;
    clip.properties.id = "sync-clip";
    clip.properties.source = TimelineGeneratedReference{"generator", TimelineStreamKind::Video, {1, 24}, {}};
    clip.properties.timelineRange = {45, 10};
    clip.properties.sourceRange = {0, 10};
    sync.clips.push_back(clip);
    input.sequences[0].tracks.emplace_back(sync);
    auto closed = TimelineGapClosePlan{}(input, "sequence", {"video"}, {30, 10});
    expect(closed.ok() && closed.project &&
           std::get<TimelineVideoTrack>(closed.project->sequences[0].tracks[0]).clips[1].properties.timelineRange.start == 30 &&
           std::get<TimelineVideoTrack>(closed.project->sequences[0].tracks[1]).clips[0].properties.timelineRange.start == 35,
           "closing a gap moves later clips on selected and sync-locked tracks");
    auto animated=project();
    auto &animatedTrack=std::get<TimelineVideoTrack>(animated.sequences[0].tracks[0]);
    TimelineEffect effect;effect.id="track-animation";effect.pluginId="example";
    effect.parameters.push_back({"amount",0.0});
    TimelineAutomationCurve curve;curve.parameterId="amount";curve.defaultValue=0.0;
    curve.keyframes={{0,0.0,TimelineKeyframeInterpolation::Bezier},{100,1.0}};
    effect.automation.push_back(curve);animatedTrack.properties.effects.push_back(effect);
    const auto ripple=TimelineGapClosePlan{}(animated,"sequence",{"video"},{30,10});
    expect(ripple.ok() && ripple.project && [&]{
        const auto &p=std::get<TimelineVideoTrack>(ripple.project->sequences[0].tracks[0]).properties;
        if(!p.effectTimeMap || p.effects!=animatedTrack.properties.effects || p.effectTimeMap->sourceDurationTicks!=100 ||
           p.effectTimeMap->segments.size()!=2)return false;
        const auto a=TrackEffectTimeMapper{}(p,{31,2});
        const auto b=TrackEffectTimeMapper{}(p,{31,1});
        return a.ok() && a.sourceTime==TimelineRational{31,2} && b.ok() && b.sourceTime==TimelineRational{41,1};
    }(),"gap closing preserves exact track automation by mapping shifted time to its original curve");
    const auto sub=SubsequenceExtractor{}(animated,"sequence",{40,20},"sub",SubsequenceMode::Copy,"copy");
    expect(sub.ok() && sub.project && [&]{
        const auto *seq=findTimelineSequence(*sub.project,"sub");
        if(!seq)return false;
        const auto &p=std::get<TimelineVideoTrack>(seq->tracks[0]).properties;
        const auto mapped=TrackEffectTimeMapper{}(p,{3,2});
        return p.effectTimeMap && p.effects==animatedTrack.properties.effects && mapped.ok() &&
               mapped.sourceTime==TimelineRational{83,2};
    }(),"subsequence extraction preserves the original track automation and exact source-time offset");
    auto invalidMap=animatedTrack.properties;
    invalidMap.effectTimeMap=TimelineTrackEffectTimeMap{100,{{{0,10},0},{{5,10},20}}};
    expect(!TrackEffectTimeMapper{}(invalidMap,{1,1}).ok(),
           "the mapper validates all segments before returning a matching source time");
    timelineTrackProperties(input.sequences[0].tracks[1]).locked = true;
    expect(!TimelineGapClosePlan{}(input, "sequence", {"video"}, {30, 10}).ok(),
           "a required locked sync track rejects the entire gap close");
    timelineTrackProperties(input.sequences[0].tracks[1]).locked = false;
    expect(!TimelineGapClosePlan{}(input, "sequence", {"video"}, {25, 10}).ok(),
           "gap close rejects an interval occupied by a clip");
}

void simplify() {
    auto input = project();
    std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]).clips[1].properties.enabled = false;
    TimelineAudioTrack empty;
    empty.properties.id = "empty";
    input.sequences[0].tracks.emplace_back(empty);
    SequenceSimplificationOptions options;
    options.removeDisabledClips = true;
    options.removeEmptyTracks = true;
    options.outputSequenceId = "simplified";
    auto plan = SequenceSimplificationPlan{}(input, "sequence", options);
    expect(plan.ok() && plan.project && plan.project->sequences.size() == 2 &&
           plan.project->sequences[0] == input.sequences[0] &&
           plan.project->sequences[1].id == "simplified" &&
           plan.project->sequences[1].tracks.size() == 1 &&
           std::get<TimelineVideoTrack>(plan.project->sequences[1].tracks[0]).clips.size() == 1,
           "independent simplification options may produce a new sequence while retaining the original");
    options.outputSequenceId.reset();
    options.removeDisabledClips = false;
    options.removeEmptyTracks = false;
    options.removeGaps = true;
    auto gaps = SequenceSimplificationPlan{}(input, "sequence", options);
    expect(gaps.ok() && gaps.project &&
           std::get<TimelineVideoTrack>(gaps.project->sequences[0].tracks[0]).clips[0].properties.timelineRange.start == 0 &&
           std::get<TimelineVideoTrack>(gaps.project->sequences[0].tracks[0]).clips[1].properties.timelineRange.start == 20,
           "gap simplification closes common empty intervals without deleting clips");

    auto nestedInput = project();
    auto nested = nestedInput.sequences[0];
    nested.id = "nested";
    TimelineVideoTrack parentTrack;
    parentTrack.properties.id = "parent-track";
    TimelineVideoClip parent;
    parent.properties.id = "parent";
    parent.properties.source = TimelineSequenceReference{"nested", TimelineStreamKind::Video};
    parent.properties.timelineRange = {0, 100};
    parent.properties.sourceRange = {0, 100};
    parentTrack.clips.push_back(parent);
    nestedInput.sequences[0].tracks = {parentTrack};
    nestedInput.sequences.push_back(nested);
    SequenceSimplificationOptions flatten;
    flatten.flattenNestedSequences = true;
    auto flat = SequenceSimplificationPlan{}(nestedInput, "sequence", flatten);
    expect(flat.ok() && flat.project &&
           std::get<TimelineVideoTrack>(flat.project->sequences[0].tracks[0]).clips.size() == 2 &&
           std::get<TimelineVideoTrack>(flat.project->sequences[0].tracks[0]).clips[0].properties.id == "parent:a",
           "simple neutral nesting flattens without replacing the underlying source clips");
    auto multi=nestedInput;
    auto overlay=std::get<TimelineVideoTrack>(multi.sequences[1].tracks[0]);
    overlay.properties.id="overlay";
    overlay.clips[0].properties.id="overlay-a";
    overlay.clips[1].properties.id="overlay-b";
    multi.sequences[1].tracks.emplace_back(overlay);
    TimelineAudioTrack unrelatedAudio;unrelatedAudio.properties.id="audio";
    multi.sequences[1].tracks.emplace_back(unrelatedAudio);
    const auto flattened=SequenceSimplificationPlan{}(multi,"sequence",flatten);
    expect(flattened.ok() && flattened.project && flattened.project->sequences[0].tracks.size()==2 && [&] {
        const auto &tracks=flattened.project->sequences[0].tracks;
        const auto &lower=std::get<TimelineVideoTrack>(tracks[0]);
        const auto &upper=std::get<TimelineVideoTrack>(tracks[1]);
        return lower.clips.size()==2 && upper.clips.size()==2 &&
               lower.clips[0].properties.id=="parent:a" && upper.clips[0].properties.id=="parent:overlay-a" &&
               upper.clips[0].properties.timelineRange==overlay.clips[0].properties.timelineRange;
    }(),"multi-track nested video flattens into adjacent ordered tracks while unrelated audio stays separate");
    auto withAudio=nestedInput;
    for(auto &sequence:withAudio.sequences){sequence.audioSampleRate=48000;sequence.audioChannelLayout="stereo";}
    TimelineAudioTrack nestedAudio;nestedAudio.properties.id="nested-audio";
    TimelineAudioClip audioSource;audioSource.properties.id="audio-source";
    audioSource.properties.source=TimelineGeneratedReference{"tone",TimelineStreamKind::Audio,{1,24},{}};
    audioSource.properties.timelineRange={0,100};audioSource.properties.sourceRange={0,100};
    nestedAudio.clips.push_back(audioSource);withAudio.sequences[1].tracks.emplace_back(nestedAudio);
    TimelineAudioTrack parentAudio;parentAudio.properties.id="parent-audio";
    TimelineAudioClip audioParent;audioParent.properties.id="audio-parent";
    audioParent.properties.source=TimelineSequenceReference{"nested",TimelineStreamKind::Audio};
    audioParent.properties.timelineRange={0,100};audioParent.properties.sourceRange={0,100};
    parentAudio.clips.push_back(audioParent);withAudio.sequences[0].tracks.emplace_back(parentAudio);
    const auto preserveAudio=SequenceSimplificationPlan{}(withAudio,"sequence",flatten);
    expect(preserveAudio.ok() && preserveAudio.project &&
           std::get<TimelineAudioTrack>(preserveAudio.project->sequences[0].tracks[1])==parentAudio &&
           std::get<TimelineAudioTrack>(preserveAudio.project->sequences[1].tracks[1])==nestedAudio,
           "flattening nested video retains the editable original audio tracks and references");
    std::get<TimelineVideoTrack>(nestedInput.sequences[1].tracks[0]).clips[0].transform.blendMode = "multiply";
    expect(!SequenceSimplificationPlan{}(nestedInput, "sequence", flatten).ok(),
           "flattening rejects child blend modes that depend on the outer background");
    std::get<TimelineVideoTrack>(nestedInput.sequences[1].tracks[0]).clips[0].transform.blendMode = "normal";
    nestedInput.sequences[1].color.primaries = "different";
    expect(!SequenceSimplificationPlan{}(nestedInput, "sequence", flatten).ok(),
           "flattening rejects an unrepresented color conversion between sequences");
    nestedInput.sequences[1].color = {};
    std::get<TimelineVideoTrack>(nestedInput.sequences[1].tracks[0]).clips[0].properties.syncGroupId = "sync";
    expect(!SequenceSimplificationPlan{}(nestedInput, "sequence", flatten).ok(),
           "flattening rejects sync identities that could collide with outer clips");
}

void reframe() {
    auto input = project();
    AutoReframeRequest request;
    request.effectId = "reframe";
    request.keyframes = {{0, {20, 24}, 1.5}, {20, {30, 24}, 2.0}};
    auto plan = AutoReframeKeyframeApplier{}(input, "sequence", "a", request);
    expect(plan.ok() && plan.project &&
           std::get<TimelineVideoTrack>(plan.project->sequences[0].tracks[0]).clips[0].properties.effects.size() == 1,
           "caller-provided reframe keyframes are retained as a timeline effect");
    if (plan.ok()) {
        const auto &effect = std::get<TimelineVideoTrack>(plan.project->sequences[0].tracks[0]).clips[0].properties.effects[0];
        expect(effect.automation.size() == 3 && effect.automation[0].keyframes.size() == 2,
               "reframe center and scale are stored as independently editable curves");
    }
    request.keyframes.clear();
    request.sourceExtent = {64, 64};
    request.targetExtent = {32, 64};
    request.selectedTrackId = "person";
    request.observations = {{0, "person", {10, 10}, {10, 20}}, {10, "person", {20, 10}, {10, 20}}};
    auto tracked = AutoReframeKeyframeApplier{}(input, "sequence", "a", request);
    expect(tracked.ok() && tracked.project &&
           std::get<TimelineVideoTrack>(tracked.project->sequences[0].tracks[0]).clips[0].properties.effects.size() == 1,
           "externally detected and tracked subject boxes produce reframe keyframes");
    request.selectedTrackId = "missing";
    expect(!AutoReframeKeyframeApplier{}(input, "sequence", "a", request).ok(),
           "a missing selected subject does not silently choose another subject");
    std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]).properties.locked = true;
    expect(!AutoReframeKeyframeApplier{}(input, "sequence", "a", request).ok(),
           "reframe cannot change a locked track");
}

void trackDetectedSubjects() {
    std::vector<AutoReframeDetection> detections = {
        {0, 1, "person", .9, {0, 0}, {20, 20}},
        {0, 2, "car", .8, {40, 0}, {15, 15}},
        {10, 1, "person", .85, {2, 0}, {20, 20}},
        {10, 2, "car", .7, {41, 0}, {15, 15}},
        {20, 1, "person", .1, {4, 0}, {20, 20}},
    };
    AutoReframeTrackingOptions options;
    options.idPrefix = "subject";
    options.maximumGapTicks = 20;
    options.minimumConfidence = .5;
    options.minimumOverlap = .25;
    auto tracks = AutoReframeSubjectTracker{}(detections, options);
    expect(tracks.ok() && tracks.tracks.size() == 2 && tracks.tracks[0].observations.size() == 2 &&
           tracks.tracks[0].observations[0].trackId == "subject/0" &&
           tracks.tracks[0].observations[1].origin.x == 2,
           "explicit detector results are associated across frames and low confidence objects are filtered");
    options.minimumOverlap = 2;
    expect(!AutoReframeSubjectTracker{}(detections, options).ok(), "invalid tracking policy is rejected");
    options.minimumOverlap = .25;
    struct Frame { int x; } first{0}, second{2};
    const std::vector<AutoReframeAnalysisFrame<Frame>> frames{{0, &first}, {10, &second}};
    int calls = 0;
    auto detector = [&](const Frame &frame) -> std::expected<std::vector<AutoReframeDetection>, std::string> {
        ++calls;
        return std::vector<AutoReframeDetection>{{0, 1, "person", .9, {double(frame.x), 0}, {20, 20}}};
    };
    auto analyzed = analyzeAutoReframeFrames<Frame>(frames, detector, options);
    expect(analyzed.ok() && calls == 2 && analyzed.tracks.size() == 1 && analyzed.tracks[0].observations.size() == 2,
           "external orchestration invokes the explicitly injected detector and associates its results");
    auto failingDetector = [](const Frame &) -> std::expected<std::vector<AutoReframeDetection>, std::string> {
        return std::unexpected(std::string{});
    };
    expect(!analyzeAutoReframeFrames<Frame>(frames, failingDetector, options).ok(),
           "a detector error remains an error even when its diagnostic string is empty");
}

void liftExtractAndSubsequence() {
    auto input = project();
    TimelineVideoTrack sync;
    sync.properties.id = "sync"; sync.properties.syncLocked = true;
    auto linkedClip = std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]).clips[0];
    linkedClip.properties.id = "linked";
    linkedClip.properties.linkGroupId = "group";
    std::get<TimelineVideoTrack>(input.sequences[0].tracks[0]).clips[0].properties.linkGroupId = "group";
    sync.clips.push_back(linkedClip);
    input.sequences[0].tracks.emplace_back(sync);
    input.sequences[0].linkGroups.push_back({"group", {"a", "linked"}, true});
    auto lifted = TimelineLiftPlan{}(input, "sequence", {"video"}, {15, 30}, "lift");
    expect(lifted.ok() && lifted.project &&
           std::get<TimelineVideoTrack>(lifted.project->sequences[0].tracks[0]).clips[0].properties.timelineRange == TimelineTickRange{10, 5} &&
           std::get<TimelineVideoTrack>(lifted.project->sequences[0].tracks[1]).clips[0].properties.timelineRange == TimelineTickRange{10, 5} &&
           lifted.project->sequences[0].linkGroups.size() == 1,
           "lift preserves a gap and keeps linked fragments synchronized");
    auto extracted = TimelineExtractPlan{}(input, "sequence", {"video"}, {15, 30}, "extract");
    expect(extracted.ok() && extracted.project &&
           std::get<TimelineVideoTrack>(extracted.project->sequences[0].tracks[0]).clips[1].properties.timelineRange.start == 15,
           "extract closes the lifted interval on affected tracks");
    timelineTrackProperties(input.sequences[0].tracks[1]).locked = true;
    expect(!TimelineLiftPlan{}(input, "sequence", {"video"}, {15, 30}, "lift").ok() &&
           !TimelineExtractPlan{}(input, "sequence", {"video"}, {15, 30}, "extract").ok(),
           "a required locked track rejects lift and extract");

    auto original = project();
    auto copy = SubsequenceExtractor{}(original, "sequence", {10, 20}, "sub", SubsequenceMode::Copy, "copy");
    expect(copy.ok() && copy.project && copy.project->sequences.size() == 2 &&
           copy.project->sequences[0] == original.sequences[0] && copy.project->sequences[1].durationTicks == 20 &&
           std::get<TimelineVideoTrack>(copy.project->sequences[1].tracks[0]).clips[0].properties.timelineRange.start == 0,
           "copying a subsequence retains the source and rebases its selected range");
    auto move = SubsequenceExtractor{}(original, "sequence", {10, 20}, "sub", SubsequenceMode::Move, "move");
    expect(move.ok() && move.project && move.project->sequences.size() == 2 &&
           std::get<TimelineVideoTrack>(move.project->sequences[0].tracks[0]).clips.size() == 1 &&
           std::get<TimelineVideoTrack>(move.project->sequences[0].tracks[0]).clips[0].properties.timelineRange.start == 20,
           "moving a subsequence extracts its source interval and closes the gap");
}
}

int main() {
    gaps();
    settingsAndTrackLocks();
    transitionDefaults();
    captionEdits();
    throughEdits();
    transitions();
    splitPlans();
    trimAndExtend();
    closeGaps();
    simplify();
    reframe();
    trackDetectedSubjects();
    liftExtractAndSubsequence();
    return failures ? 1 : 0;
}
