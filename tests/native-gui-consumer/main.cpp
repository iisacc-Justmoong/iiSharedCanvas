#include <iiSharedCanvas>

#include <iiGUI/NativeEditorCanvasView.h>

int main()
{
    iiSharedCanvas::CanvasInputTranslator translator;
    ii::gui::EditorCanvasInput input;
    input.phase = ii::gui::EditorCanvasPhase::begin;
    input.document = {2, 3};
    input.native.buttons = 1;
    auto output = translator.execute(input);
    if (!output || output->pointer.phase != PointerEventPhase::Press
        || output->pointer.documentPosition.x != 2) return 1;
    iiSharedCanvas::CanvasInteractionOptions policy; policy.rasterLayerSelected = true;
    const auto action = iiSharedCanvas::CanvasInteractionRouter{}.execute({}, {}, policy, *output);
    if (!action || !action->stroke) return 1;
    const auto cache = iiSharedCanvas::CanvasTileCacheUpdater{}.execute({}, 0, 1, {}, {}, 4, 16);
    if (!cache || cache->revision != 1) return 1;
    const auto dirty = iiSharedCanvas::CanvasDirtyTilePlanner{}.execute(
        *cache, 1, {}, iiSharedCanvas::RasterSampling::Nearest, 4);
    if (!dirty || !dirty->empty()) return 1;
    const auto viewport = iiSharedCanvas::CanvasViewportPlanner{}.execute({}, {});
    if (!viewport || viewport->scale != 1) return 1;
    auto mount = std::make_shared<iiSharedCanvas::CanvasViewMount>();
    iiSharedCanvas::CanvasNativeViewSource source({}, {}, mount, "canvas");
    return 0;
}
