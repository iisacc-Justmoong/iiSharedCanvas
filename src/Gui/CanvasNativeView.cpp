#include "Gui/CanvasNativeView.h"

#include <utility>

namespace iiSharedCanvas {

CanvasNativeViewSource::CanvasNativeViewSource(ii::gui::EditorCanvasOptions options,
    ii::gui::NativeEditorCanvasCallbacks callbacks, std::shared_ptr<CanvasViewMount> mount,
    std::string widgetId, std::optional<ii::gui::AccessibilitySemantics> accessibility)
    : options_(std::move(options)), callbacks_(std::move(callbacks)),
      mount_(std::move(mount)), widgetId_(std::move(widgetId)), accessibility_(std::move(accessibility))
{
}

ii::core::Result<void> CanvasNativeViewSource::build(ii::gui::WidgetBuilder &builder) const
{
    auto canvas = ii::gui::NativeEditorCanvas::create(builder, {}, widgetId_, options_, callbacks_);
    if (!canvas) return std::unexpected(canvas.error());
    if (accessibility_) {
        auto assigned = canvas->root().set_accessibility_semantics(accessibility_);
        if (!assigned) return std::unexpected(assigned.error());
    }
    if (mount_) mount_->canvas = std::move(*canvas);
    return {};
}

} // namespace iiSharedCanvas
