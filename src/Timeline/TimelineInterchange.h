#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"
#include "Media/MediaIo.h"

#include <cstdint>
#include <string>

namespace iiSharedCanvas {

struct TimelineInterchangeOptions {
    std::string sequenceName = "iisc Timeline";
    // Hold clips do not allocate one image per frame; allow ordinary long edits.
    MediaLimits limits{.maxFrames = 1'000'000};
    std::uint32_t maxLayers = 4096;
    std::uint32_t maxClips = 65536;
};

// Publishes a NEW directory containing timeline.xml (legacy FCP XML),
// timeline.fcpxml, per-layer PNG/WAV media, a manifest and a native source snapshot.
// No existing directory is overwritten. Native documents are never mutated.
IISHAREDCANVAS_EXPORT MediaIoResult exportTimelineInterchange(
    const Document &document, const std::string &directory,
    const TimelineInterchangeOptions &options = {});

} // namespace iiSharedCanvas
