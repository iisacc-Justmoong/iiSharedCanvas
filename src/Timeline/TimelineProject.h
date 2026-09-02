#pragma once

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iiSharedCanvas {

struct TimelineTimeBase {
    std::uint32_t numerator = 1;
    std::uint32_t denominator = 1;

    bool operator==(const TimelineTimeBase &) const = default;
};

struct TimelineFrameRate {
    std::uint32_t numerator = 24;
    std::uint32_t denominator = 1;

    bool operator==(const TimelineFrameRate &) const = default;
};

struct TimelineRational {
    std::int64_t numerator = 1;
    std::uint64_t denominator = 1;

    bool operator==(const TimelineRational &) const = default;
};

struct TimelineTickRange {
    std::int64_t start = 0;
    std::int64_t duration = 0;

    bool operator==(const TimelineTickRange &) const = default;
};

struct TimelineExtent {
    std::int32_t width = 0;
    std::int32_t height = 0;

    bool operator==(const TimelineExtent &) const = default;
};

struct TimelineVector2 {
    double x = 0.0;
    double y = 0.0;

    bool operator==(const TimelineVector2 &) const = default;
};

struct TimelineColor {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double alpha = 1.0;

    bool operator==(const TimelineColor &) const = default;
};

using TimelinePropertyValue = std::variant<
    std::monostate,
    bool,
    std::int64_t,
    double,
    std::string,
    TimelineRational,
    TimelineColor,
    TimelineVector2,
    std::vector<double>>;

struct TimelineProperty {
    std::string key;
    TimelinePropertyValue value;

    bool operator==(const TimelineProperty &) const = default;
};

enum class TimelineFrameRateMode : std::uint8_t {
    Constant,
    Variable,
    Passthrough,
};

enum class TimelineTimecodeCounting : std::uint8_t {
    NonDropFrame,
    DropFrame,
};

struct TimelineTimecode {
    std::int64_t startFrame = 0;
    TimelineFrameRate frameRate;
    std::uint32_t nominalFramesPerSecond = 24;
    TimelineTimecodeCounting counting = TimelineTimecodeCounting::NonDropFrame;
    bool wrapsAt24Hours = true;

    bool operator==(const TimelineTimecode &) const = default;
};

struct TimelineContainerDescriptor {
    std::string identifier;
    std::string name;
    std::string mimeType;
    std::string fileExtension;
    std::string majorBrand;
    std::vector<std::string> compatibleBrands;
    std::string profile;
    std::vector<TimelineProperty> options;

    bool operator==(const TimelineContainerDescriptor &) const = default;
};

struct TimelineCodecDescriptor {
    std::string identifier;
    std::string name;
    std::string profile;
    std::string level;
    std::string tag;
    std::string implementation;
    std::optional<std::uint64_t> bitRate;
    std::optional<std::uint32_t> bitsPerCodedSample;
    std::optional<std::uint32_t> bitsPerRawSample;
    std::vector<std::uint8_t> initializationData;
    std::vector<TimelineProperty> options;

    bool operator==(const TimelineCodecDescriptor &) const = default;
};

enum class TimelineColorRange : std::uint8_t {
    Unspecified,
    Limited,
    Full,
};

struct TimelineMasteringDisplay {
    TimelineVector2 red;
    TimelineVector2 green;
    TimelineVector2 blue;
    TimelineVector2 white;
    double minimumLuminance = 0.0;
    double maximumLuminance = 0.0;

    bool operator==(const TimelineMasteringDisplay &) const = default;
};

struct TimelineContentLight {
    double maximumContentLightLevel = 0.0;
    double maximumFrameAverageLightLevel = 0.0;

    bool operator==(const TimelineContentLight &) const = default;
};

struct TimelineColorDescription {
    std::string primaries;
    std::string transfer;
    std::string matrix;
    TimelineColorRange range = TimelineColorRange::Unspecified;
    std::string chromaLocationTop;
    std::string chromaLocationBottom;
    std::optional<TimelineMasteringDisplay> masteringDisplay;
    std::optional<TimelineContentLight> contentLight;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineColorDescription &) const = default;
};

enum class TimelineScanMode : std::uint8_t {
    Unspecified,
    Progressive,
    Interlaced,
};

enum class TimelineFieldOrder : std::uint8_t {
    None,
    TopFirst,
    BottomFirst,
};

enum class TimelineAlphaMode : std::uint8_t {
    None,
    Straight,
    Premultiplied,
};

struct TimelineVideoSampleTiming {
    std::int64_t presentationTicks = 0;
    std::optional<std::int64_t> decodingTicks;
    std::int64_t durationTicks = 0;
    bool keyFrame = false;
    std::optional<std::uint64_t> byteOffset;
    std::optional<std::uint64_t> byteSize;

    bool operator==(const TimelineVideoSampleTiming &) const = default;
};

struct TimelineVideoTiming {
    TimelineFrameRateMode mode = TimelineFrameRateMode::Constant;
    std::optional<TimelineFrameRate> nominal;
    std::optional<TimelineFrameRate> average;
    std::optional<TimelineFrameRate> minimum;
    std::optional<TimelineFrameRate> maximum;
    std::vector<TimelineVideoSampleTiming> samples;

    bool operator==(const TimelineVideoTiming &) const = default;
};

struct TimelineAudioLoudness {
    std::optional<double> integratedLufs;
    std::optional<double> truePeakDbtp;
    std::optional<double> loudnessRangeLu;
    std::optional<double> maximumMomentaryLufs;
    std::optional<double> maximumShortTermLufs;

    bool operator==(const TimelineAudioLoudness &) const = default;
};

struct TimelineVideoStream {
    std::string id;
    std::uint32_t streamIndex = 0;
    TimelineTimeBase timeBase;
    std::int64_t startTicks = 0;
    std::int64_t durationTicks = 0;
    TimelineCodecDescriptor codec;
    TimelineExtent codedExtent;
    TimelineExtent displayExtent;
    TimelineRational pixelAspectRatio;
    TimelineVideoTiming timing;
    std::string pixelFormat;
    std::string chromaSubsampling;
    std::uint16_t bitDepth = 0;
    TimelineScanMode scanMode = TimelineScanMode::Unspecified;
    TimelineFieldOrder fieldOrder = TimelineFieldOrder::None;
    TimelineAlphaMode alphaMode = TimelineAlphaMode::None;
    TimelineColorDescription color;
    double rotationDegrees = 0.0;
    bool mirroredHorizontal = false;
    bool mirroredVertical = false;
    std::optional<std::uint64_t> frameCount;
    std::string projection;
    std::string stereoLayout;
    std::string language;
    std::string title;
    std::vector<std::string> dispositions;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineVideoStream &) const = default;
};

struct TimelineAudioStream {
    std::string id;
    std::uint32_t streamIndex = 0;
    TimelineTimeBase timeBase;
    std::int64_t startTicks = 0;
    std::int64_t durationTicks = 0;
    TimelineCodecDescriptor codec;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::string channelLayout;
    std::vector<std::string> speakerLabels;
    std::string sampleFormat;
    std::uint16_t bitDepth = 0;
    std::optional<std::uint32_t> frameSize;
    std::optional<std::uint32_t> blockAlignment;
    std::uint64_t primingSamples = 0;
    std::uint64_t trailingSamples = 0;
    std::string serviceKind;
    TimelineAudioLoudness loudness;
    std::string language;
    std::string title;
    std::vector<std::string> dispositions;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineAudioStream &) const = default;
};

enum class TimelineSubtitleKind : std::uint8_t {
    Subtitle,
    Caption,
    HearingImpaired,
    Forced,
    Description,
    Chapter,
    Metadata,
};

struct TimelineSubtitleCue {
    std::string id;
    TimelineTickRange range;
    std::string text;
    std::string markup;
    std::string imageResourceId;
    std::string language;
    std::string voice;
    std::string regionId;
    std::string styleId;
    std::optional<double> line;
    std::optional<double> position;
    std::optional<double> size;
    std::string horizontalAlignment;
    std::string verticalAlignment;
    std::string writingDirection;
    std::vector<TimelineProperty> settings;

    bool operator==(const TimelineSubtitleCue &) const = default;
};

struct TimelineSubtitleStream {
    std::string id;
    std::uint32_t streamIndex = 0;
    TimelineTimeBase timeBase;
    std::int64_t startTicks = 0;
    std::int64_t durationTicks = 0;
    TimelineCodecDescriptor codec;
    std::string format;
    TimelineSubtitleKind kind = TimelineSubtitleKind::Subtitle;
    std::string textEncoding = "UTF-8";
    TimelineExtent canvasExtent;
    std::string language;
    bool hearingImpaired = false;
    bool forced = false;
    std::vector<TimelineSubtitleCue> cues;
    std::string title;
    std::vector<std::string> dispositions;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineSubtitleStream &) const = default;
};

struct TimelineDataStream {
    std::string id;
    std::uint32_t streamIndex = 0;
    TimelineTimeBase timeBase;
    std::int64_t startTicks = 0;
    std::int64_t durationTicks = 0;
    TimelineCodecDescriptor codec;
    std::string kind;
    std::string language;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineDataStream &) const = default;
};

using TimelineMediaStream = std::variant<
    TimelineVideoStream,
    TimelineAudioStream,
    TimelineSubtitleStream,
    TimelineDataStream>;

enum class TimelineStreamKind : std::uint8_t {
    Video,
    Audio,
    Subtitle,
    Data,
};

enum class TimelineMediaRepresentationRole : std::uint8_t {
    Original,
    Proxy,
    Optimized,
    Preview,
    OfflinePlaceholder,
};

struct TimelineAttachment {
    std::string id;
    std::string name;
    std::string mimeType;
    std::string uri;
    std::vector<std::uint8_t> data;

    bool operator==(const TimelineAttachment &) const = default;
};

struct TimelineMediaRepresentation {
    std::string id;
    TimelineMediaRepresentationRole role = TimelineMediaRepresentationRole::Original;
    std::string uri;
    TimelineContainerDescriptor container;
    std::optional<std::uint64_t> fileSizeBytes;
    std::string checksumAlgorithm;
    std::string checksum;
    std::int64_t durationTicks = 0;
    TimelineTimeBase timeBase;
    std::optional<TimelineTimecode> timecode;
    std::vector<TimelineMediaStream> streams;
    std::vector<TimelineAttachment> attachments;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineMediaRepresentation &) const = default;
};

struct TimelineMediaSource {
    std::string id;
    std::string name;
    std::string originalRepresentationId;
    std::string activeRepresentationId;
    std::vector<TimelineMediaRepresentation> representations;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineMediaSource &) const = default;
};

struct TimelineMediaReference {
    std::string mediaSourceId;
    std::string streamId;

    bool operator==(const TimelineMediaReference &) const = default;
};

struct TimelineSequenceReference {
    std::string sequenceId;
    TimelineStreamKind kind = TimelineStreamKind::Video;

    bool operator==(const TimelineSequenceReference &) const = default;
};

struct TimelineGeneratedReference {
    std::string generatorId;
    TimelineStreamKind kind = TimelineStreamKind::Video;
    TimelineTimeBase timeBase;
    std::vector<TimelineProperty> parameters;

    bool operator==(const TimelineGeneratedReference &) const = default;
};

using TimelineClipSource = std::variant<
    TimelineMediaReference,
    TimelineSequenceReference,
    TimelineGeneratedReference>;

enum class TimelineKeyframeInterpolation : std::uint8_t {
    Hold,
    Linear,
    Bezier,
};

struct TimelineBezierTangent {
    double timeOffset = 0.0;
    double valueOffset = 0.0;

    bool operator==(const TimelineBezierTangent &) const = default;
};

struct TimelineTimeMapPoint {
    std::int64_t timelineTicks = 0;
    std::int64_t sourceTicks = 0;
    TimelineKeyframeInterpolation interpolation = TimelineKeyframeInterpolation::Linear;
    TimelineBezierTangent incoming;
    TimelineBezierTangent outgoing;

    bool operator==(const TimelineTimeMapPoint &) const = default;
};

struct TimelineAutomationKeyframe {
    std::int64_t timeTicks = 0;
    TimelinePropertyValue value;
    TimelineKeyframeInterpolation interpolation = TimelineKeyframeInterpolation::Linear;
    TimelineBezierTangent incoming;
    TimelineBezierTangent outgoing;

    bool operator==(const TimelineAutomationKeyframe &) const = default;
};

struct TimelineAutomationCurve {
    std::string parameterId;
    TimelinePropertyValue defaultValue;
    std::vector<TimelineAutomationKeyframe> keyframes;

    bool operator==(const TimelineAutomationCurve &) const = default;
};

struct TimelineEffect {
    std::string id;
    std::string name;
    std::string category;
    std::string pluginId;
    std::string vendor;
    std::string schemaVersion;
    bool enabled = true;
    bool bypass = false;
    std::optional<TimelineTickRange> activeRange;
    std::vector<TimelineProperty> parameters;
    std::vector<TimelineAutomationCurve> automation;
    std::vector<std::string> inputIds;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineEffect &) const = default;
};

enum class TimelineMarkerKind : std::uint8_t {
    Standard,
    Chapter,
    Comment,
    Cue,
    Beat,
    In,
    Out,
};

struct TimelineMarker {
    std::string id;
    std::string name;
    TimelineMarkerKind kind = TimelineMarkerKind::Standard;
    TimelineTickRange range;
    TimelineColor color;
    std::string comment;
    std::string author;
    std::string createdAt;
    std::vector<std::string> tags;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineMarker &) const = default;
};

enum class TimelineLoopMode : std::uint8_t {
    None,
    Repeat,
    PingPong,
};

enum class TimelineVideoSampling : std::uint8_t {
    Nearest,
    Previous,
    Blend,
    OpticalFlow,
};

enum class TimelineAudioStretchMode : std::uint8_t {
    Resample,
    TimeStretch,
    Elastique,
    Custom,
};

struct TimelineClipProperties {
    std::string id;
    std::string name;
    bool enabled = true;
    TimelineClipSource source;
    TimelineTickRange timelineRange;
    TimelineTickRange sourceRange;
    TimelineRational playbackRate;
    TimelineLoopMode loopMode = TimelineLoopMode::None;
    TimelineVideoSampling videoSampling = TimelineVideoSampling::Previous;
    TimelineAudioStretchMode audioStretchMode = TimelineAudioStretchMode::Resample;
    bool preservePitch = true;
    std::string linkGroupId;
    std::string syncGroupId;
    std::string role;
    TimelineColor colorLabel;
    std::vector<TimelineTimeMapPoint> timeMap;
    std::vector<TimelineEffect> effects;
    std::vector<TimelineMarker> markers;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineClipProperties &) const = default;
};

struct TimelineTransform {
    TimelineVector2 position;
    TimelineVector2 anchor;
    TimelineVector2 scale{1.0, 1.0};
    double rotationDegrees = 0.0;
    TimelineVector2 skew;
    double opacity = 1.0;
    std::string blendMode = "normal";

    bool operator==(const TimelineTransform &) const = default;
};

struct TimelineCrop {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double feather = 0.0;

    bool operator==(const TimelineCrop &) const = default;
};

struct TimelineAudioMix {
    double gainDb = 0.0;
    double pan = 0.0;
    bool phaseInverted = false;
    std::int64_t fadeInTicks = 0;
    std::int64_t fadeOutTicks = 0;
    std::vector<double> channelMatrix;
    std::string outputBusId;

    bool operator==(const TimelineAudioMix &) const = default;
};

struct TimelineSubtitleStyle {
    std::string fontFamily;
    double fontSize = 0.0;
    TimelineColor fill{1.0, 1.0, 1.0, 1.0};
    TimelineColor outline{0.0, 0.0, 0.0, 1.0};
    double outlineWidth = 0.0;
    TimelineColor background{0.0, 0.0, 0.0, 0.0};
    bool bold = false;
    bool italic = false;
    bool underline = false;
    std::string horizontalAlignment;
    std::string verticalAlignment;
    std::string writingDirection;
    std::vector<TimelineProperty> properties;

    bool operator==(const TimelineSubtitleStyle &) const = default;
};

struct TimelineVideoClip {
    TimelineClipProperties properties;
    TimelineTransform transform;
    TimelineCrop crop;

    bool operator==(const TimelineVideoClip &) const = default;
};

struct TimelineAudioClip {
    TimelineClipProperties properties;
    TimelineAudioMix mix;

    bool operator==(const TimelineAudioClip &) const = default;
};

struct TimelineSubtitleClip {
    TimelineClipProperties properties;
    std::string text;
    std::string markup;
    TimelineSubtitleStyle style;

    bool operator==(const TimelineSubtitleClip &) const = default;
};

struct TimelineDataClip {
    TimelineClipProperties properties;
    std::string format;
    std::vector<std::uint8_t> payload;

    bool operator==(const TimelineDataClip &) const = default;
};

using TimelineClip = std::variant<
    TimelineVideoClip,
    TimelineAudioClip,
    TimelineSubtitleClip,
    TimelineDataClip>;

class IISHAREDCANVAS_EXPORT TimelineClipView final {
public:
    TimelineClipView() = default;
    TimelineClipView(TimelineVideoClip *clip) noexcept;
    TimelineClipView(TimelineAudioClip *clip) noexcept;
    TimelineClipView(TimelineSubtitleClip *clip) noexcept;
    TimelineClipView(TimelineDataClip *clip) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] TimelineClipView operator*() const noexcept;
    [[nodiscard]] bool operator==(std::nullptr_t) const noexcept;
    [[nodiscard]] bool operator!=(std::nullptr_t) const noexcept;

private:
    using Pointer = std::variant<
        std::monostate,
        TimelineVideoClip *,
        TimelineAudioClip *,
        TimelineSubtitleClip *,
        TimelineDataClip *>;

    Pointer m_pointer;

    friend std::optional<TimelineStreamKind> timelineClipKind(
        TimelineClipView) noexcept;
    friend TimelineClipProperties &timelineClipProperties(TimelineClipView) noexcept;
};

class IISHAREDCANVAS_EXPORT TimelineConstClipView final {
public:
    TimelineConstClipView() = default;
    TimelineConstClipView(const TimelineVideoClip *clip) noexcept;
    TimelineConstClipView(const TimelineAudioClip *clip) noexcept;
    TimelineConstClipView(const TimelineSubtitleClip *clip) noexcept;
    TimelineConstClipView(const TimelineDataClip *clip) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] TimelineConstClipView operator*() const noexcept;
    [[nodiscard]] bool operator==(std::nullptr_t) const noexcept;
    [[nodiscard]] bool operator!=(std::nullptr_t) const noexcept;

private:
    using Pointer = std::variant<
        std::monostate,
        const TimelineVideoClip *,
        const TimelineAudioClip *,
        const TimelineSubtitleClip *,
        const TimelineDataClip *>;

    Pointer m_pointer;

    friend std::optional<TimelineStreamKind> timelineClipKind(
        TimelineConstClipView) noexcept;
    friend const TimelineClipProperties &timelineClipProperties(
        TimelineConstClipView) noexcept;
};

enum class TimelineTransitionAlignment : std::uint8_t {
    StartAtCut,
    CenteredOnCut,
    EndAtCut,
    Custom,
};

struct TimelineTransition {
    std::string id;
    std::string name;
    std::string typeId;
    bool enabled = true;
    std::string fromClipId;
    std::string toClipId;
    TimelineTickRange timelineRange;
    TimelineTransitionAlignment alignment = TimelineTransitionAlignment::CenteredOnCut;
    std::vector<TimelineProperty> parameters;
    std::vector<TimelineAutomationCurve> automation;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineTransition &) const = default;
};

struct TimelineTrackProperties {
    std::string id;
    std::string name;
    bool enabled = true;
    bool locked = false;
    bool muted = false;
    bool solo = false;
    bool syncLocked = false;
    std::string role;
    std::string language;
    TimelineColor colorLabel;
    std::string blendMode = "normal";
    double opacity = 1.0;
    std::string outputBusId;
    std::vector<TimelineEffect> effects;
    std::vector<TimelineMarker> markers;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineTrackProperties &) const = default;
};

struct TimelineVideoTrack {
    TimelineTrackProperties properties;
    std::vector<TimelineVideoClip> clips;
    std::vector<TimelineTransition> transitions;

    bool operator==(const TimelineVideoTrack &) const = default;
};

struct TimelineAudioTrack {
    TimelineTrackProperties properties;
    std::vector<TimelineAudioClip> clips;
    std::vector<TimelineTransition> transitions;

    bool operator==(const TimelineAudioTrack &) const = default;
};

struct TimelineSubtitleTrack {
    TimelineTrackProperties properties;
    std::vector<TimelineSubtitleClip> clips;
    std::vector<TimelineTransition> transitions;

    bool operator==(const TimelineSubtitleTrack &) const = default;
};

struct TimelineDataTrack {
    TimelineTrackProperties properties;
    std::vector<TimelineDataClip> clips;
    std::vector<TimelineTransition> transitions;

    bool operator==(const TimelineDataTrack &) const = default;
};

using TimelineTrack = std::variant<
    TimelineVideoTrack,
    TimelineAudioTrack,
    TimelineSubtitleTrack,
    TimelineDataTrack>;

struct TimelineLinkGroup {
    std::string id;
    std::vector<std::string> clipIds;
    bool syncLocked = false;

    bool operator==(const TimelineLinkGroup &) const = default;
};

struct TimelineSequence {
    std::string id;
    std::string name;
    TimelineTimeBase timeBase;
    TimelineFrameRate editingFrameRate;
    std::int64_t startTicks = 0;
    std::int64_t durationTicks = 0;
    std::optional<TimelineTickRange> workArea;
    TimelineExtent canvasExtent;
    TimelineRational pixelAspectRatio;
    TimelineColorDescription color;
    std::uint32_t audioSampleRate = 0;
    std::string audioChannelLayout;
    std::optional<TimelineTimecode> timecode;
    std::vector<TimelineTrack> tracks;
    std::vector<TimelineMarker> markers;
    std::vector<TimelineLinkGroup> linkGroups;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineSequence &) const = default;
};

struct TimelineBin {
    std::string id;
    std::string name;
    std::string parentBinId;
    std::vector<std::string> mediaSourceIds;
    std::vector<std::string> sequenceIds;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineBin &) const = default;
};

enum class TimelineRateControl : std::uint8_t {
    ConstantQuality,
    ConstantBitrate,
    AverageBitrate,
    VariableBitrate,
    ConstrainedVariableBitrate,
    Lossless,
};

struct TimelineVideoOutput {
    TimelineCodecDescriptor codec;
    TimelineExtent extent;
    TimelineFrameRate frameRate;
    TimelineFrameRateMode frameRateMode = TimelineFrameRateMode::Constant;
    TimelineRational pixelAspectRatio;
    std::string pixelFormat;
    std::uint16_t bitDepth = 0;
    TimelineColorDescription color;
    TimelineAlphaMode alphaMode = TimelineAlphaMode::None;
    TimelineRateControl rateControl = TimelineRateControl::VariableBitrate;
    std::optional<std::uint64_t> minimumBitRate;
    std::optional<std::uint64_t> averageBitRate;
    std::optional<std::uint64_t> maximumBitRate;
    std::optional<double> quality;
    std::uint32_t gopSize = 0;
    std::uint32_t maximumBFrames = 0;
    bool twoPass = false;
    std::string encoderPreset;
    std::string tune;
    std::string hardwareEncoder;
    std::vector<TimelineProperty> options;

    bool operator==(const TimelineVideoOutput &) const = default;
};

struct TimelineAudioOutput {
    TimelineCodecDescriptor codec;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::string channelLayout;
    std::string sampleFormat;
    std::uint16_t bitDepth = 0;
    TimelineRateControl rateControl = TimelineRateControl::AverageBitrate;
    std::optional<std::uint64_t> bitRate;
    std::optional<double> quality;
    bool normalizeLoudness = false;
    std::optional<double> targetIntegratedLufs;
    std::optional<double> targetTruePeakDbtp;
    std::string resampler;
    std::string dither;
    std::vector<TimelineProperty> options;

    bool operator==(const TimelineAudioOutput &) const = default;
};

enum class TimelineSubtitleOutputMode : std::uint8_t {
    Mux,
    Sidecar,
    BurnIn,
    Omit,
};

struct TimelineSubtitleOutput {
    TimelineSubtitleOutputMode mode = TimelineSubtitleOutputMode::Mux;
    TimelineCodecDescriptor codec;
    std::string format;
    std::string language;
    bool defaultDisposition = false;
    bool forcedDisposition = false;
    std::string sidecarUri;
    std::vector<TimelineProperty> options;

    bool operator==(const TimelineSubtitleOutput &) const = default;
};

enum class TimelineRenderRangeMode : std::uint8_t {
    EntireSequence,
    WorkArea,
    Custom,
};

struct TimelineRenderProfile {
    std::string id;
    std::string name;
    std::string sequenceId;
    std::string outputUri;
    TimelineContainerDescriptor container;
    TimelineRenderRangeMode rangeMode = TimelineRenderRangeMode::EntireSequence;
    std::optional<TimelineTickRange> customRange;
    std::optional<TimelineVideoOutput> video;
    std::optional<TimelineAudioOutput> audio;
    std::vector<TimelineSubtitleOutput> subtitles;
    bool fastStart = false;
    bool fragmented = false;
    std::optional<std::int64_t> fragmentDurationTicks;
    bool writeChapters = true;
    bool writeTimecode = true;
    bool writeMetadata = true;
    std::vector<TimelineProperty> muxOptions;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineRenderProfile &) const = default;
};

struct TimelineProject {
    std::string id;
    std::string name;
    std::string applicationName;
    std::string applicationVersion;
    std::string createdAt;
    std::string modifiedAt;
    std::string activeSequenceId;
    std::vector<TimelineMediaSource> mediaSources;
    std::vector<TimelineSequence> sequences;
    std::vector<TimelineBin> bins;
    std::vector<TimelineRenderProfile> renderProfiles;
    std::vector<TimelineProperty> metadata;

    bool operator==(const TimelineProject &) const = default;
};

enum class TimelineValidationCode : std::uint8_t {
    InvalidProject,
    InvalidId,
    DuplicateId,
    DuplicateProperty,
    MissingReference,
    StreamKindMismatch,
    InvalidTimeBase,
    InvalidFrameRate,
    InvalidTimecode,
    InvalidRange,
    InvalidRepresentation,
    InvalidStream,
    InvalidSequence,
    InvalidTrack,
    InvalidClip,
    InvalidEffect,
    InvalidAutomation,
    InvalidTransition,
    InvalidMarker,
    InvalidLinkGroup,
    InvalidBin,
    InvalidRenderProfile,
    InvalidNumericValue,
};

struct TimelineValidationIssue {
    TimelineValidationCode code = TimelineValidationCode::InvalidProject;
    std::string path;
    std::string message;

    bool operator==(const TimelineValidationIssue &) const = default;
};

struct TimelineValidationResult {
    std::vector<TimelineValidationIssue> issues;

    [[nodiscard]] bool ok() const noexcept { return issues.empty(); }
};

IISHAREDCANVAS_EXPORT std::optional<double> timelineTicksToSeconds(
    std::int64_t ticks,
    TimelineTimeBase timeBase) noexcept;

IISHAREDCANVAS_EXPORT TimelineStreamKind timelineStreamKind(
    const TimelineMediaStream &stream) noexcept;
IISHAREDCANVAS_EXPORT TimelineStreamKind timelineTrackKind(
    const TimelineTrack &track) noexcept;
IISHAREDCANVAS_EXPORT TimelineStreamKind timelineClipKind(
    const TimelineClip &clip) noexcept;
IISHAREDCANVAS_EXPORT std::optional<TimelineStreamKind> timelineClipKind(
    TimelineClipView clip) noexcept;
IISHAREDCANVAS_EXPORT std::optional<TimelineStreamKind> timelineClipKind(
    TimelineConstClipView clip) noexcept;

IISHAREDCANVAS_EXPORT TimelineTrackProperties &timelineTrackProperties(
    TimelineTrack &track) noexcept;
IISHAREDCANVAS_EXPORT const TimelineTrackProperties &timelineTrackProperties(
    const TimelineTrack &track) noexcept;
IISHAREDCANVAS_EXPORT TimelineClipProperties &timelineClipProperties(
    TimelineClip &clip) noexcept;
IISHAREDCANVAS_EXPORT const TimelineClipProperties &timelineClipProperties(
    const TimelineClip &clip) noexcept;
IISHAREDCANVAS_EXPORT TimelineClipProperties &timelineClipProperties(
    TimelineClipView clip) noexcept;
IISHAREDCANVAS_EXPORT const TimelineClipProperties &timelineClipProperties(
    TimelineConstClipView clip) noexcept;
IISHAREDCANVAS_EXPORT TimelineClipProperties &timelineClipProperties(
    TimelineClipProperties &properties) noexcept;
IISHAREDCANVAS_EXPORT const TimelineClipProperties &timelineClipProperties(
    const TimelineClipProperties &properties) noexcept;

IISHAREDCANVAS_EXPORT TimelineMediaSource *findTimelineMediaSource(
    TimelineProject &project,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const TimelineMediaSource *findTimelineMediaSource(
    const TimelineProject &project,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT TimelineMediaRepresentation *findTimelineMediaRepresentation(
    TimelineMediaSource &source,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const TimelineMediaRepresentation *findTimelineMediaRepresentation(
    const TimelineMediaSource &source,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT TimelineMediaStream *findTimelineMediaStream(
    TimelineMediaSource &source,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const TimelineMediaStream *findTimelineMediaStream(
    const TimelineMediaSource &source,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT TimelineSequence *findTimelineSequence(
    TimelineProject &project,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const TimelineSequence *findTimelineSequence(
    const TimelineProject &project,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT TimelineTrack *findTimelineTrack(
    TimelineSequence &sequence,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const TimelineTrack *findTimelineTrack(
    const TimelineSequence &sequence,
    const std::string &id) noexcept;

// The view resolves a typed clip without allocating or copying it. It is
// invalidated by any mutation that can relocate the owning project, sequence,
// track, or clip collection, including a successful TimelineEditor commit.
IISHAREDCANVAS_EXPORT TimelineClipView findTimelineClip(
    TimelineSequence &sequence,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT TimelineConstClipView findTimelineClip(
    const TimelineSequence &sequence,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT TimelineVideoClip *findTimelineVideoClip(
    TimelineSequence &sequence,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT TimelineAudioClip *findTimelineAudioClip(
    TimelineSequence &sequence,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT TimelineSubtitleClip *findTimelineSubtitleClip(
    TimelineSequence &sequence,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT TimelineDataClip *findTimelineDataClip(
    TimelineSequence &sequence,
    const std::string &id) noexcept;

IISHAREDCANVAS_EXPORT TimelineRenderProfile *findTimelineRenderProfile(
    TimelineProject &project,
    const std::string &id) noexcept;
IISHAREDCANVAS_EXPORT const TimelineRenderProfile *findTimelineRenderProfile(
    const TimelineProject &project,
    const std::string &id) noexcept;

IISHAREDCANVAS_EXPORT TimelineValidationResult validateTimelineProject(
    const TimelineProject &project);

} // namespace iiSharedCanvas
