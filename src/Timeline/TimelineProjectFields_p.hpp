#pragma once

#include "Timeline/TimelineProject.h"
#include <string_view>
#include <tuple>

// IISC 1.28 timeline field order is frozen. The 1.29 clip, video and track
// extensions are optional trailing fields; readers accept 1.28 field counts.
namespace iiSharedCanvas::timeline_codec {
template<class T> struct Fields;
#define IISC_TIMELINE_FIELDS(Type, ...) \
    template<> struct Fields<Type> { \
        static constexpr std::string_view typeName = #Type; \
        static constexpr std::string_view fieldNames = #__VA_ARGS__; \
        template<class V> static auto tie(V &x) { return std::tie(__VA_ARGS__); } \
    };

IISC_TIMELINE_FIELDS(TimelineTimeBase, x.numerator, x.denominator)
IISC_TIMELINE_FIELDS(TimelineFrameRate, x.numerator, x.denominator)
IISC_TIMELINE_FIELDS(TimelineRational, x.numerator, x.denominator)
IISC_TIMELINE_FIELDS(TimelineTickRange, x.start, x.duration)
IISC_TIMELINE_FIELDS(TimelineExtent, x.width, x.height)
IISC_TIMELINE_FIELDS(TimelineVector2, x.x, x.y)
IISC_TIMELINE_FIELDS(TimelineColor, x.red, x.green, x.blue, x.alpha)
IISC_TIMELINE_FIELDS(TimelineProperty, x.key, x.value)
IISC_TIMELINE_FIELDS(TimelineTimecode, x.startFrame, x.frameRate, x.nominalFramesPerSecond, x.counting, x.wrapsAt24Hours)
IISC_TIMELINE_FIELDS(TimelineContainerDescriptor, x.identifier, x.name, x.mimeType, x.fileExtension, x.majorBrand, x.compatibleBrands, x.profile, x.options)
IISC_TIMELINE_FIELDS(TimelineCodecDescriptor, x.identifier, x.name, x.profile, x.level, x.tag, x.implementation, x.bitRate, x.bitsPerCodedSample, x.bitsPerRawSample, x.initializationData, x.options)
IISC_TIMELINE_FIELDS(TimelineMasteringDisplay, x.red, x.green, x.blue, x.white, x.minimumLuminance, x.maximumLuminance)
IISC_TIMELINE_FIELDS(TimelineContentLight, x.maximumContentLightLevel, x.maximumFrameAverageLightLevel)
IISC_TIMELINE_FIELDS(TimelineColorDescription, x.primaries, x.transfer, x.matrix, x.range, x.chromaLocationTop, x.chromaLocationBottom, x.masteringDisplay, x.contentLight, x.metadata)
IISC_TIMELINE_FIELDS(TimelineVideoSampleTiming, x.presentationTicks, x.decodingTicks, x.durationTicks, x.keyFrame, x.byteOffset, x.byteSize)
IISC_TIMELINE_FIELDS(TimelineVideoTiming, x.mode, x.nominal, x.average, x.minimum, x.maximum, x.samples)
IISC_TIMELINE_FIELDS(TimelineAudioLoudness, x.integratedLufs, x.truePeakDbtp, x.loudnessRangeLu, x.maximumMomentaryLufs, x.maximumShortTermLufs)
IISC_TIMELINE_FIELDS(TimelineVideoStream, x.id, x.streamIndex, x.timeBase, x.startTicks, x.durationTicks, x.codec, x.codedExtent, x.displayExtent, x.pixelAspectRatio, x.timing, x.pixelFormat, x.chromaSubsampling, x.bitDepth, x.scanMode, x.fieldOrder, x.alphaMode, x.color, x.rotationDegrees, x.mirroredHorizontal, x.mirroredVertical, x.frameCount, x.projection, x.stereoLayout, x.language, x.title, x.dispositions, x.metadata)
IISC_TIMELINE_FIELDS(TimelineAudioStream, x.id, x.streamIndex, x.timeBase, x.startTicks, x.durationTicks, x.codec, x.sampleRate, x.channelCount, x.channelLayout, x.speakerLabels, x.sampleFormat, x.bitDepth, x.frameSize, x.blockAlignment, x.primingSamples, x.trailingSamples, x.serviceKind, x.loudness, x.language, x.title, x.dispositions, x.metadata)
IISC_TIMELINE_FIELDS(TimelineSubtitleCue, x.id, x.range, x.text, x.markup, x.imageResourceId, x.language, x.voice, x.regionId, x.styleId, x.line, x.position, x.size, x.horizontalAlignment, x.verticalAlignment, x.writingDirection, x.settings)
IISC_TIMELINE_FIELDS(TimelineSubtitleStream, x.id, x.streamIndex, x.timeBase, x.startTicks, x.durationTicks, x.codec, x.format, x.kind, x.textEncoding, x.canvasExtent, x.language, x.hearingImpaired, x.forced, x.cues, x.title, x.dispositions, x.metadata)
IISC_TIMELINE_FIELDS(TimelineDataStream, x.id, x.streamIndex, x.timeBase, x.startTicks, x.durationTicks, x.codec, x.kind, x.language, x.metadata)
IISC_TIMELINE_FIELDS(TimelineAttachment, x.id, x.name, x.mimeType, x.uri, x.data)
IISC_TIMELINE_FIELDS(TimelineMediaRepresentation, x.id, x.role, x.uri, x.container, x.fileSizeBytes, x.checksumAlgorithm, x.checksum, x.durationTicks, x.timeBase, x.timecode, x.streams, x.attachments, x.metadata, x.ownedMediaId)
IISC_TIMELINE_FIELDS(TimelineMediaSource, x.id, x.name, x.originalRepresentationId, x.activeRepresentationId, x.representations, x.metadata)
IISC_TIMELINE_FIELDS(TimelineMediaReference, x.mediaSourceId, x.streamId)
IISC_TIMELINE_FIELDS(TimelineSequenceReference, x.sequenceId, x.kind)
IISC_TIMELINE_FIELDS(TimelineGeneratedReference, x.generatorId, x.kind, x.timeBase, x.parameters)
IISC_TIMELINE_FIELDS(TimelineBezierTangent, x.timeOffset, x.valueOffset)
IISC_TIMELINE_FIELDS(TimelineTimeMapPoint, x.timelineTicks, x.sourceTicks, x.interpolation, x.incoming, x.outgoing)
IISC_TIMELINE_FIELDS(TimelineAutomationKeyframe, x.timeTicks, x.value, x.interpolation, x.incoming, x.outgoing)
IISC_TIMELINE_FIELDS(TimelineAutomationCurve, x.parameterId, x.defaultValue, x.keyframes)
IISC_TIMELINE_FIELDS(TimelineEffect, x.id, x.name, x.category, x.pluginId, x.vendor, x.schemaVersion, x.enabled, x.bypass, x.activeRange, x.parameters, x.automation, x.inputIds, x.metadata)
IISC_TIMELINE_FIELDS(TimelineMarker, x.id, x.name, x.kind, x.range, x.color, x.comment, x.author, x.createdAt, x.tags, x.metadata)
IISC_TIMELINE_FIELDS(TimelineClipContentWindow, x.offsetTicks, x.durationTicks)
IISC_TIMELINE_FIELDS(TimelineClipProperties, x.id, x.name, x.enabled, x.source, x.timelineRange, x.sourceRange, x.playbackRate, x.loopMode, x.videoSampling, x.audioStretchMode, x.preservePitch, x.linkGroupId, x.syncGroupId, x.role, x.colorLabel, x.timeMap, x.effects, x.markers, x.metadata, x.contentWindow)
IISC_TIMELINE_FIELDS(TimelineTransform, x.position, x.anchor, x.scale, x.rotationDegrees, x.skew, x.opacity, x.blendMode)
IISC_TIMELINE_FIELDS(TimelineCrop, x.left, x.top, x.right, x.bottom, x.feather)
IISC_TIMELINE_FIELDS(TimelineAudioMix, x.gainDb, x.pan, x.phaseInverted, x.fadeInTicks, x.fadeOutTicks, x.channelMatrix, x.outputBusId)
IISC_TIMELINE_FIELDS(TimelineSubtitleStyle, x.fontFamily, x.fontSize, x.fill, x.outline, x.outlineWidth, x.background, x.bold, x.italic, x.underline, x.horizontalAlignment, x.verticalAlignment, x.writingDirection, x.properties)
IISC_TIMELINE_FIELDS(TimelineEditableVideoSource, x.ownedSnapshotId, x.sequenceId, x.clipId)
IISC_TIMELINE_FIELDS(TimelineVideoClip, x.properties, x.transform, x.crop, x.editableOriginal)
IISC_TIMELINE_FIELDS(TimelineAudioClip, x.properties, x.mix)
IISC_TIMELINE_FIELDS(TimelineSubtitleClip, x.properties, x.text, x.markup, x.style)
IISC_TIMELINE_FIELDS(TimelineDataClip, x.properties, x.format, x.payload)
IISC_TIMELINE_FIELDS(TimelineTransition, x.id, x.name, x.typeId, x.enabled, x.fromClipId, x.toClipId, x.timelineRange, x.alignment, x.parameters, x.automation, x.metadata)
IISC_TIMELINE_FIELDS(TimelineTrackEffectTimeSegment, x.outputRange, x.sourceStartTicks)
IISC_TIMELINE_FIELDS(TimelineTrackEffectTimeMap, x.sourceDurationTicks, x.segments)
IISC_TIMELINE_FIELDS(TimelineTrackProperties, x.id, x.name, x.enabled, x.locked, x.muted, x.solo, x.syncLocked, x.role, x.language, x.colorLabel, x.blendMode, x.opacity, x.outputBusId, x.effects, x.markers, x.metadata, x.effectTimeMap)
IISC_TIMELINE_FIELDS(TimelineVideoTrack, x.properties, x.clips, x.transitions)
IISC_TIMELINE_FIELDS(TimelineAudioTrack, x.properties, x.clips, x.transitions)
IISC_TIMELINE_FIELDS(TimelineSubtitleTrack, x.properties, x.clips, x.transitions)
IISC_TIMELINE_FIELDS(TimelineDataTrack, x.properties, x.clips, x.transitions)
IISC_TIMELINE_FIELDS(TimelineLinkGroup, x.id, x.clipIds, x.syncLocked)
IISC_TIMELINE_FIELDS(TimelineSequence, x.id, x.name, x.timeBase, x.editingFrameRate, x.startTicks, x.durationTicks, x.workArea, x.canvasExtent, x.pixelAspectRatio, x.color, x.audioSampleRate, x.audioChannelLayout, x.timecode, x.tracks, x.markers, x.linkGroups, x.metadata)
IISC_TIMELINE_FIELDS(TimelineBin, x.id, x.name, x.parentBinId, x.mediaSourceIds, x.sequenceIds, x.metadata)
IISC_TIMELINE_FIELDS(TimelineVideoOutput, x.codec, x.extent, x.frameRate, x.frameRateMode, x.pixelAspectRatio, x.pixelFormat, x.bitDepth, x.color, x.alphaMode, x.rateControl, x.minimumBitRate, x.averageBitRate, x.maximumBitRate, x.quality, x.gopSize, x.maximumBFrames, x.twoPass, x.encoderPreset, x.tune, x.hardwareEncoder, x.options)
IISC_TIMELINE_FIELDS(TimelineAudioOutput, x.codec, x.sampleRate, x.channelCount, x.channelLayout, x.sampleFormat, x.bitDepth, x.rateControl, x.bitRate, x.quality, x.normalizeLoudness, x.targetIntegratedLufs, x.targetTruePeakDbtp, x.resampler, x.dither, x.options)
IISC_TIMELINE_FIELDS(TimelineSubtitleOutput, x.mode, x.codec, x.format, x.language, x.defaultDisposition, x.forcedDisposition, x.sidecarUri, x.options)
IISC_TIMELINE_FIELDS(TimelineRenderProfile, x.id, x.name, x.sequenceId, x.outputUri, x.container, x.rangeMode, x.customRange, x.video, x.audio, x.subtitles, x.fastStart, x.fragmented, x.fragmentDurationTicks, x.writeChapters, x.writeTimecode, x.writeMetadata, x.muxOptions, x.metadata)
IISC_TIMELINE_FIELDS(TimelineProject, x.id, x.name, x.applicationName, x.applicationVersion, x.createdAt, x.modifiedAt, x.activeSequenceId, x.mediaSources, x.sequences, x.bins, x.renderProfiles, x.metadata)

#undef IISC_TIMELINE_FIELDS
} // namespace iiSharedCanvas::timeline_codec
