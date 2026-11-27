#include <Timeline/FrameMatching.h>
#include <Timeline/SequencePlanning.h>

#include <iostream>

using namespace iiSharedCanvas;
namespace {
int failures = 0;
void expect(bool value, const char *message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
TimelineProject project() {
    TimelineVideoStream stream;
    stream.id = "video"; stream.timeBase = {1, 24}; stream.durationTicks = 24;
    stream.codec.identifier = "test"; stream.codedExtent = stream.displayExtent = {4, 4};
    stream.pixelFormat = "rgba"; stream.bitDepth = 8; stream.timing.nominal = TimelineFrameRate{24, 1};
    TimelineMediaRepresentation media;
    media.id = "original"; media.uri = "file:///source.mov"; media.container.identifier = "test";
    media.timeBase = {1, 24}; media.durationTicks = 24; media.streams.emplace_back(stream);
    TimelineMediaSource source;
    source.id = "source"; source.originalRepresentationId = source.activeRepresentationId = "original";
    source.representations.push_back(media);
    TimelineVideoClip clip;
    clip.properties.id = "a"; clip.properties.source = TimelineMediaReference{"source", "video"};
    clip.properties.timelineRange = {0, 12}; clip.properties.sourceRange = {0, 24};
    clip.properties.playbackRate = {2, 1};
    TimelineVideoTrack track;
    track.properties.id = "v"; track.clips.push_back(clip);
    clip.properties.id = "b"; clip.properties.timelineRange.start = 12; track.clips.push_back(clip);
    TimelineSequence sequence;
    sequence.id = "main"; sequence.timeBase = {1, 24}; sequence.editingFrameRate = {24, 1};
    sequence.durationTicks = 24; sequence.canvasExtent = {4, 4}; sequence.tracks.emplace_back(track);
    TimelineProject project;
    project.id = "project"; project.activeSequenceId = "main";
    project.mediaSources.push_back(source); project.sequences.push_back(sequence);
    return project;
}
}
int main() {
    auto value = project();
    expect(validateTimelineProject(value).ok(), "matching fixture is valid");
    auto forward = SourceFrameMatcher{}(value, "main", 3);
    expect(forward.ok() && forward.matches.size() == 1 && forward.matches[0].sourceFrameIndex == 6 &&
           forward.matches[0].mediaSourceId == "source" && forward.matches[0].clipPath == std::vector<std::string>{"a"},
           "double-speed sequence frame resolves to its source frame");
    auto reverseLookup = ReverseFrameMatcher{}(value, "main", "source", "video", 6, 100);
    expect(reverseLookup.ok() && reverseLookup.occurrences.size() == 2 &&
           reverseLookup.occurrences[0].sequenceFrameIndex == 3 && reverseLookup.occurrences[1].sequenceFrameIndex == 15,
           "reverse lookup returns every occurrence of a source frame");
    auto &clips = std::get<TimelineVideoTrack>(value.sequences[0].tracks[0]).clips;
    clips[0].properties.playbackRate = {-2, 1};
    forward = SourceFrameMatcher{}(value, "main", 0);
    expect(forward.ok() && forward.matches.size() == 1 && forward.matches[0].sourceFrameIndex == 23,
           "reverse playback starts on the last source frame, not the exclusive source end");
    clips[0].properties.playbackRate = {1, 1};
    clips[0].properties.timeMap = {{0, 0}, {6, 12}, {12, 24}};
    forward = SourceFrameMatcher{}(value, "main", 3);
    expect(forward.ok() && forward.matches.size() == 1 && forward.matches[0].sourceFrameIndex == 6,
           "linear variable time maps retain their source correspondence");
    auto &stream = std::get<TimelineVideoStream>(value.mediaSources[0].representations[0].streams[0]);
    stream.timing.mode = TimelineFrameRateMode::Variable;
    stream.timing.samples = {{0, {}, 3}, {3, {}, 2}, {5, {}, 3}, {8, {}, 16}};
    forward = SourceFrameMatcher{}(value, "main", 3);
    expect(forward.ok() && forward.matches.size() == 1 && forward.matches[0].sourceFrameIndex == 2,
           "variable-rate sample timing is used rather than a nominal frame-rate estimate");
    stream.timing.samples = {{0, {}, 10}, {5, {}, 4}, {9, {}, 15}};
    forward = SourceFrameMatcher{}(value, "main", 3);
    expect(forward.ok() && forward.matches.size() == 1 && forward.matches[0].sourceFrameIndex == 1,
           "a later presented frame takes precedence over an overlapping earlier sample");
    stream.timing.samples = {{0, {}, 3}, {3, {}, 2}, {5, {}, 3}, {8, {}, 16}};
    auto nested = value.sequences[0];
    nested.id = "nested";
    value.sequences.push_back(nested);
    TimelineVideoTrack parentTrack; parentTrack.properties.id = "parent-track";
    TimelineVideoClip parent; parent.properties.id = "parent";
    parent.properties.source = TimelineSequenceReference{"nested", TimelineStreamKind::Video};
    parent.properties.timelineRange = {0, 24}; parent.properties.sourceRange = {0, 24};
    parentTrack.clips.push_back(parent);
    value.sequences[0].tracks = {parentTrack};
    forward = SourceFrameMatcher{}(value, "main", 3);
    expect(forward.ok() && forward.matches.size() == 1 &&
           forward.matches[0].clipPath == std::vector<std::string>({"parent", "a"}) &&
           forward.matches[0].sourceFrameIndex == 2,
           "nested sequence mapping preserves the complete clip path");
    expect(!ReverseFrameMatcher{}(value, "main", "source", "video", 2, 1).ok(),
           "reverse search fails explicitly when its frame budget is insufficient");
    auto nestedRate = project();
    auto child = nestedRate.sequences[0]; child.id = "child"; child.editingFrameRate = {12, 1};
    nestedRate.sequences.push_back(child);
    nestedRate.sequences[0].editingFrameRate = {72, 1};
    TimelineVideoTrack rateTrack; rateTrack.properties.id = "outer";
    TimelineVideoClip rateClip; rateClip.properties.id = "nested-clip";
    rateClip.properties.source = TimelineSequenceReference{"child", TimelineStreamKind::Video};
    rateClip.properties.timelineRange = {0, 24}; rateClip.properties.sourceRange = {0, 24};
    rateTrack.clips.push_back(rateClip); nestedRate.sequences[0].tracks = {rateTrack};
    const auto previousNested = SourceFrameMatcher{}(nestedRate, "main", 4);
    expect(previousNested.ok() && previousNested.matches.size() == 1 && previousNested.matches[0].sourceFrameIndex == 0,
           "nested previous sampling resolves a child sequence frame before traversing its clips");
    std::get<TimelineVideoTrack>(nestedRate.sequences[0].tracks[0]).clips[0].properties.videoSampling = TimelineVideoSampling::Nearest;
    const auto nearestNested = SourceFrameMatcher{}(nestedRate, "main", 4);
    expect(nearestNested.ok() && nearestNested.matches.size() == 1 && nearestNested.matches[0].sourceFrameIndex == 4,
           "nested nearest sampling uses the child sequence frame rate");
    std::get<TimelineVideoTrack>(nestedRate.sequences[0].tracks[0]).clips[0].properties.videoSampling = TimelineVideoSampling::Blend;
    const auto blendedNested=SourceFrameMatcher{}(nestedRate,"main",4);
    expect(blendedNested.ok() && blendedNested.matches.size()==2 && blendedNested.matches[0].sourceFrameIndex==0 &&
           blendedNested.matches[1].sourceFrameIndex==4 && blendedNested.matches[0].interpolationPath.size()==1 &&
           blendedNested.matches[0].interpolationPath[0].clipId=="nested-clip" &&
           blendedNested.matches[0].interpolationPath[0].upperSampleTime.numerator=="2" &&
           blendedNested.matches[1].interpolationPath[0].upper,
           "nested blend retains both child-frame branches and the parent's exact interpolation interval");
    std::get<TimelineVideoTrack>(nestedRate.sequences[0].tracks[0]).clips[0].properties.videoSampling = TimelineVideoSampling::Nearest;
    const auto nestedOccurrences = ReverseFrameMatcher{}(nestedRate, "main", "source", "video", 4, 100);
    expect(nestedOccurrences.ok() && nestedOccurrences.occurrences.size() == 12 &&
           nestedOccurrences.occurrences[0].sequenceFrameIndex == 4 &&
           nestedOccurrences.occurrences[5].sequenceFrameIndex == 9 &&
           nestedOccurrences.occurrences[6].sequenceFrameIndex == 40,
           "reverse matching includes every repeated nested frame selected by the parent");
    std::get<TimelineVideoTrack>(nestedRate.sequences[0].tracks[0]).clips[0].properties.playbackRate = {-1, 1};
    const auto reversedNested = SourceFrameMatcher{}(nestedRate, "main", 0);
    expect(reversedNested.ok() && reversedNested.matches.size() == 1 && reversedNested.matches[0].sourceFrameIndex == 20,
           "a reversed nested sequence begins at its last included child frame");
    auto unsupported = project();
    auto &unsupportedClip = std::get<TimelineVideoTrack>(unsupported.sequences[0].tracks[0]).clips[0];
    unsupported.sequences[0].editingFrameRate = {72, 1};
    unsupportedClip.properties.videoSampling = TimelineVideoSampling::Nearest;
    const auto nearest = SourceFrameMatcher{}(unsupported, "main", 1);
    expect(nearest.ok() && nearest.matches.size() == 1 && nearest.matches[0].sourceFrameIndex == 1,
           "nearest sampling resolves a fractional source position to the nearest presentation frame");
    unsupportedClip.properties.playbackRate = {-2, 1};
    const auto nearestReverse = SourceFrameMatcher{}(unsupported, "main", 0);
    expect(nearestReverse.ok() && nearestReverse.matches.size() == 1 && nearestReverse.matches[0].sourceFrameIndex == 23,
           "nearest reverse playback respects the exclusive source endpoint");
    auto edge = project();
    edge.sequences[0].editingFrameRate = {72, 1};
    auto &edgeClips = std::get<TimelineVideoTrack>(edge.sequences[0].tracks[0]).clips;
    edgeClips.resize(1);
    edgeClips[0].properties.timelineRange.duration = edgeClips[0].properties.sourceRange.duration = 20;
    edgeClips[0].properties.playbackRate = {1, 1};
    edgeClips[0].properties.videoSampling = TimelineVideoSampling::Nearest;
    const auto nearestEdge = SourceFrameMatcher{}(edge, "main", 59);
    expect(nearestEdge.ok() && nearestEdge.matches.size() == 1 && nearestEdge.matches[0].sourceFrameIndex == 19,
           "nearest sampling does not select a constant-rate frame beyond the clip source end");
    auto &edgeStream = std::get<TimelineVideoStream>(edge.mediaSources[0].representations[0].streams[0]);
    edgeStream.timing.mode = TimelineFrameRateMode::Variable;
    edgeStream.timing.samples = {{0, {}, 8}, {8, {}, 8}, {16, {}, 4}, {20, {}, 4}};
    edgeClips[0].properties.timelineRange.duration = edgeClips[0].properties.sourceRange.duration = 20;
    const auto nearestVariableEdge = SourceFrameMatcher{}(edge, "main", 59);
    expect(nearestVariableEdge.ok() && nearestVariableEdge.matches.size() == 1 &&
           nearestVariableEdge.matches[0].sourceFrameIndex == 2,
           "nearest sampling does not select a variable-rate frame beyond the clip source end");
    unsupportedClip.properties.playbackRate = {2, 1};
    unsupportedClip.properties.videoSampling = TimelineVideoSampling::Blend;
    const auto blended = SourceFrameMatcher{}(unsupported, "main", 1);
    expect(blended.ok() && blended.matches.size() == 2 &&
           blended.matches[0].sourceFrameIndex == 0 && blended.matches[1].sourceFrameIndex == 1 &&
           blended.matches[0].interpolationPath.size() == 1 && blended.matches[1].interpolationPath.size() == 1 &&
           blended.matches[0].interpolationPath[0].mode == TimelineVideoSampling::Blend &&
           !blended.matches[0].interpolationPath[0].upper && blended.matches[1].interpolationPath[0].upper &&
           blended.matches[0].interpolationPath[0].lowerSampleTime.numerator == "0" &&
           blended.matches[0].interpolationPath[0].upperSampleTime.numerator == "1" &&
           blended.matches[0].sourcePosition.numerator == 2 && blended.matches[0].sourcePosition.denominator == 3,
           "blended sampling returns both source frames and their exact interpolation interval");
    unsupportedClip.properties.videoSampling = TimelineVideoSampling::OpticalFlow;
    const auto flow = SourceFrameMatcher{}(unsupported, "main", 1);
    expect(flow.ok() && flow.matches.size() == 2 &&
           flow.matches[0].interpolationPath[0].mode == TimelineVideoSampling::OpticalFlow &&
           flow.matches[1].interpolationPath[0].upper,
           "optical-flow sampling returns both source dependencies and the interpolation mode");
    const auto flowReverse = ReverseFrameMatcher{}(unsupported, "main", "source", "video", 1, 100);
    expect(flowReverse.ok() && !flowReverse.occurrences.empty() &&
           flowReverse.occurrences[0].sequenceFrameIndex == 1,
           "reverse lookup includes a frame used as an interpolation endpoint");
    auto variableBlend=project();variableBlend.sequences[0].editingFrameRate={72,1};
    auto &variableClip=std::get<TimelineVideoTrack>(variableBlend.sequences[0].tracks[0]).clips[0];
    variableClip.properties.videoSampling=TimelineVideoSampling::Blend;
    auto &variableStream=std::get<TimelineVideoStream>(variableBlend.mediaSources[0].representations[0].streams[0]);
    variableStream.timing.mode=TimelineFrameRateMode::Variable;
    variableStream.timing.samples={{0,{},3},{3,{},2},{5,{},3},{8,{},16}};
    const auto vfrBlend=SourceFrameMatcher{}(variableBlend,"main",4);
    expect(vfrBlend.ok() && vfrBlend.matches.size()==2 && vfrBlend.matches[0].sourceFrameIndex==0 &&
           vfrBlend.matches[1].sourceFrameIndex==1 &&
           vfrBlend.matches[0].interpolationPath[0].upperSampleTime.numerator=="3",
           "interpolation uses actual neighboring VFR presentation times");
    unsupportedClip.properties.videoSampling = TimelineVideoSampling::Previous;
    unsupportedClip.properties.playbackRate = {1, 1};
    unsupportedClip.properties.timeMap = {{0, 0, TimelineKeyframeInterpolation::Bezier}, {12, 24}};
    const auto bezier = SourceFrameMatcher{}(unsupported, "main", 3);
    expect(bezier.ok() && bezier.matches.size() == 1 && bezier.matches[0].sourceFrameIndex == 2 &&
           bezier.matches[0].sourcePositionBezier, "Bezier maps return an exact source-time expression");
    auto exactCurve = project();
    exactCurve.sequences[0].editingFrameRate = {72, 1};
    auto &curved = std::get<TimelineVideoTrack>(exactCurve.sequences[0].tracks[0]).clips[0];
    curved.properties.playbackRate = {1, 1};
    curved.properties.timeMap = {
        {0, 0, TimelineKeyframeInterpolation::Bezier, {}, {0., 8.}},
        {12, 24, TimelineKeyframeInterpolation::Linear, {-12., -8.}, {}}
    };
    const auto irrational = SourceFrameMatcher{}(exactCurve, "main", 1);
    expect(irrational.ok() && irrational.matches.size() == 1 && irrational.matches[0].sourceFrameIndex == 7 &&
           irrational.matches[0].sourcePositionBezier && irrational.matches[0].sourcePosition.denominator == 0 &&
           irrational.matches[0].sourcePositionBezier->target.numerator == "1" &&
           irrational.matches[0].sourcePositionBezier->target.denominator == "3" &&
           irrational.matches[0].sourcePositionBezier->timeControl[3].numerator == "12" &&
           irrational.matches[0].sourcePositionBezier->valueControl[1].numerator == "8",
           "irrational Bezier source time retains exact controls and selects the correct source frame");
    const auto occurrences = ReverseFrameMatcher{}(exactCurve,"main","source","video",7,100);
    expect(occurrences.ok() && !occurrences.occurrences.empty() && occurrences.occurrences[0].sequenceFrameIndex == 1,
           "reverse frame matching includes positions reached by an exact Bezier map");
    auto boundaryCurve = exactCurve;
    boundaryCurve.sequences[0].editingFrameRate = {48, 1};
    const auto exactBoundary = SourceFrameMatcher{}(boundaryCurve, "main", 3);
    expect(exactBoundary.ok() && exactBoundary.matches.size() == 1 &&
           exactBoundary.matches[0].sourceFrameIndex == 12,
           "an exact algebraic intersection on a frame boundary selects that frame");
    auto monotoneUnordered = exactCurve;
    auto &unorderedPoints = std::get<TimelineVideoTrack>(monotoneUnordered.sequences[0].tracks[0]).clips[0].properties.timeMap;
    unorderedPoints[0].outgoing.timeOffset = 12.;
    unorderedPoints[1].incoming.timeOffset = -12.;
    expect(validateTimelineProject(monotoneUnordered).ok(),
           "a monotone time curve remains valid even when its inner controls are not ordered");
    auto turning = exactCurve;
    auto &turningPoints = std::get<TimelineVideoTrack>(turning.sequences[0].tracks[0]).clips[0].properties.timeMap;
    turningPoints[0].outgoing.timeOffset = 24.;
    turningPoints[1].incoming.timeOffset = -24.;
    expect(!validateTimelineProject(turning).ok(),
           "a time curve whose derivative is negative only inside the segment is rejected");
    auto nonphysical = exactCurve;
    std::get<TimelineVideoTrack>(nonphysical.sequences[0].tracks[0]).clips[0].properties.timeMap[0].outgoing.timeOffset = -1.;
    expect(!validateTimelineProject(nonphysical).ok(),
           "a Bezier time axis that moves backwards is rejected as nonphysical");
    auto looping = project();
    auto &loopClip=std::get<TimelineVideoTrack>(looping.sequences[0].tracks[0]).clips[0];
    loopClip.properties.loopMode=TimelineLoopMode::PingPong;
    loopClip.properties.sourceRange.duration=8;
    const auto split=TimelineSplitPlan{}(looping,"main",{{"a","a-right"}},{},5);
    bool preserved=split.ok();
    for(std::uint64_t frame=0;frame<12 && preserved;++frame){
        const auto before=SourceFrameMatcher{}(looping,"main",frame);
        const auto after=SourceFrameMatcher{}(*split.project,"main",frame);
        preserved=before.ok() && after.ok() && before.matches.size()==1 && after.matches.size()==1 &&
                  before.matches[0].sourceFrameIndex==after.matches[0].sourceFrameIndex;
    }
    expect(preserved,"splitting a ping-pong loop retains every original source-frame mapping");
    auto extendedLoop=looping;
    auto &extendedClips=std::get<TimelineVideoTrack>(extendedLoop.sequences[0].tracks[0]).clips;
    extendedClips.resize(1);
    extendedClips[0].properties.timelineRange.start=4;
    const auto extension=TimelineExtendToPlayheadPlan{}(extendedLoop,"main",{"a"},TimelineClipEdge::Start,2);
    bool extensionPreserved=extension.ok();
    for(std::uint64_t frame=4;frame<16 && extensionPreserved;++frame){
        const auto before=SourceFrameMatcher{}(extendedLoop,"main",frame);
        const auto after=SourceFrameMatcher{}(*extension.project,"main",frame);
        extensionPreserved=before.ok() && after.ok() && before.matches.size()==1 && after.matches.size()==1 &&
                           before.matches[0].sourceFrameIndex==after.matches[0].sourceFrameIndex;
    }
    expect(extensionPreserved,"extending a loop backward preserves the phase of every pre-existing frame");
    auto mappedLoop=project();
    auto &mappedLoopClip=std::get<TimelineVideoTrack>(mappedLoop.sequences[0].tracks[0]).clips[0];
    mappedLoopClip.properties.playbackRate={1,1};
    mappedLoopClip.properties.sourceRange.duration=8;
    mappedLoopClip.properties.loopMode=TimelineLoopMode::Repeat;
    mappedLoopClip.properties.timeMap={{0,0},{12,24}};
    const auto linearLoop=SourceFrameMatcher{}(mappedLoop,"main",7);
    expect(linearLoop.ok() && linearLoop.matches.size()==1 && linearLoop.matches[0].sourceFrameIndex==6,
           "linear time maps wrap into a repeating clip source range");
    mappedLoopClip.properties.loopMode=TimelineLoopMode::PingPong;
    const auto pingPongMap=SourceFrameMatcher{}(mappedLoop,"main",7);
    expect(pingPongMap.ok() && pingPongMap.matches.size()==1 && pingPongMap.matches[0].sourceFrameIndex==1,
           "time maps reflect the source coordinate in odd ping-pong cycles");
    mappedLoop.sequences[0].editingFrameRate={72,1};
    mappedLoopClip.properties.loopMode=TimelineLoopMode::Repeat;
    mappedLoopClip.properties.timeMap={{0,0,TimelineKeyframeInterpolation::Bezier,{},{0.,8.}},
                                     {12,24,TimelineKeyframeInterpolation::Linear,{-12.,-8.},{}}};
    const auto bezierLoop=SourceFrameMatcher{}(mappedLoop,"main",2);
    expect(bezierLoop.ok() && bezierLoop.matches.size()==1 && bezierLoop.matches[0].sourceFrameIndex==1 &&
           bezierLoop.matches[0].sourcePositionBezier,
           "a repeated irrational Bezier time retains an exact wrapped expression");
    return failures ? 1 : 0;
}
