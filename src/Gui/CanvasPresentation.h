#pragma once

#include "Render/FrameRenderer.h"

#include <Input/PointerEvent.h>
#include <iiGUI/NativeEditorCanvasView.h>

#include <expected>
#include <memory>

namespace iiSharedCanvas {

enum class CanvasInputKind { Pointer, Scroll };
enum class CanvasInputError { InvalidValue };

// Detached input data. The application owns gesture policy, editor lifetime,
// ordering and synchronization; the translator never edits a document.
struct CanvasInputEvent {
    CanvasInputKind kind = CanvasInputKind::Pointer;
    PointerEvent pointer;
    std::uint64_t deviceId = 0;
    std::uint32_t nativeButtons = 0;
    // Coordinates relative to the canvas surface, independent of document transform.
    double screenX = 0, screenY = 0;
    double scrollX = 0, scrollY = 0;
    bool control = false, shift = false, alt = false, meta = false, preciseScroll = false;
};

// Stateless conversion of one iiGUI event. All values are returned explicitly;
// neither input ownership nor callbacks escape the call. Concurrent calls are safe.
class CanvasInputTranslator final {
public:
    // Converts one borrowed event without retaining references or changing the
    // document. Invalid coordinates, pressure, device, or phase return an error.
    // The caller controls delivery order; independent calls are thread-safe.
    [[nodiscard]] std::expected<CanvasInputEvent, CanvasInputError>
    execute(const ii::gui::EditorCanvasInput &input) const noexcept;
};

// Stateless view conversion. The source frame is borrowed only for this call;
// the returned iiGUI image owns an independent pixel buffer. The caller renders
// the requested region and controls lifetime, scheduling, and view publication.
class CanvasFramePresenter final {
public:
    // The frame origin must match sampledRegion; its pixels must cover that
    // region. Failure returns an error without publishing a partial image.
    // The detached result is safe to retain after the borrowed inputs expire.
    [[nodiscard]] ii::core::Result<std::shared_ptr<const ii::gui::WidgetDrawing>> execute(
        const FrameRenderResult &frame, CanvasRegion sampledRegion,
        const ii::gui::EditorCanvasQuery &query,
        RasterSampling sampling = RasterSampling::Nearest,
        std::uint64_t maximumPixels = ii::gui::maximum_image_pixels) const;
};

} // namespace iiSharedCanvas
