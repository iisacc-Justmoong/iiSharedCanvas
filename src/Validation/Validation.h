#pragma once

#include "Document/Document.h"
#include "iiSharedCanvas/Export.h"

#include <string>
#include <vector>

namespace iiSharedCanvas {

enum class ValidationCode {
    UnsupportedFormatVersion,
    InvalidCanvasExtent,
    InvalidInfiniteCanvas,
    InvalidTimeline,
    InvalidAssetId,
    DuplicateAssetId,
    InvalidRasterAsset,
    InvalidRasterChunk,
    InvalidVectorAsset,
    DuplicateLayerId,
    InvalidLayer,
    InvalidLayerFrameRange,
    MissingAsset,
    InvalidKeyframes,
    ContentKindMismatch,
    InvalidStableDiffusionMetadata,
};

struct ValidationIssue {
    ValidationCode code;
    std::string path;
    std::string message;
};

struct ValidationResult {
    std::vector<ValidationIssue> issues;

    bool ok() const noexcept { return issues.empty(); }
};

IISHAREDCANVAS_EXPORT ValidationResult validate(const Document &document);

} // namespace iiSharedCanvas
