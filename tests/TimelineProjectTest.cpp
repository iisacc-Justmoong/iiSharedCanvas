#include <iiSharedCanvas.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
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

bool contains(const iiSharedCanvas::TimelineValidationResult &result,
              iiSharedCanvas::TimelineValidationCode code)
{
    for (const auto &issue : result.issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

iiSharedCanvas::TimelineProject makeProject()
{
    using namespace iiSharedCanvas;

    TimelineProject project;
    project.id = "edit-project";
    project.name = "Feature edit";
    project.applicationName = "Reference NLE";
    project.applicationVersion = "1.0";
    project.createdAt = "2026-09-02T12:00:00+09:00";
    project.modifiedAt = project.createdAt;
    project.activeSequenceId = "main";
    project.metadata.push_back({"production", std::string{"iisacc"}});

    TimelineMediaSource camera;
    camera.id = "camera-a";
    camera.name = "A camera";
    camera.originalRepresentationId = "camera-original";
    camera.activeRepresentationId = "camera-original";

    TimelineMediaRepresentation original;
    original.id = "camera-original";
    original.role = TimelineMediaRepresentationRole::Original;
    original.uri = "file:///media/a-camera.mov";
    original.container.identifier = "quicktime";
    original.container.name = "QuickTime / MOV";
    original.container.mimeType = "video/quicktime";
    original.container.fileExtension = "mov";
    original.container.majorBrand = "qt";
    original.container.options.push_back({"fastStart", true});
    original.fileSizeBytes = 8'000'000U;
    original.checksumAlgorithm = "sha256";
    original.checksum = "0123456789abcdef";
    original.durationTicks = 600'000;
    original.timeBase = {1, 60'000};
    original.timecode = TimelineTimecode{
        0, {30'000, 1'001}, 30, TimelineTimecodeCounting::DropFrame, true};

    TimelineVideoStream video;
    video.id = "v0";
    video.streamIndex = 0;
    video.timeBase = {1, 60'000};
    video.startTicks = 0;
    video.durationTicks = 600'000;
    video.codec.identifier = "prores";
    video.codec.name = "Apple ProRes";
    video.codec.profile = "422 HQ";
    video.codec.tag = "apch";
    video.codec.bitRate = 220'000'000U;
    video.codedExtent = {1'920, 1'080};
    video.displayExtent = {1'920, 1'080};
    video.pixelAspectRatio = {1, 1};
    video.timing.mode = TimelineFrameRateMode::Variable;
    video.timing.nominal = TimelineFrameRate{30'000, 1'001};
    video.timing.average = TimelineFrameRate{30'000, 1'001};
    video.timing.minimum = TimelineFrameRate{24'000, 1'001};
    video.timing.maximum = TimelineFrameRate{60'000, 1'001};
    video.timing.samples = {
        {0, std::nullopt, 2'002, true, std::nullopt, std::nullopt},
        {2'002, std::nullopt, 2'000, false, std::nullopt, std::nullopt},
    };
    video.pixelFormat = "yuv422p10le";
    video.chromaSubsampling = "4:2:2";
    video.bitDepth = 10;
    video.scanMode = TimelineScanMode::Progressive;
    video.fieldOrder = TimelineFieldOrder::None;
    video.alphaMode = TimelineAlphaMode::None;
    video.color.primaries = "bt2020";
    video.color.transfer = "smpte2084";
    video.color.matrix = "bt2020nc";
    video.color.range = TimelineColorRange::Limited;
    video.color.masteringDisplay = TimelineMasteringDisplay{
        {0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}, {0.3127, 0.3290},
        0.0001, 1'000.0};
    video.color.contentLight = TimelineContentLight{1'000.0, 400.0};
    video.rotationDegrees = 0;
    video.language = "und";
    video.dispositions = {"default"};
    video.metadata.push_back({"cameraAngle", std::string{"A"}});

    TimelineAudioStream audio;
    audio.id = "a0";
    audio.streamIndex = 1;
    audio.timeBase = {1, 48'000};
    audio.startTicks = 0;
    audio.durationTicks = 480'000;
    audio.codec.identifier = "pcm_s24le";
    audio.codec.name = "PCM 24-bit little-endian";
    audio.sampleRate = 48'000;
    audio.channelCount = 2;
    audio.channelLayout = "stereo";
    audio.sampleFormat = "s32";
    audio.bitDepth = 24;
    audio.language = "en-US";
    audio.loudness.integratedLufs = -23.0;
    audio.loudness.truePeakDbtp = -1.0;

    TimelineSubtitleStream captions;
    captions.id = "s0";
    captions.streamIndex = 2;
    captions.timeBase = {1, 1'000};
    captions.durationTicks = 10'000;
    captions.codec.identifier = "webvtt";
    captions.format = "WebVTT";
    captions.language = "en-US";
    captions.hearingImpaired = true;
    TimelineSubtitleCue openingCue;
    openingCue.id = "opening-cue";
    openingCue.range = {0, 1'000};
    openingCue.text = "Opening";
    captions.cues.push_back(openingCue);

    TimelineDataStream metadata;
    metadata.id = "d0";
    metadata.streamIndex = 3;
    metadata.timeBase = {1, 90'000};
    metadata.durationTicks = 900'000;
    metadata.codec.identifier = "scte_35";
    metadata.kind = "timed-metadata";

    original.streams.emplace_back(video);
    original.streams.emplace_back(audio);
    original.streams.emplace_back(captions);
    original.streams.emplace_back(metadata);
    original.attachments.push_back({
        "caption-image", "Caption image", "image/png",
        "file:///media/caption-image.png", {}});
    camera.representations.push_back(original);

    TimelineMediaRepresentation proxy = original;
    proxy.id = "camera-proxy";
    proxy.role = TimelineMediaRepresentationRole::Proxy;
    proxy.uri = "file:///proxy/a-camera-proxy.mp4";
    proxy.container.identifier = "mp4";
    proxy.container.name = "MPEG-4 Part 14";
    proxy.container.mimeType = "video/mp4";
    proxy.container.fileExtension = "mp4";
    proxy.fileSizeBytes = 800'000U;
    proxy.checksum.clear();
    std::get<TimelineVideoStream>(proxy.streams[0]).codec.identifier = "h264";
    std::get<TimelineVideoStream>(proxy.streams[0]).codec.profile = "proxy";
    std::get<TimelineAudioStream>(proxy.streams[1]).codec.identifier = "aac";
    camera.representations.push_back(proxy);
    project.mediaSources.push_back(camera);

    TimelineSequence sequence;
    sequence.id = "main";
    sequence.name = "Main sequence";
    sequence.timeBase = {1, 60'000};
    sequence.editingFrameRate = {30'000, 1'001};
    sequence.startTicks = 0;
    sequence.durationTicks = 600'000;
    sequence.workArea = TimelineTickRange{60'000, 480'000};
    sequence.canvasExtent = TimelineExtent{1'920, 1'080};
    sequence.pixelAspectRatio = {1, 1};
    sequence.color = video.color;
    sequence.audioSampleRate = 48'000;
    sequence.audioChannelLayout = "stereo";
    sequence.timecode = original.timecode;

    TimelineVideoClip firstVideo;
    firstVideo.properties.id = "video-1";
    firstVideo.properties.name = "Opening";
    firstVideo.properties.source = TimelineMediaReference{"camera-a", "v0"};
    firstVideo.properties.timelineRange = {0, 300'000};
    firstVideo.properties.sourceRange = {0, 300'000};
    firstVideo.properties.playbackRate = {1, 1};
    firstVideo.properties.linkGroupId = "av-link-1";
    firstVideo.properties.role = "dialogue-picture";
    firstVideo.transform.position = {960.0, 540.0};
    firstVideo.transform.anchor = {0.5, 0.5};
    firstVideo.transform.scale = {1.0, 1.0};
    firstVideo.transform.opacity = 1.0;
    firstVideo.crop = {0.0, 0.0, 0.0, 0.0, 0.0};
    firstVideo.properties.timeMap = {
        {0, 0, TimelineKeyframeInterpolation::Linear, {}, {}},
        {300'000, 240'000, TimelineKeyframeInterpolation::Bezier,
         {-0.2, -0.2}, {0.2, 0.2}},
    };
    TimelineEffect grade;
    grade.id = "grade-1";
    grade.name = "Color grade";
    grade.category = "color";
    grade.pluginId = "builtin.color-primary";
    grade.parameters.push_back({"exposure", 0.0});
    TimelineAutomationCurve exposure;
    exposure.parameterId = "exposure";
    exposure.defaultValue = 0.0;
    exposure.keyframes = {
        {0, 0.0, TimelineKeyframeInterpolation::Linear, {}, {}},
        {300'000, 1.0, TimelineKeyframeInterpolation::Bezier,
         {-0.2, 0.0}, {0.2, 0.0}},
    };
    grade.automation.push_back(exposure);
    firstVideo.properties.effects.push_back(grade);
    firstVideo.properties.markers.push_back({
        "clip-note", "Focus check", TimelineMarkerKind::Comment,
        {120'000, 0}, {1.0, 0.5, 0.0, 1.0}, "Review focus", "editor",
        project.createdAt, {"review"}, {}});

    TimelineVideoClip secondVideo = firstVideo;
    secondVideo.properties.id = "video-2";
    secondVideo.properties.name = "Second shot";
    secondVideo.properties.timelineRange = {300'000, 300'000};
    secondVideo.properties.sourceRange = {300'000, 300'000};
    secondVideo.properties.linkGroupId.clear();
    secondVideo.properties.timeMap.clear();
    secondVideo.properties.markers.clear();
    secondVideo.properties.effects.clear();

    TimelineVideoTrack videoTrack;
    videoTrack.properties.id = "v1";
    videoTrack.properties.name = "Video 1";
    videoTrack.properties.role = "primary-storyline";
    videoTrack.properties.syncLocked = true;
    videoTrack.properties.blendMode = "normal";
    videoTrack.clips = {firstVideo, secondVideo};
    TimelineTransition dissolve;
    dissolve.id = "dissolve-1";
    dissolve.name = "Cross dissolve";
    dissolve.typeId = "builtin.cross-dissolve";
    dissolve.fromClipId = "video-1";
    dissolve.toClipId = "video-2";
    dissolve.timelineRange = {270'000, 60'000};
    dissolve.alignment = TimelineTransitionAlignment::CenteredOnCut;
    dissolve.parameters.push_back({"curve", std::string{"equal-power"}});
    videoTrack.transitions.push_back(dissolve);

    TimelineAudioClip audioClip;
    audioClip.properties.id = "audio-1";
    audioClip.properties.name = "Dialogue";
    audioClip.properties.source = TimelineMediaReference{"camera-a", "a0"};
    audioClip.properties.timelineRange = {0, 300'000};
    audioClip.properties.sourceRange = {0, 240'000};
    audioClip.properties.linkGroupId = "av-link-1";
    audioClip.mix.gainDb = -3.0;
    audioClip.mix.pan = 0.0;
    audioClip.mix.fadeInTicks = 2'000;
    audioClip.mix.fadeOutTicks = 2'000;
    audioClip.mix.channelMatrix = {1.0, 0.0, 0.0, 1.0};

    TimelineAudioTrack audioTrack;
    audioTrack.properties.id = "a1";
    audioTrack.properties.name = "Dialogue";
    audioTrack.properties.role = "dialogue";
    audioTrack.properties.outputBusId = "master";
    audioTrack.clips.push_back(audioClip);

    TimelineSubtitleClip subtitleClip;
    subtitleClip.properties.id = "subtitle-1";
    subtitleClip.properties.name = "Opening caption";
    subtitleClip.properties.source = TimelineMediaReference{"camera-a", "s0"};
    subtitleClip.properties.timelineRange = {60'000, 120'000};
    subtitleClip.properties.sourceRange = {1'000, 2'000};
    subtitleClip.text = "A new beginning";
    subtitleClip.style.fontFamily = "Noto Sans";
    subtitleClip.style.fontSize = 48.0;
    subtitleClip.style.fill = {1.0, 1.0, 1.0, 1.0};
    subtitleClip.style.outline = {0.0, 0.0, 0.0, 1.0};
    subtitleClip.style.outlineWidth = 2.0;
    subtitleClip.style.horizontalAlignment = "center";
    subtitleClip.style.verticalAlignment = "bottom";

    TimelineSubtitleTrack subtitleTrack;
    subtitleTrack.properties.id = "s1";
    subtitleTrack.properties.name = "English captions";
    subtitleTrack.properties.language = "en-US";
    subtitleTrack.clips.push_back(subtitleClip);

    TimelineDataClip dataClip;
    dataClip.properties.id = "data-1";
    dataClip.properties.name = "Timed metadata";
    dataClip.properties.source = TimelineMediaReference{"camera-a", "d0"};
    dataClip.properties.timelineRange = {0, 60'000};
    dataClip.properties.sourceRange = {0, 90'000};
    dataClip.properties.playbackRate = {1, 1};
    dataClip.format = "application/x-scte35";
    dataClip.payload = {0xfcU};

    TimelineDataTrack dataTrack;
    dataTrack.properties.id = "d1";
    dataTrack.properties.name = "Timed metadata";
    dataTrack.clips.push_back(dataClip);
    TimelineDataClip generatedData = dataClip;
    generatedData.properties.id = "generated-data";
    generatedData.properties.name = "Generated clock";
    TimelineGeneratedReference generator;
    generator.generatorId = "builtin.clock";
    generator.kind = TimelineStreamKind::Data;
    generator.timeBase = {1, 1'000};
    generator.parameters.push_back({"timezone", std::string{"UTC"}});
    generatedData.properties.source = generator;
    generatedData.properties.timelineRange = {60'000, 60'000};
    generatedData.properties.sourceRange = {0, 1'000};
    generatedData.payload.clear();
    generatedData.format = "application/json";
    dataTrack.clips.push_back(generatedData);

    sequence.tracks.emplace_back(videoTrack);
    sequence.tracks.emplace_back(audioTrack);
    sequence.tracks.emplace_back(subtitleTrack);
    sequence.tracks.emplace_back(dataTrack);
    sequence.markers.push_back({
        "chapter-1", "Opening", TimelineMarkerKind::Chapter,
        {0, 0}, {0.2, 0.6, 1.0, 1.0}, "Opening chapter", "editor",
        project.createdAt, {"chapter"}, {}});
    sequence.linkGroups.push_back({"av-link-1", {"video-1", "audio-1"}, true});
    project.sequences.push_back(sequence);

    TimelineBin bin;
    bin.id = "rushes";
    bin.name = "Rushes";
    bin.mediaSourceIds = {"camera-a"};
    bin.sequenceIds = {"main"};
    project.bins.push_back(bin);

    TimelineRenderProfile web;
    web.id = "web-h264";
    web.name = "Web H.264";
    web.sequenceId = "main";
    web.outputUri = "file:///exports/main.mp4";
    web.container.identifier = "mp4";
    web.container.name = "MPEG-4 Part 14";
    web.container.mimeType = "video/mp4";
    web.container.fileExtension = "mp4";
    web.container.options.push_back({"fastStart", true});
    TimelineVideoOutput webVideo;
    webVideo.codec.identifier = "h264";
    webVideo.codec.profile = "high";
    webVideo.codec.level = "4.2";
    webVideo.extent = {1'920, 1'080};
    webVideo.frameRate = {30'000, 1'001};
    webVideo.frameRateMode = TimelineFrameRateMode::Constant;
    webVideo.pixelAspectRatio = {1, 1};
    webVideo.pixelFormat = "yuv420p";
    webVideo.bitDepth = 8;
    webVideo.color.primaries = "bt709";
    webVideo.color.transfer = "bt709";
    webVideo.color.matrix = "bt709";
    webVideo.rateControl = TimelineRateControl::VariableBitrate;
    webVideo.averageBitRate = 12'000'000U;
    webVideo.maximumBitRate = 20'000'000U;
    webVideo.quality = 20.0;
    webVideo.gopSize = 60;
    webVideo.maximumBFrames = 3;
    webVideo.twoPass = true;
    webVideo.hardwareEncoder = "videotoolbox";
    web.video = webVideo;
    TimelineAudioOutput webAudio;
    webAudio.codec.identifier = "aac";
    webAudio.sampleRate = 48'000;
    webAudio.channelCount = 2;
    webAudio.channelLayout = "stereo";
    webAudio.sampleFormat = "fltp";
    webAudio.bitRate = 320'000U;
    webAudio.normalizeLoudness = true;
    webAudio.targetIntegratedLufs = -14.0;
    web.audio = webAudio;
    web.metadata.push_back({"copyright", std::string{"iisacc"}});
    project.renderProfiles.push_back(web);

    TimelineRenderProfile master = web;
    master.id = "master-prores";
    master.name = "ProRes master";
    master.outputUri = "file:///exports/main-master.mov";
    master.container.identifier = "quicktime";
    master.container.fileExtension = "mov";
    master.video->codec.identifier = "prores";
    master.video->codec.profile = "4444 XQ";
    master.video->extent = {3'840, 2'160};
    master.video->frameRate = {24, 1};
    master.video->pixelFormat = "yuva444p12le";
    master.video->bitDepth = 12;
    master.video->rateControl = TimelineRateControl::ConstantQuality;
    master.video->averageBitRate.reset();
    master.video->maximumBitRate.reset();
    master.audio->codec.identifier = "pcm_s24le";
    master.audio->bitRate.reset();
    project.renderProfiles.push_back(master);
    return project;
}

} // namespace

int main()
{
    using namespace iiSharedCanvas;

    TimelineProject project = makeProject();
    expect(validateTimelineProject(project).ok(),
           "a mixed video/audio/subtitle edit with two output formats must validate");

    expect(findTimelineMediaSource(project, "camera-a") != nullptr
               && findTimelineSequence(project, "main") != nullptr
               && findTimelineRenderProfile(project, "web-h264") != nullptr,
           "project objects must resolve by stable id");
    expect(findTimelineMediaRepresentation(
               *findTimelineMediaSource(project, "camera-a"), "camera-proxy") != nullptr
               && findTimelineMediaStream(
                   *findTimelineMediaSource(project, "camera-a"), "v0")
               != nullptr
               && timelineStreamKind(*findTimelineMediaStream(
                   *findTimelineMediaSource(project, "camera-a"), "a0"))
                    == TimelineStreamKind::Audio,
           "media streams must expose variant kind and stable-id lookup");
    expect(findTimelineTrack(*findTimelineSequence(project, "main"), "v1") != nullptr
               && findTimelineClip(*findTimelineSequence(project, "main"), "video-1")
                    != nullptr
               && timelineClipKind(findTimelineClip(
                      *findTimelineSequence(project, "main"), "data-1"))
                    == TimelineStreamKind::Data
               && findTimelineDataClip(
                      *findTimelineSequence(project, "main"), "data-1") != nullptr
               && findTimelineClip(
                      *findTimelineSequence(project, "main"), "missing") == nullptr,
           "tracks and clips must be discoverable independently of vector relocation");
    expect(!timelineClipKind(findTimelineClip(
                *findTimelineSequence(project, "main"), "missing")).has_value(),
           "an empty clip view must not be misclassified as a data clip");
    const std::optional<double> tenSeconds = timelineTicksToSeconds(
        600'000, {1, 60'000});
    expect(tenSeconds && std::abs(*tenSeconds - 10.0) < 0.000001,
           "signed ticks and rational time base must convert without fixed FPS assumptions");

    for (const TimelineFrameRate rate : {
             TimelineFrameRate{24, 1},
             TimelineFrameRate{30, 1},
             TimelineFrameRate{60, 1},
             TimelineFrameRate{24'000, 1'001},
             TimelineFrameRate{30'000, 1'001},
             TimelineFrameRate{60'000, 1'001},
         }) {
        TimelineProject changed = project;
        changed.sequences.front().editingFrameRate = rate;
        changed.renderProfiles.front().video->frameRate = rate;
        expect(validateTimelineProject(changed).ok(),
               "integer and NTSC rational edit/output frame rates must remain data-driven");
    }

    TimelineProject changedFormat = project;
    changedFormat.renderProfiles.front().container.identifier = "matroska";
    changedFormat.renderProfiles.front().container.fileExtension = "mkv";
    changedFormat.renderProfiles.front().video->codec.identifier = "av1";
    changedFormat.renderProfiles.front().audio->codec.identifier = "opus";
    changedFormat.renderProfiles.front().audio->sampleRate = 96'000;
    expect(validateTimelineProject(changedFormat).ok(),
           "container, video/audio codec, and sample rate must accept new input values");

    TimelineProject badProxyTiming = project;
    std::get<TimelineVideoStream>(
        badProxyTiming.mediaSources.front().representations[1].streams.front())
        .timeBase = {1, 30'000};
    expect(contains(validateTimelineProject(badProxyTiming),
                    TimelineValidationCode::InvalidRepresentation),
           "alternate representations must preserve logical stream timing");

    TimelineProject badRate = project;
    badRate.sequences.front().editingFrameRate.denominator = 0;
    expect(contains(validateTimelineProject(badRate),
                    TimelineValidationCode::InvalidFrameRate),
           "zero frame-rate denominators must fail");

    TimelineProject badVfr = project;
    auto &badVideo = std::get<TimelineVideoStream>(
        badVfr.mediaSources.front().representations.front().streams.front());
    badVideo.timing.minimum = TimelineFrameRate{60, 1};
    badVideo.timing.maximum = TimelineFrameRate{24, 1};
    expect(contains(validateTimelineProject(badVfr),
                    TimelineValidationCode::InvalidFrameRate),
           "VFR minimum and maximum rates must remain ordered");

    TimelineProject badVfrSample = project;
    auto &badSamples = std::get<TimelineVideoStream>(
        badVfrSample.mediaSources.front().representations.front().streams.front())
        .timing.samples;
    badSamples[1].presentationTicks = badSamples[0].presentationTicks;
    expect(contains(validateTimelineProject(badVfrSample),
                    TimelineValidationCode::InvalidStream),
           "VFR presentation timestamps must remain strictly ordered");

    TimelineProject overflowingStream = project;
    auto &overflowingAudio = std::get<TimelineAudioStream>(
        overflowingStream.mediaSources.front().representations.front().streams[1]);
    overflowingAudio.startTicks = std::numeric_limits<std::int64_t>::max();
    overflowingAudio.durationTicks = 1;
    expect(contains(validateTimelineProject(overflowingStream),
                    TimelineValidationCode::InvalidStream),
           "stream timestamp ranges must not overflow signed ticks");

    TimelineProject badTimeBase = project;
    badTimeBase.sequences.front().timeBase.denominator = 0;
    expect(contains(validateTimelineProject(badTimeBase),
                    TimelineValidationCode::InvalidTimeBase),
           "zero time-base denominators must fail");

    TimelineProject badTimecode = project;
    badTimecode.sequences.front().timecode = TimelineTimecode{
        0, {24, 1}, 30, TimelineTimecodeCounting::NonDropFrame, true};
    expect(contains(validateTimelineProject(badTimecode),
                    TimelineValidationCode::InvalidTimecode),
           "timecode nominal counting rates must match their exact rational rate");

    TimelineProject duplicate = project;
    duplicate.mediaSources.push_back(duplicate.mediaSources.front());
    expect(contains(validateTimelineProject(duplicate),
                    TimelineValidationCode::DuplicateId),
           "duplicate stable ids must be rejected");

    TimelineProject missingSource = project;
    auto &missingSourceClip = std::get<TimelineVideoTrack>(
        missingSource.sequences.front().tracks.front()).clips.front();
    std::get<TimelineMediaReference>(missingSourceClip.properties.source)
        .mediaSourceId = "missing";
    expect(contains(validateTimelineProject(missingSource),
                    TimelineValidationCode::MissingReference),
           "clips must not reference missing media");

    TimelineProject wrongStream = project;
    auto &wrongStreamClip = std::get<TimelineVideoTrack>(
        wrongStream.sequences.front().tracks.front()).clips.front();
    std::get<TimelineMediaReference>(wrongStreamClip.properties.source)
        .streamId = "a0";
    expect(contains(validateTimelineProject(wrongStream),
                    TimelineValidationCode::StreamKindMismatch),
           "a video track must not silently accept an audio stream");

    TimelineProject zeroDuration = project;
    std::get<TimelineVideoTrack>(zeroDuration.sequences.front().tracks.front())
        .clips.front().properties.timelineRange.duration = 0;
    expect(contains(validateTimelineProject(zeroDuration),
                    TimelineValidationCode::InvalidClip),
           "clip timeline duration must be positive");

    TimelineProject badAutomation = project;
    auto &keyframes = std::get<TimelineVideoTrack>(
        badAutomation.sequences.front().tracks.front())
        .clips.front().properties.effects.front().automation.front().keyframes;
    keyframes[1].timeTicks = -1;
    expect(contains(validateTimelineProject(badAutomation),
                    TimelineValidationCode::InvalidAutomation),
           "automation keyframes must remain non-negative and strictly ordered");

    TimelineProject badPlaybackRate = project;
    auto &constantRateClip = std::get<TimelineVideoTrack>(
        badPlaybackRate.sequences.front().tracks.front()).clips.back();
    constantRateClip.properties.playbackRate = {2, 1};
    expect(contains(validateTimelineProject(badPlaybackRate),
                    TimelineValidationCode::InvalidClip),
           "constant playback rate must exactly relate source and sequence time bases");

    TimelineProject competingTimeMapRate = project;
    std::get<TimelineVideoTrack>(
        competingTimeMapRate.sequences.front().tracks.front())
        .clips.front().properties.playbackRate = {2, 1};
    expect(contains(validateTimelineProject(competingTimeMapRate),
                    TimelineValidationCode::InvalidClip),
           "an explicit time map must not silently compose with a constant playback rate");

    TimelineProject nonsensicalAutomation = project;
    auto &textEffect = std::get<TimelineVideoTrack>(
        nonsensicalAutomation.sequences.front().tracks.front())
        .clips.front().properties.effects.front();
    textEffect.parameters.front().value = std::string{"draft"};
    auto &textCurve = textEffect.automation.front();
    textCurve.defaultValue = std::string{"draft"};
    for (auto &keyframe : textCurve.keyframes) {
        keyframe.value = std::string{"final"};
        keyframe.interpolation = TimelineKeyframeInterpolation::Linear;
    }
    expect(contains(validateTimelineProject(nonsensicalAutomation),
                    TimelineValidationCode::InvalidAutomation),
           "non-numeric automation values may use hold interpolation only");

    TimelineProject invalidCueNumber = project;
    auto &cue = std::get<TimelineSubtitleStream>(
        invalidCueNumber.mediaSources.front().representations.front().streams[2])
        .cues.front();
    cue.position = std::numeric_limits<double>::quiet_NaN();
    expect(contains(validateTimelineProject(invalidCueNumber),
                    TimelineValidationCode::InvalidNumericValue),
           "subtitle cue positioning values must be finite");

    TimelineProject overflowingUnknownSample = project;
    for (auto &representation
         : overflowingUnknownSample.mediaSources.front().representations) {
        auto &unknownVideo = std::get<TimelineVideoStream>(
            representation.streams.front());
        unknownVideo.durationTicks = 0;
        unknownVideo.timing.samples.resize(1);
        unknownVideo.timing.samples.front().presentationTicks
            = std::numeric_limits<std::int64_t>::max();
        unknownVideo.timing.samples.front().durationTicks = 1;
    }
    expect(contains(validateTimelineProject(overflowingUnknownSample),
                    TimelineValidationCode::InvalidStream),
           "unknown-duration sample ranges must still reject tick overflow");

    TimelineProject overflowingUnknownCue = project;
    for (auto &representation
         : overflowingUnknownCue.mediaSources.front().representations) {
        auto &unknownSubtitles = std::get<TimelineSubtitleStream>(
            representation.streams[2]);
        unknownSubtitles.durationTicks = 0;
        unknownSubtitles.cues.front().range = {
            std::numeric_limits<std::int64_t>::max(), 1};
    }
    expect(contains(validateTimelineProject(overflowingUnknownCue),
                    TimelineValidationCode::InvalidRange),
           "unknown-duration subtitle cue ranges must still reject tick overflow");

    TimelineProject sampleOutsideFile = project;
    auto &fileSample = std::get<TimelineVideoStream>(
        sampleOutsideFile.mediaSources.front().representations.front().streams.front())
        .timing.samples.front();
    fileSample.byteOffset = 7'999'999U;
    fileSample.byteSize = 2U;
    expect(contains(validateTimelineProject(sampleOutsideFile),
                    TimelineValidationCode::InvalidStream),
           "sample byte ranges must fit the known representation file size");

    TimelineProject danglingSubtitleImage = project;
    auto &imageCue = std::get<TimelineSubtitleStream>(
        danglingSubtitleImage.mediaSources.front().representations.front().streams[2])
        .cues.front();
    imageCue.text.clear();
    imageCue.imageResourceId = "missing-attachment";
    expect(contains(validateTimelineProject(danglingSubtitleImage),
                    TimelineValidationCode::MissingReference),
           "image subtitle cues must reference an attachment in their representation");

    TimelineProject badNumeric = project;
    std::get<TimelineVideoTrack>(badNumeric.sequences.front().tracks.front())
        .clips.front().transform.opacity =
            std::numeric_limits<double>::quiet_NaN();
    expect(contains(validateTimelineProject(badNumeric),
                    TimelineValidationCode::InvalidNumericValue),
           "non-finite transform values must fail closed");

    TimelineProject badTransition = project;
    std::get<TimelineVideoTrack>(badTransition.sequences.front().tracks.front())
        .transitions.front().toClipId = "missing-clip";
    expect(contains(validateTimelineProject(badTransition),
                    TimelineValidationCode::MissingReference),
           "transitions must reference clips in their owning track");

    TimelineProject badTransitionAlignment = project;
    std::get<TimelineVideoTrack>(
        badTransitionAlignment.sequences.front().tracks.front())
        .transitions.front().alignment = TimelineTransitionAlignment::StartAtCut;
    expect(contains(validateTimelineProject(badTransitionAlignment),
                    TimelineValidationCode::InvalidTransition),
           "transition ranges must obey their declared cut alignment");

    TimelineProject danglingLinkGroup = project;
    std::get<TimelineVideoTrack>(danglingLinkGroup.sequences.front().tracks.front())
        .clips.back().properties.linkGroupId = "missing-link-group";
    expect(contains(validateTimelineProject(danglingLinkGroup),
                    TimelineValidationCode::MissingReference),
           "clip link-group ids must resolve to a group containing that clip");

    TimelineProject badNestedTrim = project;
    TimelineSequence nested;
    nested.id = "nested";
    nested.name = "Nested sequence";
    nested.timeBase = {1, 60'000};
    nested.editingFrameRate = {30, 1};
    nested.durationTicks = 120'000;
    nested.canvasExtent = {1'920, 1'080};
    nested.pixelAspectRatio = {1, 1};
    badNestedTrim.sequences.push_back(nested);
    auto &nestedReferenceClip = std::get<TimelineVideoTrack>(
        badNestedTrim.sequences.front().tracks.front()).clips.front();
    nestedReferenceClip.properties.source = TimelineSequenceReference{
        "nested", TimelineStreamKind::Video};
    nestedReferenceClip.properties.sourceRange = {0, 300'000};
    expect(contains(validateTimelineProject(badNestedTrim),
                    TimelineValidationCode::InvalidClip),
           "nested sequence trims must fit the referenced sequence range");

    TimelineProject nestedCycle = project;
    nested.durationTicks = 120'000;
    TimelineVideoClip nestedClip;
    nestedClip.properties.id = "nested-clip";
    nestedClip.properties.source = TimelineSequenceReference{
        "main", TimelineStreamKind::Video};
    nestedClip.properties.timelineRange = {0, 60'000};
    nestedClip.properties.sourceRange = {0, 60'000};
    nestedClip.properties.playbackRate = {1, 1};
    TimelineVideoTrack nestedTrack;
    nestedTrack.properties.id = "nested-video";
    nestedTrack.clips.push_back(nestedClip);
    nested.tracks = {nestedTrack};
    nestedCycle.sequences.push_back(nested);
    auto &cycleClip = std::get<TimelineVideoTrack>(
        nestedCycle.sequences.front().tracks.front()).clips.front();
    cycleClip.properties.source = TimelineSequenceReference{
        "nested", TimelineStreamKind::Video};
    cycleClip.properties.sourceRange = {0, 60'000};
    cycleClip.properties.timeMap.clear();
    expect(contains(validateTimelineProject(nestedCycle),
                    TimelineValidationCode::InvalidSequence),
           "nested sequence references must remain acyclic");

    TimelineProject badOutput = project;
    badOutput.renderProfiles.front().container.identifier.clear();
    expect(contains(validateTimelineProject(badOutput),
                    TimelineValidationCode::InvalidRenderProfile),
           "render profiles require an explicit mutable container identifier");

    TimelineProject missingVisualFormat = project;
    missingVisualFormat.sequences.front().canvasExtent = {};
    expect(contains(validateTimelineProject(missingVisualFormat),
                    TimelineValidationCode::InvalidSequence),
           "sequences with visual clips require a positive canvas extent");

    TimelineProject missingAudioFormat = project;
    missingAudioFormat.sequences.front().audioSampleRate = 0;
    missingAudioFormat.sequences.front().audioChannelLayout.clear();
    expect(contains(validateTimelineProject(missingAudioFormat),
                    TimelineValidationCode::InvalidSequence),
           "sequences with audio clips require an explicit mix sample rate and layout");

    TimelineProject missingWorkArea = project;
    missingWorkArea.sequences.front().workArea.reset();
    missingWorkArea.renderProfiles.front().rangeMode
        = TimelineRenderRangeMode::WorkArea;
    expect(contains(validateTimelineProject(missingWorkArea),
                    TimelineValidationCode::InvalidRenderProfile),
           "work-area output mode requires a work area on its sequence");

    TimelineProject duplicateOption = project;
    duplicateOption.renderProfiles.front().container.options.push_back(
        {"fastStart", false});
    expect(contains(validateTimelineProject(duplicateOption),
                    TimelineValidationCode::DuplicateProperty),
           "format-specific option keys must be unique without erasing values");

    TimelineProject binCycle = project;
    binCycle.bins.front().parentBinId = "child-bin";
    TimelineBin childBin;
    childBin.id = "child-bin";
    childBin.name = "Child";
    childBin.parentBinId = "rushes";
    binCycle.bins.push_back(childBin);
    expect(contains(validateTimelineProject(binCycle),
                    TimelineValidationCode::InvalidBin),
           "bin parent relationships must remain acyclic");

    return failures == 0 ? 0 : 1;
}
