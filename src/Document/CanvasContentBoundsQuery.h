#pragma once

#include "Document/TransparentBoundsScanner.h"
#include "Render/FrameRenderer.h"
#include "iiSharedCanvas/Export.h"

#include <optional>
#include <span>
#include <vector>

namespace iiSharedCanvas {

enum class CanvasContentBoundsScope { VisibleComposite, AllLayers };
struct CanvasContentBoundsRequest {
    CanvasContentBoundsScope scope=CanvasContentBoundsScope::VisibleComposite;
    // Omission means the document view for VisibleComposite. AllLayers requires
    // an explicit bounded search region, which may extend outside the canvas.
    std::optional<CanvasRegion> region;
    // Empty means every timeline frame; otherwise entries must be distinct.
    std::vector<FrameIndex> frames;
    std::uint8_t threshold=0;
    std::size_t maximumPixels=64ULL*1024*1024;
};

// Combines externally rendered, detached samples. The caller determines their
// scope and owns the inputs; no renderer, document or file is accessed here.
class IISHAREDCANVAS_EXPORT CanvasContentBoundsQuery final {
public:
    // Returns the union of pixels whose alpha exceeds threshold. Invalid
    // samples, coordinate overflow or an exhausted work budget fail closed.
    [[nodiscard]] CanvasBoundsResult operator()(std::span<const FrameRenderResult> samples,
        std::uint8_t threshold=0,std::size_t maximumPixels=64ULL*1024*1024) const;
};

// External controller: renders the requested frames and invokes the detached
// query. AllLayers temporarily exposes hidden artwork in an owned document copy;
// it unions each layer independently within the explicit search region. Source
// data, history and file bindings are never changed. The copy can be as large as
// the input document; maximumPixels bounds rendering work, not allocator RSS.
[[nodiscard]] IISHAREDCANVAS_EXPORT CanvasBoundsResult queryCanvasContentBounds(
    const Document &,const CanvasContentBoundsRequest &);

} // namespace iiSharedCanvas
