#pragma once

#include "File/DocumentFile.h"

#include <string>

namespace iiSharedCanvas {

struct IiscUpgradeResult {
    DocumentFileCode code = DocumentFileCode::None;
    bool changed = false;
    FormatVersion sourceVersion;
    std::string outputPath;
    std::string message;
    [[nodiscard]] bool ok() const noexcept { return code == DocumentFileCode::None; }
};

// UTF-8 local paths. An empty output upgrades in place; an explicit output must
// not exist. Preserve snapshot/working storage kind and all document authorship.
// The current model is a no-op in place. No application or GUI initialization.
IISHAREDCANVAS_EXPORT IiscUpgradeResult upgradeIiscFile(
    const std::string &inputPath, const std::string &outputPath = {}, SerializationLimits limits = {});

} // namespace iiSharedCanvas
