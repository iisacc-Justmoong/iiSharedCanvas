#pragma once

#include "Document/Document.h"
#include "Serialization/IiscCodec.h"
#include "iiSharedCanvas/Export.h"
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iiSharedCanvas {

// A detached instruction to render one nested video clip. The source project,
// owned media and portable IISC snapshot retain the original composition.
// For each i in [0, frameCount), render the isolated parent clip at absolute
// sequence frame firstSequenceFrame + i, including any transition handles
// outside its visible range. The caller owns and orders the work.
struct SequenceRasterFlattenPreparation {
    std::string sequenceId, clipId, sourceSequenceId, sourceClipId;
    TimelineProject expectedProject;
    std::vector<TimelineOwnedMedia> expectedMedia;
    std::vector<std::uint8_t> snapshot;
    TimelineExtent extent;
    TimelineFrameRate frameRate;
    TimelineTimeBase outputTimeBase;
    std::int64_t outputDurationTicks=0, firstOffsetTicks=0, frameStepTicks=0;
    TimelineTickRange visibleSourceRange;
    std::uint64_t firstSequenceFrame=0, frameCount=0;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty() && frameCount>0 && !snapshot.empty(); }
};
struct SequenceRasterFlattenIds { std::string source, representation, stream, ownedMedia, originalSnapshotMedia; };
struct SequenceRasterFlattenResult {
    std::optional<TimelineProject> project;
    std::vector<TimelineOwnedMedia> media;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return project.has_value() && error.empty(); }
};
struct SequenceRasterFlattenWriteResult {
    bool noteWritten=false, snapshotWritten=false;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return noteWritten && snapshotWritten && error.empty(); }
};
struct RawSequenceFrameResult {
    RasterLayer pixels;
    std::string error;
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Prepare a self-contained source snapshot and exact sequence-frame grid.
// The input is borrowed only during the call. The result owns its snapshot and
// model. No renderer, file system, global state or implicit resource is used.
// Invalid references, non-video clips, and exhausted limits return an error.
class IISHAREDCANVAS_EXPORT SequenceRasterFlattenPrepare final {
public:
    [[nodiscard]] SequenceRasterFlattenPreparation operator()(const Document &,const std::string &sequenceId,
        const std::string &clipId,std::uint64_t maximumFrames,std::uint64_t maximumSnapshotBytes) const;
};

// Write the editable-group design note to an app-provided repository path and
// its source snapshot to an app-provided temporary-storage path. Both paths
// must be new; neither is overwritten. Each path is an explicit I/O grant.
// On partial failure the result identifies created files for caller cleanup.
class IISHAREDCANVAS_EXPORT SequenceRasterFlattenTemporaryWriter final {
public:
    [[nodiscard]] SequenceRasterFlattenWriteResult operator()(const SequenceRasterFlattenPreparation &,
        const std::filesystem::path &snapshotPath,const std::filesystem::path &notePath) const;
};

// Turn exactly the supplied straight-ARGB frames into an owned, lossless raw
// sequence and a detached project. The original IISC snapshot is also copied
// into document-owned media and linked from the rendered clip. The renderer has already rendered
// the isolated clip in sequence canvas coordinates, before its final blend
// against outer tracks. The caller publishes the returned model and media via
// DocumentHistory as one entry. Source mismatch or any invalid/over-budget
// frame leaves the document untouched and produces no partial project.
class IISHAREDCANVAS_EXPORT SequenceRasterFlattenCommit final {
public:
    [[nodiscard]] SequenceRasterFlattenResult operator()(const Document &,const SequenceRasterFlattenPreparation &,
        std::span<const RasterLayer> frames,const SequenceRasterFlattenIds &,std::uint64_t maximumBytes) const;
};

// Decode the document-owned original associated with a rendered clip. This
// does not use the temporary file, so moving the IISC document preserves the
// complete editable source. The caller owns the returned document and chooses
// how to edit or publish it. Invalid references and decoder limits are reported.
class IISHAREDCANVAS_EXPORT SequenceRasterFlattenOriginal final {
public:
    [[nodiscard]] IiscDecodeResult operator()(const Document &,const std::string &sequenceId,
                                               const std::string &clipId,SerializationLimits limits = {}) const;
};

// Return a detached replacement for the document-owned editable snapshot. The
// rendered pixels remain visible until the caller prepares and publishes a new
// render. The caller publishes this result through one document history entry.
// Invalid original references or snapshot budgets fail without mutation.
class IISHAREDCANVAS_EXPORT SequenceRasterFlattenReplaceOriginal final {
public:
    [[nodiscard]] SequenceRasterFlattenResult operator()(const Document &,const std::string &sequenceId,
        const std::string &clipId,const Document &editedOriginal,std::uint64_t maximumSnapshotBytes) const;
};

// Decode one frame from the documented lossless owned raw sequence. Bytes are
// borrowed for this call; the returned frame owns its pixels. This operation
// does not load codecs or read files. Malformed or excessive inputs fail.
class IISHAREDCANVAS_EXPORT RawSequenceFrameReader final {
public:
    [[nodiscard]] RawSequenceFrameResult operator()(std::span<const std::uint8_t>,std::uint64_t frame,
                                                      std::size_t maximumPixels) const;
};
} // namespace iiSharedCanvas
