# iiSharedCanvas blueprint

Status: Phases 0 through 3 complete; measured product hardening remains open

## 1. Product objective

One canvas document must present these content classes together in layer order:

1. Static raster pixels, including pixels produced by an iiPaintEngine brush.
2. Static native vector paths.
3. Raster or vector assets selected by keyframes on a timeline.

The first commercial advantage is a reusable authoring interchange layer for
future desktop, mobile, and web products without forcing each product to invent
its own mixed-media document model.

### 1.1 Specification authority

iiSharedCanvas is the canonical canvas standard for iisacc. Its `Document`,
rendering semantics, editing boundary, validation rules, and `.iisc` encoding
are upstream contracts. Product applications consume those contracts; they do
not co-own or redefine them.

The library evolves from reusable canvas-domain requirements proven inside this
repository. Product-specific ViewModels, QML property names, session objects,
tool conventions, and compatibility aliases belong in consumer-owned adapters.
If a product integration conflicts with iiSharedCanvas, the product is changed
first. The library changes only when the requirement is general, is described
without reference to one product, and passes its own model, render, format, and
package gates.

Development and adoption use separate phases. iiSharedCanvas is specified,
implemented, tested, versioned, and installed independently; only then does a
consumer adoption task begin against that fixed contract. Cross-repository
parallel compatibility development is outside this project policy.

## 2. Boundary and ownership

~~~mermaid
flowchart LR
    APP["Application / LVRS QML UI"] --> ITEM["BitmapItem"]
    APP --> CANVAS["CanvasItem / SharedCanvas"]
    APP --> ISC["iiSharedCanvas"]
    ITEM --> EDIT["BitmapEditor"]
    CANVAS --> EDIT
    EDIT --> DOC
    ISC --> DOC["Document + validation"]
    DOC --> STACK["Ordered layers"]
    DOC --> ASSETS["Asset registry"]
    STACK --> STATIC["Static source"]
    STACK --> ANIM["Keyframed source"]
    ASSETS --> RASTER["RasterAsset"]
    ASSETS --> VECTOR["VectorAsset"]
    ANIM --> EVAL["Frame evaluator"]
    STATIC --> EVAL
    EVAL --> RESOLVED["Resolved asset"]
    RESOLVED --> RASTER
    RESOLVED --> VECTOR
    VECTOR --> VRAST["CPU vector rasterizer"]
    RASTER --> FRAME["FrameRenderer"]
    VRAST --> FRAME
    FRAME --> IIPE["iiPaintEngine compositor"]
    CANVAS --> FRAME
~~~

iiPaintEngine owns bitmap codecs, brush rasterization, ARGB pixel storage
primitives, raster blend semantics, and the affine transform type and coordinate
semantics.

iiSharedCanvas owns mixed-content document identity, ordered layers, vector
geometry, asset references, timeline semantics, cross-content validation, file
format versioning, selected-raster editing coordination, and frame composition.

All persisted model fields are public aggregate data. `DocumentEditor` is the
safe structural mutation boundary over those aggregates: it provides stable-id
lookup and explicit insert, replace, rename, move, and remove operations for
the timeline, assets, layers, keyframes, and vector paths. Every accepted edit
preserves full-document validation; rejected edits preserve the prior value.
Bitmap pixels remain the responsibility of `BitmapEditor`.

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

### Implemented selected-bitmap authoring boundary

`BitmapEditor` resolves a mutable `RasterAsset` by stable id on each operation,
so document asset-vector relocation cannot leave a cached asset pointer. It
accepts complete `RasterLayer` replacement for decoded bitmap input, direct
pixel or rectangular patch edits, clear, and streaming brush/eraser input.
Streaming input uses iiPaintEngine `RasterDabStream` only while a gesture is
active, then commits directly into the asset pixels. No point list or replayable
stroke is added to `Document`.

Undo/redo stores a maximum of 32 full raster snapshots. This is intentionally
the simplest correct first policy and makes a whole brush gesture atomic.
Patch-based history should replace it only after real document sizes establish
the required memory budget.

`BitmapItem` is the Qt Quick display boundary for one selected raster asset. It
converts the engine's ARGB storage into `QImage::Format_ARGB32` at paint time,
uses nearest-neighbor scaling, and exposes mouse painting, explicit
pressure-bearing stroke calls, clear, pixels, undo/redo, zoom, and pan. Its
standalone `createBitmap` path owns a minimal one-layer document; its C++
`bind` path edits a caller-owned document whose lifetime and GUI-thread access
remain the caller's responsibility. This item does not compose document layers
and does not serialize input events.

`CanvasItem`, registered to QML as `SharedCanvas`, is the full-document display
and raster-authoring boundary. It caches `renderFrame` output, switches timeline
frames, applies nearest-neighbor zoom and pan, and can select a raster document
layer for brush/eraser edits while the complete mixed frame remains visible.
Input is expressed in document coordinates and inverted through the selected
layer affine transform before `BitmapEditor` receives it. A caller-owned
document remains authoritative and explicit `refresh()` observes external
mutations. Application selection UX, tools, playback controls, and persistence
remain outside the item.

For an application bootstrap path, `createRasterDocument` installs one selected
transparent raster asset and layer. `replaceSelectedPixels` supports decoded
image import without file-system round trips. The Qt adapter exposes generic
bitmap-authoring controls for brush features, spacing, a three-point pressure
curve, pressure-opacity mapping, stabilizer value, tool mode, tablet/mouse
state, and stroke count. The stabilizer is transient input smoothing and none
of these settings enter the document format. Version 0.1 rerenders
synchronously after edits; the preview interval and multithreaded-event
properties preserve host configuration without asserting background rendering
that is not implemented.

## 6. Implemented render pipeline

`renderFrame` executes these steps:

1. Validate the document.
2. Resolve one asset for each visible layer at the requested frame.
3. Convert vector assets into temporary raster surfaces at the requested output
   resolution.
4. Apply the iiPaintEngine affine transform and layer opacity.
5. Composite bottom-to-top using iiPaintEngine blend semantics.
6. Return one RasterLayer without mutating document assets.

Static and animated content therefore share one render path after frame
evaluation. There is no separate animation canvas.

Version 1 rasterizes M/L/Q/C/Z paths on the CPU with deterministic 4x4 coverage
sampling, even-odd fills, and round stroke footprints. It applies the complete
iiPaintEngine affine matrix with nearest-neighbor asset sampling and clips the
result to the canvas. Source-over, multiply, screen, and overlay are delegated
to the iiPaintEngine compositor. Destination-out remains a brush eraser mode
and validation rejects it as a document layer blend mode.

## 7. Implemented serialization

The physical package is the canonical binary `.iisc` container defined in
FORMAT.md. `encodeIisc` and `decodeIisc` use only the C++ standard library and
therefore preserve the fixed direct dependency set. The 32-byte header records
version, payload size, and CRC-32. The payload stores native raster/vector
assets, ordered layers, transforms, and timeline references without archive
paths or JSON parsing.

The writer chooses raw or run-length ARGB32 deterministically and emits one byte
representation for a document. The reader verifies checksum and exact payload
length before parsing, enforces configured aggregate limits before allocation,
validates canonical UTF-8 and record tags, and validates the completed document
before exposure.

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

Serialization enforces container size, decoded pixel, collection, vector,
keyframe, and string limits before allocation. The binary transport has no
archive entries or paths, so archive recursion and path traversal do not exist
in version 1.

## 9. Dependency review

iiPaintEngine 0.1.0 is selected because it is the user's existing bitmap-only
engine and already provides RasterLayer, brush rasterization, blend modes, and
transforms. It is AGPL-3.0-only and carries Qt/LVRS transitively. iiSharedCanvas
therefore starts under AGPL-3.0-only.

No second direct dependency is present. The `.iisc` codec uses the standard
library; SVG, text shaping, GPU vector rendering, and media codecs remain
explicit future decisions rather than hidden or vendored code.

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

Completed in the current implementation.

Complete when:

- .iisc writer and reader round-trip all Phase 0 types.
- Canonical serialization and version compatibility tests pass; version 1.0
  has no earlier physical format requiring migration.
- Corrupt, oversized, path-traversal, and future-version files fail closed.
- Bitmap brush output round-trips as pixels without retained trajectories.

### Phase 2 - frame renderer

Completed in the current implementation.

Complete when:

- Static raster and vector layers render together.
- Animated raster and vector layers render deterministically at boundary
  frames.
- Transform, opacity, clipping, and supported blend modes have golden tests.
- CPU output is stable across clean builds.

### Phase 3 - authoring adapter

Complete when:

- [x] iiPaintEngine bitmap editing can commit into a selected RasterAsset.
- [x] Undo/redo stores pixel snapshots rather than stroke replay.
- [x] A reusable Qt Quick bitmap item can display and manipulate that asset.
- [x] A reusable Qt Quick mixed-document item renders frames and edits a
  selected transformed raster layer.
- [x] Public data lookup and `DocumentEditor` APIs expose validated structural
  edits without requiring direct vector manipulation or partial invalid states.
- [x] Product-neutral C++ and QML contract tests verify the authoring surface,
  mixed raster/vector/timeline output, and native `.iisc` round trip without a
  consumer application defining the API.

### Phase 4 - product hardening

Complete when:

- Cross-platform packages are installed and consumed on every target.
- Large-document memory budgets, partial decode, thumbnails, autosave, and
  crash recovery are measured.
- Public API compatibility and file migration policy are published.
