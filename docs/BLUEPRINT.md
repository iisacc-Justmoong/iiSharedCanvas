# iiSharedCanvas blueprint

Status: Phase 0 complete blueprint

## 1. Product objective

One canvas document must present these content classes together in layer order:

1. Static raster pixels, including pixels produced by an iiPaintEngine brush.
2. Static native vector paths.
3. Raster or vector assets selected by keyframes on a timeline.

The first commercial advantage is a reusable authoring interchange layer for
future desktop, mobile, and web products without forcing each product to invent
its own mixed-media document model.

## 2. Boundary and ownership

~~~mermaid
flowchart LR
    APP["Application / QML UI"] --> ISC["iiSharedCanvas"]
    ISC --> DOC["Document + validation"]
    DOC --> STACK["Ordered layers"]
    DOC --> ASSETS["Asset registry"]
    STACK --> STATIC["Static source"]
    STACK --> ANIM["Keyframed source"]
    ASSETS --> RASTER["RasterAsset"]
    ASSETS --> VECTOR["VectorAsset"]
    RASTER --> IIPE["iiPaintEngine RasterLayer"]
    ANIM --> EVAL["Frame evaluator"]
    STATIC --> EVAL
    VECTOR --> FUTURE["Vector rasterizer - Phase 2"]
    EVAL --> FUTURE
~~~

iiPaintEngine owns bitmap editing, bitmap codecs, brush rasterization, ARGB
pixel storage primitives, raster blend semantics, and raster transforms.

iiSharedCanvas owns mixed-content document identity, ordered layers, vector
geometry, asset references, timeline semantics, cross-content validation, file
format versioning, and eventually frame composition.

The application owns UI, tools, playback controls, selection experience,
autosave policy, networking, and collaboration. The library name does not imply
that real-time collaboration is part of this milestone.

Dependency direction is one-way:

~~~text
Application
  -> iiSharedCanvas
       -> iiPaintEngine
~~~

iiPaintEngine must never reference iiSharedCanvas.

## 3. Core data model

Document contains a format version, positive canvas extent, rational frame rate,
frame count, asset registry, and ordered layer stack.

Asset is one of:

- RasterAsset: stable id plus iiPaintEngine RasterLayer ARGB pixels.
- VectorAsset: stable id, positive viewport, and ordered paths.

Vector v1 intentionally supports only the geometry required by the stated
goal: move, line, quadratic curve, cubic curve, close, solid fill, and solid
stroke. Text, gradients, masks, boolean operations, and effects are not
predicted into the first model.

Layer source is one of:

- StaticSource: one asset id.
- KeyframedSource: one declared content kind and a strictly increasing list of
  frame-to-asset references.

One keyframed source cannot mix raster and vector assets. The first keyframe is
at frame zero. Every keyframe is inside the document frame count. This makes
evaluation deterministic and eliminates undefined pre-roll behavior.

## 4. Time semantics

Time is stored as integer frame positions plus a rational frame rate. Floating
timestamps are not persisted.

Phase 0 evaluation uses hold sampling for both raster and vector keyframes. The
active asset is the last keyframe at or before the requested frame. Sampling
outside the timeline fails closed.

Interpolation is excluded until a concrete product needs it. Raster
interpolation usually means cross-fade or optical methods; vector interpolation
requires matching path topology. Treating these as one generic interpolation
flag would create an invalid abstraction.

## 5. Brush semantics

A brush is an authoring operation, not durable scene geometry:

~~~text
pointer input
  -> iiPaintEngine brush rasterization
  -> committed RasterLayer pixels
  -> iiSharedCanvas RasterAsset
~~~

No pointer trajectory, curve, dab stream, replay command, or retained brush
stroke may be serialized by iiSharedCanvas. A brush preset may later be stored
as optional authoring metadata, but rendered truth remains the pixels.

## 6. Render pipeline target

The future frame renderer will execute these steps:

1. Validate the document.
2. Resolve one asset for each visible layer at the requested frame.
3. Convert vector assets into temporary raster surfaces at the requested output
   resolution.
4. Apply the iiPaintEngine affine transform and layer opacity.
5. Composite bottom-to-top using iiPaintEngine blend semantics.
6. Return one RasterLayer without mutating document assets.

Static and animated content therefore share one render path after frame
evaluation. There is no separate animation canvas.

## 7. Serialization target

The logical package is described in FORMAT.md. The package transport is planned
as a .iisc ZIP container with a manifest and separate assets. No archive or JSON
library is introduced in Phase 0 because the user fixed the initial direct
dependency set to iiPaintEngine only.

Before Phase 1 implementation, choose one of these evidence-based paths:

- Approve a small maintained archive and JSON dependency after license and
  maintenance review.
- Define a compact binary container implemented with the C++ standard library.
- Explicitly allow direct Qt Core use and make that dependency visible in
  CMake rather than relying on transitive linkage.

The current code does not pretend that an unimplemented serializer is complete.

## 8. Validation and security invariants

- Unknown newer format versions fail closed.
- Canvas, raster, and vector extents are positive.
- Raster dimensions equal the exact ARGB pixel count.
- Asset ids and layer ids are non-empty and unique.
- Every layer reference resolves.
- Layer opacity is finite and within 0 through 1.
- Transform and vector coordinates are finite.
- A vector path begins with MoveTo and has visible fill or stroke.
- Timeline rate and frame count are non-zero.
- Keyframes begin at zero, are strictly increasing, remain in range, and keep
  one content kind.

The serializer milestone must additionally enforce archive size, entry count,
decoded pixel count, recursion, path traversal, and checksum limits before
allocating large buffers.

## 9. Dependency review

iiPaintEngine 0.1.0 is selected because it is the user's existing bitmap-only
engine and already provides RasterLayer, brush rasterization, blend modes, and
transforms. It is AGPL-3.0-only and carries Qt/LVRS transitively. iiSharedCanvas
therefore starts under AGPL-3.0-only.

No second direct dependency is present. ZIP, JSON, SVG, text shaping, GPU vector
rendering, and media codecs remain explicit future decisions rather than hidden
or vendored code.

The CMake target records the imported iiPaintEngine library directory in its
build and install rpath and also supplies the standard sibling-prefix fallback.
The public target propagates that runtime search directory to final executables
because iiPaintEngine is also a public ABI dependency.
This is required because the current iiPaintEngine package uses an @rpath
install name that CMake does not automatically add to Release consumers.

## 10. Milestones and gates

### Phase 0 - blueprint and setup

Complete when:

- Git and CMake project exist.
- iiPaintEngine is the only direct dependency.
- Mixed static/animated raster and vector document model builds.
- Validation and hold evaluation are tested.
- Installable CMake package and standalone consumer are verified.
- Blueprint and format documents match the code.

### Phase 1 - durable format

Complete when:

- .iisc writer and reader round-trip all Phase 0 types.
- Canonical serialization and version migration tests pass.
- Corrupt, oversized, path-traversal, and future-version files fail closed.
- Bitmap brush output round-trips as pixels without retained trajectories.

### Phase 2 - frame renderer

Complete when:

- Static raster and vector layers render together.
- Animated raster and vector layers render deterministically at boundary
  frames.
- Transform, opacity, clipping, and supported blend modes have golden tests.
- CPU output is stable across clean builds.

### Phase 3 - authoring adapter

Complete when:

- iiPaintEngine bitmap editing can commit into a selected RasterAsset.
- Undo/redo stores pixel patches or snapshots rather than stroke replay.
- QML integration uses LVRS and remains outside the format core.

### Phase 4 - product hardening

Complete when:

- Cross-platform packages are installed and consumed on every target.
- Large-document memory budgets, partial decode, thumbnails, autosave, and
  crash recovery are measured.
- Public API compatibility and file migration policy are published.
