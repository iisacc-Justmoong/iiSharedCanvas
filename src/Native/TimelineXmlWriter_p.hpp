#pragma once

#include "Timeline/TimelineInterchange.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace iiSharedCanvas::timeline_detail {

struct InterchangeClip {
    FrameIndex start = 0, duration = 0;
    std::size_t mediaIndex = 0;
    std::string assetId;
};
struct InterchangeTrack {
    std::string id, name;
    bool visible = true;
    double opacity = 1.0;
    RasterBlendMode blendMode = RasterBlendMode::SourceOver;
    std::vector<InterchangeClip> clips;
};
struct InterchangeMedia { std::string relativePath; };
struct InterchangeAudioMedia {
    std::string relativePath;
    std::uint32_t sampleRate = 48000;
    std::uint16_t channelCount = 2;
    std::uint64_t sampleFrameCount = 0;
};
struct InterchangeAudioClip {
    FrameIndex start = 0, duration = 0;
    std::size_t mediaIndex = 0;
    std::string id, name, assetId;
    std::uint64_t sourceOffsetSamples = 0;
    double gainDb = 0;
    bool enabled = true;
};
struct InterchangeAudioTrack {
    std::string id, name;
    bool muted = false;
    double gainDb = 0;
    std::vector<InterchangeAudioClip> clips;
};
struct InterchangePlan {
    std::string name;
    FrameRate frameRate;
    CanvasExtent extent;
    FrameIndex frameCount = 1;
    std::vector<InterchangeTrack> tracks;
    std::vector<InterchangeMedia> media;
    std::vector<InterchangeAudioTrack> audioTracks;
    std::vector<InterchangeAudioMedia> audioMedia;
};
struct TimelineXmlResult {
    std::string legacyXml, fcpxml;
    MediaIoResult result;
};

bool validTimelineXmlText(std::string_view value);
TimelineXmlResult encodeTimelineXml(const InterchangePlan &plan,
                                    const std::string &finalDirectory,
                                    std::uint64_t maxBytes);

} // namespace iiSharedCanvas::timeline_detail
