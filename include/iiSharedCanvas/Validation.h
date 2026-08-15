#pragma once

#include "iiSharedCanvas/Document.h"
#include "iiSharedCanvas/Export.h"

#include <string>
#include <vector>

namespace iiSharedCanvas {

enum class ValidationCode {
    UnsupportedFormatVersion,
    InvalidCanvasExtent,
    InvalidTimeline,
    InvalidAssetId,
    DuplicateAssetId,
    InvalidRasterAsset,
    InvalidVectorAsset,
    DuplicateLayerId,
    InvalidLayer,
    MissingAsset,
    InvalidKeyframes,
    ContentKindMismatch,
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
