#pragma once

#include <iiGUI/NativeEditorCanvasView.h>
#include <iiGUI/NativeView.h>

#include <memory>
#include <optional>
#include <string>

namespace iiSharedCanvas {

// Caller-owned publication slot for the latest successfully built candidate.
// The weak iiGUI handle does not keep a widget alive. A candidate can later be
// retired if a containing LVUI tree fails; callers must handle a closed handle.
// Access is confined to the iiGUI owner thread and must be externally serialized.
struct CanvasViewMount {
    std::optional<ii::gui::NativeEditorCanvas> canvas;
};

// Adapts an application-supplied canvas query and input boundary to an LVUI
// NativeView. It owns no document or editor: callback captures and their
// lifetime, side effects, and scheduling are the application's responsibility.
class CanvasNativeViewSource final : public ii::gui::NativeViewSource {
public:
    // Values and callbacks are copied into each iiGUI widget generation. The
    // optional publication slot is explicit data access, not widget ownership.
    CanvasNativeViewSource(ii::gui::EditorCanvasOptions options,
                           ii::gui::NativeEditorCanvasCallbacks callbacks,
                           std::shared_ptr<CanvasViewMount> mount,
                           std::string widgetId,
                           std::optional<ii::gui::AccessibilitySemantics> accessibility = {});

    // Builds a single root using the borrowed builder, which is never retained.
    // On construction failure the publication slot is unchanged. Must execute
    // on the builder's owner thread; concurrent calls need external ordering.
    [[nodiscard]] ii::core::Result<void> build(ii::gui::WidgetBuilder &builder) const override;

private:
    ii::gui::EditorCanvasOptions options_;
    ii::gui::NativeEditorCanvasCallbacks callbacks_;
    std::shared_ptr<CanvasViewMount> mount_;
    std::string widgetId_;
    std::optional<ii::gui::AccessibilitySemantics> accessibility_;
};

} // namespace iiSharedCanvas
