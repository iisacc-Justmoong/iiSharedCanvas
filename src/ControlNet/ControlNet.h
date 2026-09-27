#pragma once
#include <string>

namespace iiSharedCanvas {

// A conditioning contract, not an inference runtime or an implicit model download.
struct ControlNetSettings {
    bool enabled = true;
    std::string modelId;
    std::string modelRevision;
    double conditioningScale = 1.0;
    double guidanceStart = 0.0;
    double guidanceEnd = 1.0;
    friend bool operator==(const ControlNetSettings &, const ControlNetSettings &) = default;
};

} // namespace iiSharedCanvas
