#include "CanvasEditorView.generated.h"
#include "WidgetState.h" // iiGUI's headless test surface; no OS window is opened.

#include <Bitmap/BitmapEditor.h>
#include <Gui/CanvasInteraction.h>
#include <Gui/CanvasNativeView.h>
#include <Gui/CanvasPresentation.h>
#include <Gui/CanvasTileCache.h>
#include <Render/FrameRenderer.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template<class T> T take(ii::core::Result<T> value)
{
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(*value);
}
void good(ii::core::Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
std::shared_ptr<const ii::gui::RasterImage> frameImage(const ii::gui::CanvasFrame &frame)
{
    if (!frame.images.empty()) return frame.images.front().image;
    for (const auto &overlay : frame.overlays)
        if (!overlay.images.empty()) return overlay.images.front().image;
    return {};
}
bool containsImage(const ii::gui::CanvasFrame &frame, const std::shared_ptr<const ii::gui::RasterImage> &image)
{
    for (const auto &run : frame.images) if (run.image == image) return true;
    for (const auto &overlay : frame.overlays)
        for (const auto &run : overlay.images) if (run.image == image) return true;
    return false;
}

}

int main()
{
    try {
        using namespace iiSharedCanvas;
        using namespace ii::gui;
        Document document; document.extent = {12, 8};
        document.assets.emplace_back(RasterAsset{"paint", makeRasterLayer(12, 8, 0xff000000)});
        document.layers.emplace_back(StaticBitmapLayer{{"layer"}, StaticSource{"paint"}});
        BitmapEditor editor(document, "paint");
        BitmapBrush brush; brush.argb = 0xffff0000; brush.size = 2;
        require(editor.setBrush(brush), "brush setup");
        const auto before = std::get<RasterAsset>(document.assets.front()).pixels.pixels;
        CanvasInputTranslator translator;
        CanvasInteractionRouter router;
        CanvasInteractionState gesture;
        CanvasInteractionOptions policy; policy.rasterLayerSelected = true;
        CanvasTilePresenter presenter;
        CanvasTileCacheUpdater updater;
        CanvasDirtyTilePlanner planner;
        CanvasTileCache cache; cache.revision = 1;
        const std::vector<FrameRenderTileRequest> requests{
            {{{0, 0}, {4, 8}}, {4, 8}}, {{{4, 0}, {4, 8}}, {4, 8}}, {{{8, 0}, {4, 8}}, {4, 8}}};
        auto rendered = renderFrameTiles(document, 0, requests);
        require(rendered.ok(), "initial canvas tiles did not render");
        cache = take(updater.execute(cache, 1, 1, {}, rendered.tiles, 3, 96));
        auto unchangedTile = cache.tiles[2].image;
        auto mounted = std::make_shared<CanvasViewMount>();
        NativeEditorCanvasCallbacks callbacks;
        callbacks.query = [&](const EditorCanvasQuery &query)
            -> ii::core::Result<std::shared_ptr<const WidgetDrawing>> {
            return presenter.execute(cache, query);
        };
        callbacks.input = [&](const EditorCanvasInput &event) -> ii::core::Result<void> {
            auto input = translator.execute(event);
            if (!input) return ii::core::fail(ii::core::ErrorCode::invalid_argument, "invalid input");
            auto current = mounted->canvas->options();
            if (!current) return std::unexpected(current.error());
            auto transition = router.execute(gesture, current->transform, policy, *input);
            if (!transition) return ii::core::fail(ii::core::ErrorCode::invalid_argument, "invalid gesture");
            gesture = transition->state;
            if (transition->viewportChanged) {
                current->transform = transition->viewport;
                if (auto changed = mounted->canvas->set_options(*current); !changed) return changed;
            }
            if (!transition->stroke) return {};
            const auto &p = *transition->stroke;
            StrokePoint sample{p.documentPosition, p.pressure, p.time};
            sample.tiltX = p.tiltX; sample.tiltY = p.tiltY;
            sample.rotationRadians = p.rotationRadians; sample.deviceState = p.deviceState;
            bool accepted = true;
            switch (p.phase) {
            case PointerEventPhase::Press: accepted = editor.beginStrokeInput(sample); break;
            case PointerEventPhase::Move: accepted = editor.continueStrokeInput(sample); break;
            case PointerEventPhase::Release: accepted = editor.endStrokeInput(sample); break;
            case PointerEventPhase::Cancel: editor.cancelStroke(); break;
            }
            if (!accepted) return ii::core::fail(ii::core::ErrorCode::failed, editor.lastError());
            return {};
        };
        AccessibilitySemantics accessibility;
        accessibility.role = AccessibilityRole::image;
        accessibility.name = "Editable canvas";
        accessibility.description = "Artwork surface";
        auto source = std::make_shared<CanvasNativeViewSource>(
            EditorCanvasOptions{.width = 12, .height = 8, .revision = 1}, callbacks, mounted, "drawing", accessibility);

        auto tree = std::make_shared<ii::gui::detail::WidgetState>([](ii::runtime::Task task) { task(); });
        auto host = take(tree->content_host({}, "host", {12, 8}));
        good(host.set_content([&](WidgetBuilder &widgets) {
            return lvui_generated::CanvasEditorView::build(widgets, {}, [&](auto &scope) {
                scope.backend_0.canvas = source;
                scope.backend_0.width.set(12); scope.backend_0.height.set(8);
            });
        }));
        require(mounted->canvas.has_value(), "LVUI did not mount the editor source");
        const auto semantics = take(mounted->canvas->root().snapshot()).accessibility_semantics;
        require(semantics && semantics->name == "Editable canvas" && semantics->description == "Artwork surface",
                "caller-provided accessibility semantics must reach the iiGUI canvas");
        const auto initial = take(tree->drawing_snapshot({12, 8}));
        const auto initialImage = frameImage(initial);
        require(bool(initialImage), "LVUI/iiGUI drawing did not contain the canvas image");
        good(tree->input({.kind = WidgetInputKind::pointer_down, .x = 2, .y = 2,
                          .device = PointerDevice::mouse, .timestamp = .5, .buttons = 4}));
        good(tree->input({.kind = WidgetInputKind::pointer_move, .x = 3, .y = 2,
                          .device = PointerDevice::mouse, .timestamp = .55, .buttons = 4}));
        good(tree->input({.kind = WidgetInputKind::pointer_up, .x = 3, .y = 2,
                          .device = PointerDevice::mouse, .timestamp = .6}));
        require(std::get<RasterAsset>(document.assets.front()).pixels.pixels == before,
                "middle-button navigation must not start a brush stroke");
        auto options = take(mounted->canvas->options());
        require(options.transform.offset_x == 1, "middle-button navigation must move the viewport");
        options.transform = {}; good(mounted->canvas->set_options(options));
        good(tree->input({.kind = WidgetInputKind::pointer_down, .x = 2, .y = 2,
                          .device = PointerDevice::pen, .pressure = .8, .timestamp = 1, .device_id = 7, .buttons = 1}));
        good(tree->input({.kind = WidgetInputKind::pointer_move, .x = 3, .y = 2,
                          .device = PointerDevice::pen, .pressure = .8, .timestamp = 2, .device_id = 7, .buttons = 1}));
        good(tree->input({.kind = WidgetInputKind::pointer_up, .x = 3, .y = 2,
                          .device = PointerDevice::pen, .pressure = .8, .timestamp = 3, .device_id = 7}));
        require(std::get<RasterAsset>(document.assets.front()).pixels.pixels != before && editor.canUndo(),
                "pen input did not edit the selected bitmap or create undo history");
        const auto &afterStroke = std::get<RasterAsset>(document.assets.front()).pixels.pixels;
        int minX = 12, minY = 8, maxX = -1, maxY = -1;
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 12; ++x)
            if (afterStroke[std::size_t(y * 12 + x)] != before[std::size_t(y * 12 + x)]) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        for (int y = 0; y < 8; ++y) for (int x = 8; x < 12; ++x)
            require(afterStroke[std::size_t(y * 12 + x)] == before[std::size_t(y * 12 + x)],
                    "the retained tile must not overlap the edited stroke");
        const auto dirty = editor.dirtyBounds();
        require(dirty.origin.x == minX && dirty.origin.y == minY
                    && dirty.width == maxX - minX + 1 && dirty.height == maxY - minY + 1,
                "dirty bounds must enclose exactly the changed pixels");
        const std::array changed{CanvasRegion{{dirty.origin.x, dirty.origin.y}, {dirty.width, dirty.height}}};
        const auto dirtyRequests = take(planner.execute(cache, 1, changed, RasterSampling::Nearest, 3));
        if (dirtyRequests.size() != 2)
            std::cerr << "dirty bounds=" << dirty.origin.x << ',' << dirty.origin.y << ' '
                      << dirty.width << 'x' << dirty.height << " requests=" << dirtyRequests.size() << '\n';
        require(dirtyRequests.size() == 2, "a stroke crossing the tile boundary must request both affected tiles");
        rendered = renderFrameTiles(document, 0, dirtyRequests);
        require(rendered.ok(), "changed tile did not render");
        cache = take(updater.execute(cache, 1, 2, changed, rendered.tiles, 3, 96));
        options = take(mounted->canvas->options()); ++options.revision;
        good(mounted->canvas->set_options(options));
        const auto after = take(tree->drawing_snapshot({12, 8}));
        require(frameImage(after) && frameImage(after) != initialImage,
                "document revision did not publish a new drawing");
        require(containsImage(after, unchangedTile), "unchanged image was not reused in the LVUI tree");
        editor.clearDirtyBounds();
        require(editor.undo() && std::get<RasterAsset>(document.assets.front()).pixels.pixels == before,
                "canvas input must retain undo");
        const auto undoDirty = editor.dirtyBounds();
        require(undoDirty.origin.x == minX && undoDirty.origin.y == minY
                    && undoDirty.width == maxX - minX + 1 && undoDirty.height == maxY - minY + 1,
                "undo must report the actual changed bounds");
        good(host.clear_content());
        require(!mounted->canvas->options(), "retired view handle must report closed");
        tree->close();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
