# iiSharedCanvas

iiSharedCanvas is a C++20 document and authoring foundation for composing
raster artwork, native vector paths, and frame-based raster or vector animation
on one canvas.

The repository contains the versioned in-memory model, validation rules,
deterministic keyframe evaluation, an editable raster-asset boundary, a Qt
Quick bitmap display item, CMake package export, and contract tests. File
serialization and full mixed-layer frame rendering remain later milestones and
are not claimed as complete.

## Content contract

| Authoring input | Stored render content | Initial behavior |
| --- | --- | --- |
| Bitmap image | iiPaintEngine RasterLayer | Static layer |
| iiPaintEngine brush | Committed ARGB pixels | Static or keyframed raster asset |
| Vector path | M/L/Q/C/Z path commands with solid fill or stroke | Static layer |
| Raster keyframes | Raster asset references at integer frames | Hold sampling |
| Vector keyframes | Vector asset references at integer frames | Hold sampling |

Brush trajectories, dab sequences, and replay commands are never part of the
iiSharedCanvas format. iiPaintEngine rasterizes brush input immediately, and
iiSharedCanvas receives only the committed pixels. This keeps raster editing
compatible with iiPaintEngine's bitmap-only contract.

## Bitmap display and editing

`BitmapEditor` binds to a raster asset by id and mutates that `RasterAsset`
directly. It supports individual pixels, rectangular ARGB patches, whole
`RasterLayer` replacement, clear, streamed brush/eraser input, dirty bounds,
and undo/redo. Brush input is rasterized immediately through iiPaintEngine;
only the resulting pixels live in the document. One brush gesture is one undo
operation. The initial history policy retains at most 32 full raster snapshots
for predictable behavior; patch-budget optimization remains a measured product
hardening task.

`BitmapItem` is a `QQuickPaintedItem` display and input adapter. It can bind an
existing document raster asset or create an owned bitmap, renders ARGB pixels
without smoothing, supports zoom and pan, and routes mouse or explicit
pressure-bearing stroke calls into `BitmapEditor`. This displays one selected
raster asset; it is not the future full document frame compositor. If another
owner mutates a bound document directly, the UI owner must call `refresh()` on
the GUI thread.

## Dependency

The only direct project dependency is iiPaintEngine 0.1.0. Its exported CMake
target supplies the raster types, rasterizer, blend modes, transforms, and its
Qt Core/Gui/Qml/Quick platform targets transitively. `BitmapItem` links those
already-supplied Qt targets; iiSharedCanvas performs no second package
discovery.

## Build and test

~~~sh
cmake --fresh --preset dev
cmake --build --preset dev
ctest --preset dev
~~~

The equivalent explicit commands are:

~~~sh
cmake --fresh -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
~~~

iiPaintEngine is discovered from CMAKE_PREFIX_PATH or, for the local developer
layout, from $HOME/.local/iiPaintEngine.
The generated library records the selected iiPaintEngine runtime directory and
the standard sibling-install fallback so Debug, Release, and installed
consumers resolve the same engine binary.

For a tested host install, including an installed-package consumer check:

~~~sh
./install.sh
~~~

## Source layout

Public headers and their implementations share module directories under
`src/`. The source tree does not maintain a separate `include/` directory;
installed packages still place public headers under the platform-standard
include prefix.

~~~text
src/
  iiSharedCanvas.h
  Bitmap/
    BitmapEditor.h
    BitmapEditor.cpp
  Document/
    Document.h
    Document.cpp
  QtAdapter/
    BitmapItem.h
    BitmapItem.cpp
  Validation/
    Validation.h
    Validation.cpp
~~~

## Minimal model usage

~~~cpp
#include <iiSharedCanvas.h>

using namespace iiSharedCanvas;

Document document;
document.extent = {1920, 1080};
document.timeline = {{24, 1}, 48};
document.assets.emplace_back(
    RasterAsset{"frame-0", makeRasterLayer(1920, 1080)});
document.layers.push_back({
    "paint",
    "Paint",
    true,
    1.0,
    {},
    RasterBlendMode::SourceOver,
    KeyframedSource{ContentKind::Raster, {{0, "frame-0"}}},
});

if (!validate(document).ok()) {
    // Reject before serialization or rendering.
}

BitmapEditor editor(document, "frame-0");
editor.setPixel(10, 10, 0xffffcc00U);
editor.beginStroke({20.0, 20.0}, 1.0);
editor.continueStroke({80.0, 40.0}, 1.0);
editor.endStroke({80.0, 40.0}, 1.0);
~~~

For Qt Quick hosts, call `registerIiSharedCanvasQmlTypes()` before loading the
engine and use the `iiSharedCanvas 1.0` `Bitmap` type. Product QML must continue
to use LVRS for the application UI; this library provides the canvas item, not
a second UI framework.

`iiSharedCanvas.BitmapItemRender` performs an offscreen real render and writes
`build/test-output/bitmap-item.png`. Pixel assertions verify the original and
edited ARGB values as well as nearest-neighbor zoom.

## Documents

- docs/BLUEPRINT.md defines ownership, dependency direction, milestones, and
  completion gates.
- docs/FORMAT.md defines the draft .iisc logical package and manifest contract.
- AGENTS.md fixes the engineering rules that future changes must preserve.

## License

The project is AGPL-3.0-only because iiPaintEngine is an AGPL-3.0-only public
dependency. See LICENSE and NOTICE.md.
