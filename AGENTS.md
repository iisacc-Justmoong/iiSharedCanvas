# iiSharedCanvas engineering contract

## Product boundary

iiSharedCanvas composes static raster pixels, native vector paths, and
keyframed raster or vector assets in one document.

iiPaintEngine remains bitmap-only. Brush input must become committed pixels
before crossing into iiSharedCanvas. Never persist a brush trajectory, raw
pointer sequence, curve, dab stream, or replay command.

## Dependency direction

Application -> iiSharedCanvas -> iiPaintEngine.

iiPaintEngine must not depend on iiSharedCanvas. The only direct dependency in
the initial milestone is iiPaintEngine. Any additional archive, serialization,
vector, text, or codec library requires an explicit maintenance, license, and
dependency-size review.

## Change rules

- Write or update tests before implementing behavior.
- Every source change updates the relevant document and test.
- Use only build/ for generated build output.
- Keep each public header beside its implementation in a module directory.
  Never introduce separate include/ and src/ source trees.
- After source changes, run a fresh configure, full build, all CTest tests,
  install staging, standalone installed-package consumption, and diff checks.
- Do not claim file serialization or rendering until round-trip or golden
  output tests prove it.
- Unknown format versions and invalid references fail closed.
- Prefer integer frames and rational rates over persisted floating timestamps.
- Add abstraction only after repeated concrete changes show the need.

## Stable Phase 0 semantics

- RasterAsset owns iiPaintEngine RasterLayer pixels.
- VectorAsset owns M/L/Q/C/Z paths with solid fill or stroke.
- A layer has exactly one static or keyframed source.
- One keyframed source has one content kind and begins at frame zero.
- Keyframe sampling is hold-only.
- Layer order is bottom-to-top.
- Rendering never mutates source assets.
- BitmapEditor mutates only the explicitly bound RasterAsset and persists no
  pointer trajectory or replay command.
- BitmapItem is a selected-raster display/input adapter, not evidence that the
  mixed-layer frame renderer or serializer is complete.
- Qt Quick is consumed through the Qt targets exported transitively by the sole
  package dependency, iiPaintEngine; product QML continues to use LVRS.
