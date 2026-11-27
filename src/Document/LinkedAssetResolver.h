#pragma once
#include "Render/FrameRenderer.h"
#include "Serialization/IiscCodec.h"

namespace iiSharedCanvas {
// Result of copying a document into an independently owned linked asset.
struct LinkedCanvasCopyResult {
    LinkedCanvasAsset asset;
    IiscError error;
    [[nodiscard]] bool ok() const noexcept { return error.code == IiscErrorCode::None; }
};

// Converts documents to owned copies and resolves their selected frame without
// opening external paths or mutating either document. Callers can supply codec
// limits to bound nested data and decoding.
class IISHAREDCANVAS_EXPORT LinkedAssetResolver final {
public:
    // Validate and serialize the complete source, retaining neither references
    // nor external file paths. An invalid frame or budget yields an error.
    [[nodiscard]] static LinkedCanvasCopyResult copy(std::string id, const Document &source,
                                                      FrameIndex frame, SerializationLimits limits = {});
    // Decode and render the selected frame as detached pixels with its origin.
    // Invalid nested content and render-limit violations return a failure.
    [[nodiscard]] static FrameRenderResult resolve(const LinkedCanvasAsset &, SerializationLimits limits = {});
};
} // namespace iiSharedCanvas
