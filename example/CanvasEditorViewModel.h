#pragma once

#include <iiGUI/NativeView.h>
#include <iiSDKCore/Core/Property/Observable.h>

#include <memory>

// The application owns the source and its document. LVUI observes only the
// supplied layout values; editing and rendering are orchestrated externally.
struct CanvasEditorViewModel final {
    std::shared_ptr<ii::gui::NativeViewSource> canvas;
    ii::core::Observable<double> width{0};
    ii::core::Observable<double> height{0};
};
