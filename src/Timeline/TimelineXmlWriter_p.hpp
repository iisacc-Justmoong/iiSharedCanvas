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

struct InterchangePlan {
    std::string name;
    FrameRate frameRate;
    CanvasExtent extent;
    FrameIndex frameCount = 1;
    std::vector<InterchangeTrack> tracks; // Bottom to top, including hidden tracks.
    std::vector<InterchangeMedia> media;
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
