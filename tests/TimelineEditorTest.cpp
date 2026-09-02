#include <iiSharedCanvas.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <variant>

namespace {

int failures = 0;

void expect(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

iiSharedCanvas::TimelineProject makeProject()
{
    using namespace iiSharedCanvas;

    TimelineProject project;
    project.id = "editor-project";
    project.name = "Editor project";
    project.activeSequenceId = "sequence";

    TimelineVideoStream stream;
    stream.id = "video-stream";
    stream.streamIndex = 0;
    stream.timeBase = {1, 24'000};
    stream.durationTicks = 240'000;
    stream.codec.identifier = "h264";
    stream.codedExtent = {1'920, 1'080};
    stream.displayExtent = stream.codedExtent;
    stream.pixelAspectRatio = {1, 1};
    stream.timing.mode = TimelineFrameRateMode::Constant;
    stream.timing.nominal = TimelineFrameRate{24, 1};
    stream.pixelFormat = "yuv420p";
    stream.bitDepth = 8;

    TimelineMediaRepresentation representation;
    representation.id = "original";
    representation.role = TimelineMediaRepresentationRole::Original;
    representation.uri = "file:///media/source.mp4";
    representation.container.identifier = "mp4";
    representation.timeBase = {1, 24'000};
    representation.durationTicks = 240'000;
    representation.streams.emplace_back(stream);

    TimelineMediaSource source;
    source.id = "source";
    source.name = "Source";
    source.originalRepresentationId = "original";
    source.activeRepresentationId = "original";
    source.representations.push_back(representation);
    project.mediaSources.push_back(source);

    TimelineVideoClip clip;
    clip.properties.id = "clip";
    clip.properties.name = "Clip";
    clip.properties.source = TimelineMediaReference{"source", "video-stream"};
    clip.properties.timelineRange = {0, 240'000};
    clip.properties.sourceRange = {0, 240'000};
    clip.properties.playbackRate = {1, 1};

    TimelineVideoTrack track;
    track.properties.id = "video-track";
    track.properties.name = "Video";
    track.clips.push_back(clip);

    TimelineSequence sequence;
    sequence.id = "sequence";
    sequence.name = "Sequence";
    sequence.timeBase = {1, 24'000};
    sequence.editingFrameRate = {24, 1};
    sequence.durationTicks = 240'000;
    sequence.canvasExtent = TimelineExtent{1'920, 1'080};
    sequence.tracks.emplace_back(track);
    project.sequences.push_back(sequence);

    TimelineRenderProfile profile;
    profile.id = "delivery";
    profile.name = "Delivery";
    profile.sequenceId = "sequence";
    profile.container.identifier = "mp4";
    TimelineVideoOutput output;
    output.codec.identifier = "h264";
    output.extent = {1'920, 1'080};
    output.frameRate = {24, 1};
    output.pixelAspectRatio = {1, 1};
    output.pixelFormat = "yuv420p";
    output.bitDepth = 8;
    profile.video = output;
    project.renderProfiles.push_back(profile);
    return project;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    TimelineProject invalidProject;
    TimelineEditor invalidEditor;
    const TimelineEditResult invalidBind = invalidEditor.bind(invalidProject);
    expect(!invalidBind.ok()
               && invalidBind.code == TimelineEditCode::InvalidProject
               && !invalidEditor.isBound()
               && invalidEditor.revision() == 0,
           "binding an invalid project must leave the editor unbound");

    TimelineEditor unbound;
    const TimelineEditResult unboundRate = unbound.setSequenceFrameRate(
        "sequence", {30, 1});
    expect(!unboundRate.ok()
               && unboundRate.code == TimelineEditCode::NotBound,
           "an unbound timeline editor must reject mutation");

    TimelineProject project = makeProject();
    TimelineEditor editor(project);
    expect(editor.isBound()
               && editor.project() == &project
               && editor.lastResult().ok(),
           "a valid project must bind to the timeline editor");

    const std::uint64_t initialRevision = editor.revision();
    expect(editor.setSequenceFrameRate("sequence", {30'000, 1'001}).changed
               && findTimelineSequence(project, "sequence")->editingFrameRate
                    == TimelineFrameRate{30'000, 1'001}
               && editor.revision() == initialRevision + 1,
           "sequence frame rate must be atomically replaceable with an NTSC rational");
    const std::uint64_t beforeRateNoOp = editor.revision();
    expect(editor.setSequenceFrameRate("sequence", {30'000, 1'001}).ok()
               && !editor.lastResult().changed
               && editor.revision() == beforeRateNoOp,
           "identical frame-rate replacement must not advance revision");
    const TimelineEditResult badRate = editor.setSequenceFrameRate(
        "sequence", {60, 0});
    expect(!badRate.ok()
               && badRate.code == TimelineEditCode::ValidationRejected
               && findTimelineSequence(project, "sequence")
                    ->editingFrameRate.denominator == 1'001
               && editor.revision() == beforeRateNoOp,
           "invalid frame rates must roll back project state and revision");

    const TimelineEditResult rejectedRebind = editor.bind(invalidProject);
    expect(!rejectedRebind.ok()
               && rejectedRebind.code == TimelineEditCode::InvalidProject
               && editor.isBound()
               && editor.project() == &project
               && editor.revision() == beforeRateNoOp,
           "a rejected rebind must preserve the existing project and revision");

    TimelineContainerDescriptor matroska;
    matroska.identifier = "matroska";
    matroska.name = "Matroska";
    matroska.mimeType = "video/x-matroska";
    matroska.fileExtension = "mkv";
    expect(editor.setRenderContainer("delivery", matroska).changed
               && findTimelineRenderProfile(project, "delivery")
                    ->container.identifier == "matroska",
           "container format must be mutable without a closed format enum");

    TimelineCodecDescriptor av1;
    av1.identifier = "av1";
    av1.profile = "main";
    av1.level = "5.1";
    av1.implementation = "libaom-av1";
    expect(editor.setRenderVideoCodec("delivery", av1).changed
               && findTimelineRenderProfile(project, "delivery")
                    ->video->codec.identifier == "av1",
           "video codec/profile/implementation must be atomically replaceable");

    TimelineAudioOutput audioOutput;
    audioOutput.codec.identifier = "opus";
    audioOutput.sampleRate = 96'000;
    audioOutput.channelCount = 2;
    audioOutput.channelLayout = "stereo";
    findTimelineRenderProfile(project, "delivery")->audio = audioOutput;
    expect(validateTimelineProject(project).ok(),
           "a caller may still perform validated public aggregate edits");
    TimelineCodecDescriptor flac;
    flac.identifier = "flac";
    expect(editor.setRenderAudioCodec("delivery", flac).changed
               && findTimelineRenderProfile(project, "delivery")
                    ->audio->codec.identifier == "flac",
           "audio codec must remain independent from video codec");

    TimelineMediaSource unused = project.mediaSources.front();
    unused.id = "unused-source";
    unused.name = "Unused";
    unused.originalRepresentationId = "unused-original";
    unused.activeRepresentationId = "unused-original";
    unused.representations.front().id = "unused-original";
    unused.representations.front().uri = "file:///media/unused.mp4";
    const TimelineEditResult badSourceIndex = editor.insertMediaSource(unused, 99);
    expect(!badSourceIndex.ok()
               && badSourceIndex.code == TimelineEditCode::IndexOutOfRange
               && findTimelineMediaSource(project, "unused-source") == nullptr,
           "out-of-range insertion must fail before changing source storage");
    expect(editor.insertMediaSource(unused, 0).changed
               && findTimelineMediaSource(project, "unused-source") != nullptr,
           "media sources must be insertable at an explicit storage index");
    const TimelineEditResult duplicateSource = editor.insertMediaSource(unused);
    expect(!duplicateSource.ok()
               && duplicateSource.code == TimelineEditCode::DuplicateId,
           "duplicate source ids must be rejected explicitly");
    unused.name = "Renamed unused";
    expect(editor.replaceMediaSource("unused-source", unused).changed
               && findTimelineMediaSource(project, "unused-source")->name
                    == "Renamed unused",
           "complete media source technical data must be replaceable");
    expect(editor.moveMediaSource("unused-source", 1).changed
               && project.mediaSources[1].id == "unused-source",
           "media source storage order must be controllable by stable id");
    expect(editor.removeMediaSource("unused-source").changed
               && findTimelineMediaSource(project, "unused-source") == nullptr,
           "an unreferenced media source must be removable");

    const std::uint64_t beforeReferencedRemoval = editor.revision();
    const TimelineEditResult referencedRemoval = editor.removeMediaSource("source");
    expect(!referencedRemoval.ok()
               && referencedRemoval.code == TimelineEditCode::ValidationRejected
               && findTimelineMediaSource(project, "source") != nullptr
               && editor.revision() == beforeReferencedRemoval,
           "removing referenced media must fail without cascading into clips");

    TimelineVideoTrack overlay;
    overlay.properties.id = "overlay";
    overlay.properties.name = "Overlay";
    expect(editor.insertTrack("sequence", overlay).changed
               && findTimelineTrack(*findTimelineSequence(project, "sequence"),
                                    "overlay") != nullptr,
           "typed tracks must be insertable into a sequence");
    overlay.properties.name = "Edited overlay track";
    expect(editor.replaceTrack("sequence", "overlay", overlay).changed
               && timelineTrackProperties(*findTimelineTrack(
                   *findTimelineSequence(project, "sequence"), "overlay")).name
                    == "Edited overlay track"
               && editor.moveTrack("sequence", "overlay", 0).changed
               && timelineTrackProperties(
                   findTimelineSequence(project, "sequence")->tracks.front()).id
                    == "overlay"
               && editor.moveTrack("sequence", "overlay", 1).changed,
           "tracks must support validated replacement and storage-order changes");

    TimelineVideoClip overlayClip;
    overlayClip.properties.id = "overlay-clip";
    overlayClip.properties.name = "Overlay clip";
    overlayClip.properties.source = TimelineMediaReference{"source", "video-stream"};
    overlayClip.properties.timelineRange = {0, 120'000};
    overlayClip.properties.sourceRange = {0, 120'000};
    overlayClip.properties.playbackRate = {1, 1};
    expect(editor.insertClip("sequence", "overlay", overlayClip).changed
               && findTimelineClip(*findTimelineSequence(project, "sequence"),
                                   "overlay-clip") != nullptr,
           "a clip matching its owning track type must be insertable");
    TimelineVideoClip secondOverlayClip = overlayClip;
    secondOverlayClip.properties.id = "overlay-clip-2";
    secondOverlayClip.properties.name = "Second overlay clip";
    secondOverlayClip.properties.timelineRange = {120'000, 120'000};
    secondOverlayClip.properties.sourceRange = {120'000, 120'000};
    expect(editor.insertClip("sequence", "overlay", secondOverlayClip).changed
               && editor.moveClip("sequence", "overlay-clip-2", 0).changed
               && std::get<TimelineVideoTrack>(*findTimelineTrack(
                      *findTimelineSequence(project, "sequence"), "overlay"))
                      .clips.front().properties.id == "overlay-clip-2",
           "clips must support validated order changes inside their typed track");
    overlayClip.properties.name = "Edited overlay";
    expect(editor.replaceClip("sequence", "overlay-clip", overlayClip).changed
               && timelineClipProperties(*findTimelineClip(
                   *findTimelineSequence(project, "sequence"), "overlay-clip")).name
                    == "Edited overlay",
           "clip trims and metadata must be atomically replaceable");

    TimelineAudioClip wrongKind;
    wrongKind.properties = overlayClip.properties;
    wrongKind.properties.id = "wrong-kind";
    const TimelineEditResult wrongKindInsert = editor.insertClip(
        "sequence", "overlay", wrongKind);
    expect(!wrongKindInsert.ok()
               && wrongKindInsert.code == TimelineEditCode::KindMismatch
               && findTimelineClip(*findTimelineSequence(project, "sequence"),
                                   "wrong-kind") == nullptr,
           "video tracks must reject audio clips before mutation");
    expect(editor.removeClip("sequence", "overlay-clip").changed
               && editor.removeClip("sequence", "overlay-clip-2").changed
               && editor.removeTrack("sequence", "overlay").changed,
           "clips and empty tracks must be independently removable");

    TimelineSequence alternate;
    alternate.id = "alternate";
    alternate.name = "Alternate cut";
    alternate.timeBase = {1, 60'000};
    alternate.editingFrameRate = {60, 1};
    expect(editor.insertSequence(alternate).changed
               && editor.moveSequence("alternate", 0).changed
               && project.sequences.front().id == "alternate"
               && editor.setActiveSequence("alternate").changed
               && project.activeSequenceId == "alternate"
               && editor.setActiveSequence("sequence").changed,
           "multiple sequences with different time bases and FPS must be supported");
    alternate.name = "Alternate cut 2";
    expect(editor.replaceSequence("alternate", alternate).changed
               && findTimelineSequence(project, "alternate")->name
                    == "Alternate cut 2"
               && editor.removeSequence("alternate").changed,
           "unreferenced sequences must support replacement and removal");

    TimelineRenderProfile alternateProfile = project.renderProfiles.front();
    alternateProfile.id = "alternate-delivery";
    alternateProfile.name = "Alternate delivery";
    expect(editor.insertRenderProfile(alternateProfile).changed
               && editor.moveRenderProfile("alternate-delivery", 0).changed
               && project.renderProfiles.front().id == "alternate-delivery",
           "multiple output formats must be insertable");
    alternateProfile.name = "Archive delivery";
    expect(editor.replaceRenderProfile(
               "alternate-delivery", alternateProfile).changed
               && editor.removeRenderProfile("alternate-delivery").changed,
           "render profile replacement and removal must preserve other outputs");

    project.id.clear();
    const std::uint64_t beforeExternalInvalidation = editor.revision();
    const TimelineEditResult externalInvalid = editor.setSequenceFrameRate(
        "sequence", {60, 1});
    expect(!externalInvalid.ok()
               && externalInvalid.code == TimelineEditCode::InvalidProject
               && editor.revision() == beforeExternalInvalidation,
           "external invalidation must stop further edits before mutation");

    editor.unbind();
    expect(!editor.isBound() && editor.revision() == 0,
           "unbind must clear the project relationship and revision");
    return failures == 0 ? 0 : 1;
}
