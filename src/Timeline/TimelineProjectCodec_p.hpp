#pragma once

#include "Serialization/IiscCodec.h"

namespace iiSharedCanvas::detail {

// Versioned, bounded encoding of the complete timeline value. Media payloads
// are separate IISC records so editing metadata does not duplicate video bytes.
struct TimelineProjectEncodeResult {
    std::vector<std::uint8_t> bytes;
    IiscError error;
};
struct TimelineProjectDecodeResult {
    TimelineProject project;
    IiscError error;
};

// Input values and byte spans are borrowed only until the synchronous return.
// Both results own their data and report malformed/over-budget inputs without
// partial publication or access to files, global state, or codec backends.
TimelineProjectEncodeResult encodeTimelineProject(const TimelineProject &, const SerializationLimits &);
TimelineProjectDecodeResult decodeTimelineProject(std::span<const std::uint8_t>, const SerializationLimits &);
// Measures all nested textual values against the document string budgets.
IiscError measureTimelineStrings(const TimelineProject &, const SerializationLimits &, std::uint64_t &total);

} // namespace iiSharedCanvas::detail
