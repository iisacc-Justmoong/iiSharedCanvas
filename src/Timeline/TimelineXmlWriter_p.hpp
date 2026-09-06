#pragma once

#include "TimelineInterchange.h"

#include <QByteArray>
#include <QString>

#include <vector>

namespace iiSharedCanvas::timeline_detail {

struct InterchangeClip {
    FrameIndex start = 0;
    FrameIndex duration = 0;
    std::size_t mediaIndex = 0;
    std::string assetId;
};

struct InterchangeTrack {
    std::string id;
    std::string name;
    bool visible = true;
    double opacity = 1.0;
    RasterBlendMode blendMode = RasterBlendMode::SourceOver;
    std::vector<InterchangeClip> clips;
};

struct InterchangeMedia {
    QString relativePath;
};

struct InterchangeAudioMedia {
    QString relativePath;
    std::uint32_t sampleRate = 48000;
    std::uint16_t channelCount = 2;
    std::uint64_t sampleFrameCount = 0;
};

struct InterchangeAudioClip {
    FrameIndex start = 0;
    FrameIndex duration = 0;
    std::size_t mediaIndex = 0;
    std::string id;
    std::string name;
    std::string assetId;
    // The package remaps non-frame-aligned native source trims onto a WAV
    // suffix, preserving exact samples without relying on legacy subframes.
    std::uint64_t sourceOffsetSamples = 0;
    double gainDb = 0;
    bool enabled = true;
};

struct InterchangeAudioTrack {
    std::string id;
    std::string name;
    bool muted = false;
    double gainDb = 0;
    std::vector<InterchangeAudioClip> clips;
};

struct InterchangePlan {
    std::string name;
    FrameRate frameRate;
    CanvasExtent extent;
    FrameIndex frameCount = 1;
    std::vector<InterchangeTrack> tracks; // Bottom to top, including hidden tracks.
    std::vector<InterchangeMedia> media;
    std::vector<InterchangeAudioTrack> audioTracks; // Native order, separate from visual layers.
    std::vector<InterchangeAudioMedia> audioMedia;
};

struct TimelineXmlResult {
    QByteArray legacyXml;
    QByteArray fcpxml;
    MediaIoResult result;
};

TimelineXmlResult encodeTimelineXml(const InterchangePlan &plan,
                                    const QString &finalDirectory,
                                    std::uint64_t maxBytes);

} // namespace iiSharedCanvas::timeline_detail
