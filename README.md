# iiSharedCanvas

iiSharedCanvas is a C++20 document model for composing raster artwork, native
vector paths, and frame-based raster or vector animation on one canvas.

The repository is currently at the blueprint/bootstrap milestone. It contains
the versioned in-memory model, validation rules, deterministic keyframe
evaluation, CMake package export, and contract tests. File serialization and
frame rendering are intentionally the next milestones, not features claimed as
complete here.

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

## Dependency

The only direct project dependency is iiPaintEngine 0.1.0. Its exported CMake
target supplies the raster types, blend modes, transforms, and its own
transitive platform dependencies.

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
~~~

## Documents

- docs/BLUEPRINT.md defines ownership, dependency direction, milestones, and
  completion gates.
- docs/FORMAT.md defines the draft .iisc logical package and manifest contract.
- AGENTS.md fixes the engineering rules that future changes must preserve.

## License

The project is AGPL-3.0-only because iiPaintEngine is an AGPL-3.0-only public
dependency. See LICENSE and NOTICE.md.
