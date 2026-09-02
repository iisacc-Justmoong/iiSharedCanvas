# iiSharedCanvas blueprint

Status: Phases 0 through 3 complete; bounded large-canvas interaction complete;
cross-platform product hardening remains open

## 1. Product objective

One canvas document must present these content classes together in layer order:

1. Static raster pixels, including pixels produced by an iiPaintEngine brush.
2. Static native vector paths.
3. Raster or vector assets selected by keyframes on a timeline.

An adjacent import model may carry decoded Camera RAW sensor samples and common
capture/calibration metadata until a caller explicitly processes them into
committed raster pixels. RAW sensor data is not a third document layer type.

An optional document recipe may retain Stable Diffusion prompts, sampler
settings, model identity, exact ComfyUI graph metadata, and lossless
AUTOMATIC1111 infotext. It describes how content was generated or refined but
is not rendered content and does not add an inference runtime.

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
    APP --> CRAW["CameraRawData import model"]
    APP --> A1111["Automatic1111Infotext parser"]
    APP --> SDMETA["StableDiffusionMetadata recipe"]
    CRAW -. "explicit RAW processing" .-> RASTER
    A1111 --> SDMETA
    ITEM --> EDIT["BitmapEditor"]
    CANVAS --> EDIT
    EDIT --> DOC
    ISC --> DOC["Document + validation"]
    SDMETA --> DOC
    DOC --> STACK["Ordered layers"]
    DOC --> ASSETS["Asset registry"]
    STACK --> STATIC["Static source"]
    STACK --> ANIM["Keyframed source"]
    ASSETS --> RASTER["RasterAsset"]
    ASSETS --> CHUNKS["ChunkedRasterAsset"]
    ASSETS --> VECTOR["VectorAsset"]
    ANIM --> EVAL["Frame evaluator"]
    STATIC --> EVAL
    EVAL --> RESOLVED["Resolved asset"]
    RESOLVED --> RASTER
    RESOLVED --> CHUNKS
    RESOLVED --> VECTOR
    VECTOR --> VRAST["Bounded CPU vector tile rasterizer"]
    RASTER --> FRAME["FrameRenderer"]
    VRAST --> FRAME
    FRAME --> IIPE["iiPaintEngine compositor"]
    CANVAS --> ASYNC["AsyncFrameRenderer"]
    ASYNC --> FRAME
    FRAME --> TILES["LOD texture tiles"]
    TILES --> GPU["Qt Quick scene graph / GPU transform"]
~~~

iiPaintEngine owns bitmap codecs, brush rasterization, ARGB pixel storage
primitives, raster blend semantics, and the affine transform type and coordinate
semantics.

iiSharedCanvas owns mixed-content document identity, ordered layers, vector
geometry, sparse infinite-canvas chunk coordinates, asset references, timeline semantics, cross-content validation, file
format versioning, Stable Diffusion recipe metadata, selected-raster editing
coordination, and frame composition.

All persisted model fields are public aggregate data. `DocumentEditor` is the
safe structural mutation boundary over those aggregates: it provides stable-id
lookup and explicit insert, replace, rename, move, and remove operations for
the timeline, assets, layers, keyframes, and vector paths. Every accepted edit
preserves full-document validation; rejected edits preserve the prior value.
Finite bitmap pixels remain the responsibility of `BitmapEditor`; sparse
infinite bitmap pixels remain the responsibility of `ChunkedBitmapEditor`.

The application owns UI, tools, playback controls, selection experience,
autosave policy, networking, collaboration, and the selected Camera RAW decoder
and processing pipeline. Camera RAW file decoding, demosaicing, and tone
rendering remain outside the generic data objects. AUTOMATIC1111 image-carrier extraction,
Stable Diffusion inference, model resolution/download, graph execution, and
trust policy likewise remain application or adapter responsibilities. The library name does not
imply that real-time collaboration is part of this milestone.

Dependency direction is one-way:

~~~text
Application
  -> iiSharedCanvas
       -> iiPaintEngine
~~~

iiPaintEngine must never reference iiSharedCanvas.

## 3. Core data model

`CameraRawData` is independent from `Document`. It owns one unsigned integer
sensor payload plus color, camera, lens, and capture metadata. Its sensor image
distinguishes CFA, monochrome, and interleaved linear RAW; retains bit depth,
orientation, active area, default crop, indexed channels, CFA and black-level
repeat patterns, and white levels; and stores samples in row-pixel-plane order.
The color profile retains optional as-shot neutral coordinates and one or more
finite XYZ-to-camera matrices. This boundary can receive data from DNG, LibRaw,
or a future platform decoder without making any one decoder the core model.

The Camera RAW objects do not decode files, own source-file bytes, apply
linearization or black subtraction, demosaic, color-convert, tone-map, create a
`RasterAsset`, enter the layer stack, or change `.iisc` persistence. Those are
explicit pipeline stages whose policy varies by decoder and product.

`StableDiffusionMetadata` is optional state owned by `Document`. Typed prompts,
output settings, sampler passes, model resources, LoRAs, software identity, and
extension entries support direct inspection. Raw ComfyUI `prompt` and
`workflow` JSON remain separate because the first is the API execution graph
and the second restores the UI graph. The strings are syntax-checked and
preserved exactly; iiSharedCanvas does not interpret custom nodes, execute the
graph, fetch model resources, or attach the recipe to one particular layer.

`Automatic1111Infotext` is a lossless compatibility view over extracted
AUTOMATIC1111 parameters text. It preserves the complete text and ordered
key/value sequence while projecting portable prompts, dimensions, sampling
passes, model resources, and version data into `StableDiffusionMetadata`.
Unknown extension fields stay available and malformed typed data fails closed.
The image container and model runtime remain outside this object.

Document contains a format version, finite/infinite mode, positive currently
allocated canvas extent, optional world origin and chunk size, rational frame
rate, frame count, asset registry, and ordered layer stack. A finite canvas is
bounded at origin zero. An infinite canvas grows its allocated region outwards
on chunk boundaries while its conceptual world remains unbounded within the
supported signed coordinate domain.

Asset is one of:

- RasterAsset: stable id plus iiPaintEngine RasterLayer ARGB pixels.
- ChunkedRasterAsset: stable id plus canonical row-major sparse RasterChunk
  entries addressed by signed world column and row.
- VectorAsset: stable id, positive viewport, and ordered paths.

Vector v1 intentionally supports only the geometry required by the stated
goal: move, line, quadratic curve, cubic curve, close, solid fill, and solid
stroke. Text, gradients, masks, boolean operations, and effects are not
predicted into the first model.

Layer is a public variant of two complete, structurally parallel types:

- BitmapLayer: shared LayerProperties plus one source that may reference only a
  RasterAsset or ChunkedRasterAsset.
- VectorLayer: shared LayerProperties plus one source that may reference only a
  VectorAsset.

Layer source is one of:

- StaticSource: one asset id.
- KeyframedSource: a strictly increasing list of frame-to-asset references. Its
  content kind is fixed by the owning BitmapLayer or VectorLayer.

Neither a static nor a keyframed layer source can cross its owning layer type.
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

`ChunkedBitmapEditor` applies the same committed-pixel brush contract in world
coordinates. It allocates only chunks receiving raster samples; missing chunks
are transparent. Sparse undo/redo snapshots the chunk collection, and region
growth never rewrites a chunk's coordinates or pixel payload.

`BitmapItem` is the Qt Quick display boundary for one selected raster asset. It
converts the engine's ARGB storage into `QImage::Format_ARGB32` at paint time,
uses nearest-neighbor scaling, and exposes mouse painting, explicit
pressure-bearing stroke calls, clear, pixels, undo/redo, zoom, and pan. Its
standalone `createBitmap` path owns a minimal one-layer document; its C++
`bind` path edits a caller-owned document whose lifetime and GUI-thread access
remain the caller's responsibility. This item does not compose document layers
and does not serialize input events.

`CanvasItem`, registered to QML as `SharedCanvas`, is the full-document display
and raster-authoring boundary. It caches bounded LOD texture tiles, switches timeline
frames, applies nearest-neighbor GPU zoom and pan, and can select a raster document
layer for brush/eraser edits while the complete mixed frame remains visible.
Input is expressed in document coordinates and inverted through the selected
layer affine transform before `BitmapEditor` receives it. A caller-owned
document remains authoritative and explicit `refresh()` observes external
mutations. Its GUI thread snapshots validated document state, a coalescing
worker renders only missing visible/prefetch tiles, and the Qt Quick scene graph
uploads and transforms those tiles. Application selection UX, tools, playback
controls, and persistence remain outside the item.

For an application bootstrap path, `createRasterDocument` installs one selected
transparent raster asset and layer. `replaceSelectedPixels` supports decoded
image import without file-system round trips. The Qt adapter exposes generic
bitmap-authoring controls for brush features, spacing, a three-point pressure
curve, pressure-opacity mapping, stabilizer value, tool mode, tablet/mouse
state, and stroke count. The stabilizer is transient input smoothing and none
of these settings enter the document format. Version 0.1 commits model edits
synchronously but rerenders asynchronously after edits.
`livePreviewFrameIntervalMs` bounds active-stroke snapshot scheduling, while
the multithreaded-event property remains host input configuration. Superseded
worker requests are coalesced by `AsyncFrameRenderer` independently.

`createInfiniteRasterDocument` installs one selected empty sparse raster layer.
The host supplies camera demand through `ensureInfiniteCanvasRegion`. The
document unions that demand with its allocated world region and rounds each new
edge outwards to a chunk boundary. `CanvasItem` exposes the world origin and
returns exact four-side growth, leaving visual resizing and camera preservation
to the consumer UI.

## 6. Implemented render pipeline

`renderFrame`, `renderFrameRegion`, and `renderFrameTiles` execute these steps
through the public layer-rendering boundary:

1. Validate the document.
2. Resolve one finite raster, sparse chunked raster, or vector asset for each
   visible layer at the requested frame.
3. Rasterize each layer into isolated bounded tiles, applying its affine
   transform while preserving full-strength pixels and carrying opacity and
   blend mode as metadata.
4. Composite those layer tiles bottom-to-top using iiPaintEngine opacity and
   blend semantics.
5. Return either the full frame or bounded world-region tiles without mutating
   document assets.

Static and animated content therefore share one render path after frame
evaluation. There is no separate animation canvas.

`renderFrameLayerTiles` is the independently addressable layer operation,
`renderFrameLayers` returns a stable ordered batch, and `composeFrameLayers`
is the explicit composition boundary. Layers render concurrently from one immutable document snapshot. `AsyncFrameRenderer` uses no more layer workers
than the global Qt thread pool allows, joins their results in document order,
then performs the final composition without exposing a partial frame.

Version 1 rasterizes M/L/Q/C/Z paths on a worker CPU with deterministic 4x4 coverage
sampling, even-odd fills, and round stroke footprints. It applies the complete
iiPaintEngine affine matrix with nearest-neighbor asset sampling and clips the
result to the canvas. Source-over, multiply, screen, and overlay are delegated
to the iiPaintEngine compositor. Destination-out remains a brush eraser mode
and validation rejects it as a document layer blend mode.

The Qt adapter selects a power-of-two LOD from viewport zoom, keeps no more
than 64 resident composed 512-texel tiles and 256 isolated layer tiles, and
coalesces queued renders to the newest immutable snapshot/request. Pan and zoom
update a `QSGTransformNode` immediately. Source-over layers are uploaded under
independent scene-graph opacity nodes; multiply, screen, overlay, or an
incomplete layer cache uses the deterministic iiPaintEngine composite tile.
Hardware-backed Qt Quick then owns tile texture sampling and scene composition;
software backends preserve correctness with the same nodes.

## 7. Implemented serialization

The physical package is the canonical binary `.iisc` container defined in
FORMAT.md. `encodeIisc` and `decodeIisc` use only the C++ standard library and
therefore preserve the fixed direct dependency set. The 32-byte header records
version, payload size, and CRC-32. The payload stores native raster/vector
assets, sparse chunks, allocated world geometry, ordered layers, transforms,
timeline references, and optional format-1.2 generation metadata without
archive paths. ComfyUI JSON is retained as exact UTF-8 and receives bounded
syntax validation rather than graph parsing or normalization.

The writer chooses raw or run-length ARGB32 deterministically and emits one byte
representation for a document. The reader verifies checksum and exact payload
length before parsing, enforces configured aggregate limits before allocation,
validates canonical UTF-8 and record tags, and validates the completed document
before exposure.

## 8. Validation and security invariants

Camera RAW validation is separate from document validation:

- Sensor extent, significant bit depth, sample-plane count, and exact sample
  storage must agree without integer overflow.
- Active area is contained by the sensor and default crop by the active area.
- CFA cells reference existing channels; linear planes correspond to channels.
- Black/white levels, as-shot neutral coordinates, color matrices, lens ranges,
  and capture values must be finite, correctly sized, and physically ordered.
- Bounds-checked access returns no value for invalid coordinates or malformed
  patterns and never performs implicit image processing.

- Unknown newer format versions fail closed.
- Canvas, raster, and vector extents are positive.
- Infinite canvases require format 1.1+, a power-of-two chunk size from 32
  through 4096, and signed coordinates whose allocated region remains bounded.
- Sparse chunks are unique, row-major, and exactly the configured chunk size;
  finite canvases reject sparse raster assets.
- Raster dimensions equal the exact ARGB pixel count.
- Asset ids and layer ids are non-empty and unique.
- Every layer reference resolves.
- Layer opacity is finite and within 0 through 1.
- Transform and vector coordinates are finite.
- A vector path begins with MoveTo and has visible fill or stroke.
- Timeline rate and frame count are non-zero.
- Keyframes begin at zero, are strictly increasing, remain in range, and keep
  the content kind fixed by their owning BitmapLayer or VectorLayer.
- Stable Diffusion metadata requires format 1.2, contains at least one payload,
  keeps dimensions and discrete counts positive, and keeps sampler/LoRA numeric
  values finite and inside their defined ranges.
- ComfyUI prompt and workflow payloads are JSON objects; extension values are
  valid JSON under unique non-reserved keys. All such metadata remains
  untrusted and is never executed by validation or rendering.

Serialization enforces container size, decoded pixel, collection, vector,
keyframe, metadata-entry, and string limits before allocation. The binary transport has no
archive entries or paths, so archive recursion and path traversal do not exist
in version 1.

## 9. Dependency review

iiPaintEngine 0.1.0 is selected because it is the user's existing bitmap-only
engine and already provides RasterLayer, brush rasterization, blend modes, and
transforms. It is AGPL-3.0-only and carries Qt/LVRS transitively. iiSharedCanvas
therefore starts under AGPL-3.0-only.

No second direct dependency is present. Qt Core thread-pool/future primitives
and the public Qt Quick scene graph arrive through iiPaintEngine's existing Qt
targets. The `.iisc` codec uses the standard library; SVG, text shaping, GPU
vector path rasterization, and media codecs remain
explicit future decisions rather than hidden or vendored code.

[ComfyUI's workflow metadata documentation](https://docs.comfy.org/development/api-development/workflow-metadata),
[Workflow API format](https://docs.comfy.org/development/api-development/workflow-api-format),
and [metadata writer](https://github.com/Comfy-Org/ComfyUI/blob/master/comfy_api/latest/_ui.py)
were reviewed for the `prompt`, `workflow`, KSampler, and `extra_pnginfo`
contracts. [nlohmann/json 3.12.0](https://github.com/nlohmann/json) was also
reviewed as an active, MIT-licensed single-header implementation. It was not
introduced because this module preserves open-ended JSON verbatim and needs
only bounded syntax validation; adding a package and installed dependency for
that narrow role would increase maintenance and consumer surface. The bounded
standard-library validator never normalizes the authoritative strings.

AUTOMATIC1111's official
[infotext parser](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/infotext_utils.py),
[infotext writer](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/processing.py),
[image metadata reader/writer](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/images.py),
and [checkpoint hash model](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/sd_models.py)
were reviewed for prompts, open-ended fields, carrier keys, Hires defaults, and
current/legacy hash meaning. A bounded standard-library parser is sufficient
after carrier extraction, so no Python, Pillow, EXIF, image codec, or WebUI
runtime is introduced.

[Adobe DNG 1.7.1](https://helpx.adobe.com/camera-raw/desktop/dng-and-file-formats/digital-negative.html)
was reviewed as the current public reference for CFA and LinearRaw
interpretation, orientation, active/crop geometry, repeating black levels,
per-plane white levels, as-shot neutral coordinates, and camera color matrices.
The aggregates reuse those common concepts without copying DNG tag numbers,
implementing TIFF/DNG transport, or claiming DNG conformance.

[LibRaw](https://github.com/LibRaw/LibRaw) was reviewed for proprietary-camera
decoding. It is actively maintained, supports a broad camera set, and is
dual-licensed LGPL-2.1 or CDDL-1.0. It also adds a native decoder/codec surface
whose maintenance and distribution cost is unnecessary for in-memory objects.
It is therefore not added now; a future decoder adapter can depend on it
without forcing iiSharedCanvas core or every consumer to do so.

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
- Format-neutral Camera RAW aggregates and independent validation build.
- Typed Stable Diffusion metadata and exact ComfyUI graph preservation build.
- Lossless AUTOMATIC1111 infotext parsing and typed common projection build.
- Validation and hold evaluation are tested.
- Installable CMake package and standalone consumer are verified.
- Blueprint and format documents match the code.

### Phase 1 - durable format

Completed in the current implementation.

Complete when:

- .iisc writer and reader round-trip all Phase 0 types.
- Format 1.2 round-trips generation metadata while 1.0 and 1.1 remain
  byte-canonical and metadata-free.
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
- [x] Infinite documents grow an allocated region from camera demand and store,
  render, edit, undo, and serialize sparse signed-coordinate raster chunks.

### Phase 4 - product hardening

Complete when:

- Cross-platform packages are installed and consumed on every target.
- [x] Interactive rendering uses bounded resident tiles, LOD, immutable worker
  snapshots, and GPU scene transforms for tens-of-thousands-pixel canvases.
- Partial decode, thumbnails, autosave, and crash recovery are measured.
- Public API compatibility and file migration policy are published.
