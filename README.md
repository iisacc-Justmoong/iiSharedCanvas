# iiSharedCanvas

iiSharedCanvas is a C++20 document and authoring foundation for composing
raster artwork, native vector paths, and frame-based raster or vector animation
on one canvas. It also provides format-neutral decoded Camera RAW authoring
data for import pipelines. It is the authoritative canvas standard for iisacc
products; applications adopt its model and format through application-owned
adapters.

The repository contains the versioned in-memory model, validation rules,
deterministic keyframe evaluation, bounded mixed raster/vector tile rendering,
an editable raster-asset boundary, sparse infinite-canvas chunks, asynchronous
Qt Quick scene-graph presentation, CMake package export, canonical `.iisc`
serialization, and contract tests. Cross-platform device profiling, partial
decode, and crash-recovery hardening remain later milestones.

Consumer applications do not shape this public contract in parallel. A library
change is completed and verified here first; consumers then conform to that
fixed version through their own bridges. Existing product ViewModels, QML names,
session-layer models, and tool conventions are not compatibility requirements
for iiSharedCanvas.

## Content contract

| Authoring input | Stored render content | Initial behavior |
| --- | --- | --- |
| Bitmap image | iiPaintEngine RasterLayer | Static layer |
| iiPaintEngine brush | Committed ARGB pixels | Static or keyframed raster asset |
| Infinite raster paint | Committed ARGB pixels in touched world chunks | Static sparse raster layer |
| Decoded Camera RAW | Unsigned sensor samples plus calibration and capture metadata | Import-side aggregate, not a document layer |
| Stable Diffusion recipe | Typed generation settings plus exact workflow metadata | Optional document metadata, never render content |
| Vector path | M/L/Q/C/Z path commands with solid fill or stroke | Static layer |
| Raster keyframes | Raster asset references at integer frames | Hold sampling |
| Vector keyframes | Vector asset references at integer frames | Hold sampling |

Brush trajectories, dab sequences, and replay commands are never part of the
iiSharedCanvas format. iiPaintEngine rasterizes brush input immediately, and
iiSharedCanvas receives only the committed pixels. This keeps raster editing
compatible with iiPaintEngine's bitmap-only contract.

## Camera RAW authoring data

`CameraRawData` is a format-neutral decoded Camera RAW aggregate. It separates
the sensor image, color profile, camera identity, lens identity, and capture
settings so an importer does not need to force manufacturer tags into a canvas
layer. `CameraRawSensorImage` supports one-to-32-bit unsigned CFA, monochrome,
or interleaved linear RAW samples in canonical row-pixel-plane order.

The sensor image can retain all eight TIFF-style orientations, an absolute
active area and default crop, indexed color channels, a repeating CFA pattern,
a row-column-plane black-level pattern, and scalar or per-plane white levels.
`CameraRawColorProfile` holds optional as-shot neutral coordinates and any
number of finite XYZ-to-camera calibration matrices. Camera, lens, exposure,
aperture, ISO, focal length, focus distance, and exposure compensation remain
independent metadata objects.

`cameraRawSampleAt`, `cameraRawChannelIndexAt`, `cameraRawBlackLevelAt`, and
`cameraRawWhiteLevelAt` provide bounds-checked access. CFA and black-level
patterns start at the active-area origin. `validateCameraRaw` checks dimensions,
sample counts and ranges, regions, pattern references, calibration cardinality,
and numeric metadata. File bytes, manufacturer-specific decoding, demosaicing,
noise reduction, tone mapping, RGB conversion, and implicit `RasterAsset`
creation are deliberately not performed by these data objects.

## Stable Diffusion generation metadata

`StableDiffusionMetadata` preserves typed generation parameters without making
an inference engine part of the canvas. It stores positive and negative
prompts, output extent, batch size, CLIP skip, and any number of sampling
passes. Each pass can retain its graph node id, seed, steps, CFG scale, sampler,
scheduler, denoise strength, and start/end step bounds. Model and VAE resources
retain role, name, hash, hash type, and URI; LoRA entries retain independent
model and CLIP strengths. Software identity, creation time, an unmodified
AUTOMATIC1111 parameters string, and namespaced extension values cover common
round-trip cases without flattening a multi-stage recipe into one sampler.

`parseAutomatic1111Infotext` reads the extracted infotext used by
AUTOMATIC1111 without making an image codec part of the canvas model. It keeps
the complete source text, multiline positive and negative prompts, and every
ordered key/value pair, including duplicate and future extension keys. JSON-
quoted values decode commas, colons, escapes, and Unicode for typed access;
`findAutomatic1111Parameter` follows AUTOMATIC1111's last-value-wins behavior.
The common projection maps output size, batch and CLIP settings, base and Hires
sampler passes, checkpoint/VAE/Hires/refiner resources, software version, and
10-character SHA-256 prefixes. Unmapped settings remain unique generic extras,
while the raw infotext remains authoritative and byte-exact.

The compatibility boundary follows AUTOMATIC1111's official
[`parse_generation_parameters`](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/infotext_utils.py),
including the three-field terminal-line test, default CLIP skip of 1, default
`Automatic` schedule, inherited Hires sampler/scheduler, and Hires step value
zero meaning the main step count. Its official
[image metadata implementation](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/images.py)
places this text in PNG `parameters` or an image-format metadata carrier such
as EXIF `UserComment`. Carrier extraction stays in the importing adapter;
iiSharedCanvas accepts the extracted UTF-8 string and does not add PNG, JPEG,
WebP, AVIF, GIF, Pillow, or EXIF dependencies.

ComfyUI `prompt` and `workflow` JSON are stored separately and byte-for-byte as
UTF-8 strings. This follows ComfyUI's distinction between the API execution
graph (`prompt`) and the UI-restoration graph (`workflow`); either may be
present independently. `extraPngInfo` preserves extension JSON under
non-reserved keys. Validation requires each graph to be a JSON object, each
extension value to be valid JSON, and keys to be unique, but it never executes
nodes, downloads models, or treats embedded metadata as trusted input.

The metadata belongs to the complete `Document`, not to a bitmap or vector
layer, because one workflow can produce or refine several canvas assets.
`validateStableDiffusionMetadata` validates it independently, while
`validate(Document)` enforces the format boundary. `DocumentEditor` can set or
clear the recipe atomically and upgrades a legacy document to format 1.2 when
metadata is first attached. Rendering ignores the recipe and therefore remains
pixel-identical with or without it.

## Data access and structural editing

Every persisted field remains directly available through the public aggregate
types in `Document/Document.h`. Stable-id lookup helpers expose typed raster and
vector assets, layers, exact keyframes, collection indices, and every static or
keyframed reference to an asset.

The ordered layer stack is a `BitmapLayer | VectorLayer` variant. Both concrete
types have the same `LayerProperties + LayerSource` structure, while validation
and `DocumentEditor` reject every cross-type asset reference. Bitmap layers may
hold only finite or chunked raster assets; vector layers may hold only native
vector assets. A keyframed source inherits its content kind from its owning
layer instead of persisting a second mutable kind in memory.

`DocumentEditor` is the validated structural mutation API. It edits canvas
extent, frame rate/count, assets, layer properties/order/sources, keyframes, and
vector path collections. Successful operations keep the complete document
valid and increment a monotonic editor revision once. Rejected operations
return a typed code, path, and message without partially changing the document.
Asset renaming rewrites all references atomically; referenced asset removal is
rejected instead of cascading silently.

`CanvasItem::document()` exposes the bound raw document for C++ inspection.
`CanvasItem::documentEditor()` exposes the bound structural editor, while
`CanvasItem::editDocument()` is the preferred callback boundary because it
rerenders the current frame, repairs selection, and clamps the current frame
after a valid timeline shrink. Direct raw edits remain possible, but their
owner must call `refresh()` and validate the result.

See [docs/API.md](docs/API.md) for the complete data field, lookup, mutation,
failure, lifetime, and threading contracts.

## Bitmap display and editing

`BitmapEditor` binds to a raster asset by id and mutates that `RasterAsset`
directly. It supports individual pixels, rectangular ARGB patches, whole
`RasterLayer` replacement, clear, streamed brush/eraser input, dirty bounds,
and undo/redo. Brush input is rasterized immediately through iiPaintEngine;
only the resulting pixels live in the document. One brush gesture is one undo
operation. The initial history policy retains at most 32 full raster snapshots
for predictable behavior; patch-budget optimization remains a measured product
hardening task.

`ChunkedBitmapEditor` stores only touched chunks in an infinite canvas. Signed
world coordinates select canonical row/column chunks, missing chunks remain
transparent, and each brush gesture commits pixels with one sparse undo entry.
Camera movement changes the allocated world region on chunk boundaries without
moving existing chunks or converting the document into one monolithic bitmap.

`BitmapItem` is a `QQuickPaintedItem` display and input adapter. It can bind an
existing document raster asset or create an owned bitmap, renders ARGB pixels
without smoothing, supports zoom and pan, and routes mouse or explicit
pressure-bearing stroke calls into `BitmapEditor`. This displays one selected
raster asset; it is not the full-document canvas item. If another
owner mutates a bound document directly, the UI owner must call `refresh()` on
the GUI thread.

`CanvasItem` is the full-document Qt Quick boundary and is registered as
`SharedCanvas`. It is a `QQuickItem` whose worker renders only visible,
prefetched 512-texel tiles from an immutable document snapshot. Each visible document layer is an independent asynchronous render unit. The item retains
the isolated layer tiles as well as their deterministic bottom-to-top composite.
Qt Quick scene graph textures perform presentation: they present source-over
layers independently and use the composed tile fallback for blend modes that
require iiPaintEngine. They
also perform nearest-neighbor scaling, pan, and zoom on the active graphics
backend instead of repainting a canvas-sized `QQuickPaintedItem` surface. A host can select a raster document layer and
paint or erase it in document coordinates while every surrounding raster and
vector layer remains visible. The item inverts the selected layer transform
before committing pixels and exposes selected-layer undo/redo.

`createRasterDocument()` creates and selects one transparent raster layer for a
product host that needs an immediately editable canvas. The authoring surface
also exposes the brush spacing and feature toggles, three-point pressure curve,
pressure-to-opacity switch, stabilizer strength, tool mode, live-stroke state,
stroke count, and mouse/tablet input state. Whole-raster replacement accepts an
iiPaintEngine `RasterLayer` directly, so image import does not require a
temporary bitmap file. Model edits still commit atomically on the GUI owning
thread, but frame changes, edits, and `refresh()` schedule coalesced background
layer-tile rendering. `rendering`, `renderCompleted`, `residentTileCount`,
`residentLayerTileCount`, `gpuAccelerated`, and `graphicsBackend` expose that
runtime state. Camera-only
changes update the GPU scene transform immediately and request only missing
prefetch tiles; they do not advance the document presentation revision.

`createInfiniteRasterDocument()` creates the sparse equivalent with a small
initial allocated region. `ensureInfiniteCanvasRegion()` grows that region
outwards on chunk boundaries as the host camera reveals new world space. The
item exposes the allocated world origin and reports each side's growth so a
consumer can resize its visual surface without a visible camera jump.

## Mixed frame rendering

`renderFrame(document, frame)` validates the complete document, resolves static
or hold-sampled raster/vector assets, rasterizes native M/L/Q/C/Z vector paths,
applies the complete iiPaintEngine affine matrix, clips to the document extent,
and composites visible layers bottom-to-top. Source-over, multiply, screen, and
overlay use iiPaintEngine composition semantics. Destination-out remains a
brush/eraser operation and is rejected as a document layer blend mode.

The CPU vector path uses deterministic 4x4 coverage sampling, even-odd fills,
and round stroke footprints. Transformed raster and vector assets use
nearest-neighbor sampling. A singular transform has an empty footprint. The
renderer returns a transparent frame for a valid document with no layers and
fails closed for invalid documents or out-of-range frames.

`renderFrameLayerTiles` renders one selected document layer without lower-layer
pixels. `renderFrameLayers` validates once and returns every layer in stable
document order with visibility, opacity, and blend metadata; `composeFrameLayers`
performs the separate deterministic composition step. `renderFrameRegion` renders one world region into an explicitly bounded output extent, and
`renderFrameTiles` renders a request batch as compatibility conveniences built
on that layer boundary.
`AsyncFrameRenderer` assigns layer work to a bounded number of global thread-pool
workers and retains both the isolated batch and its composite. LOD output dimensions may be
smaller than the world region, so a tens-of-thousands-pixel canvas never needs
a canvas-sized display allocation. Sparse chunks are culled before temporary
surfaces are created, and native vector paths rasterize directly into the tile
output instead of allocating their full viewport.

## Durable `.iisc` format

`encodeIisc()` and `decodeIisc()` round-trip every version 1 document field in
a canonical little-endian binary container. Its 32-byte header carries the
format version, exact payload size, and CRC-32 checksum. Raster payloads select
raw ARGB32 or canonical run-length ARGB32, whichever is smaller. Native vector
commands and keyframe references remain native data and are never silently
rasterized.

Version 1.1 adds finite/infinite canvas mode, an allocated world origin, a
power-of-two chunk size, and canonical sparse raster assets. Version 1.0 files
continue to decode as finite canvases at origin zero.

Version 1.2 appends optional Stable Diffusion generation metadata after the
ordered layer stack. Version 1.0 and 1.1 containers remain readable and
canonical; they decode without generation metadata. Version 1.2 preserves
typed prompt/sampler/model fields, raw ComfyUI graphs, and extension values
without affecting layer rendering.

Decoding exposes the complete public `Document` aggregate rather than an opaque
file handle. Consumers can enumerate bottom-to-top `Layer` variants, distinguish
`BitmapLayer` from `VectorLayer`, and inspect identity, name, visibility,
opacity, transform, blend mode, and static or keyframed asset references.
Image/pixel assets expose `RasterLayer` dimensions
and ARGB pixels; native shape assets expose their viewport, ordered paths,
M/L/Q/C/Z commands and control points, fill, and stroke. See `docs/API.md` for
field-level traversal and validated mutation examples.

The reader checks the full container checksum before parsing, validates UTF-8,
rejects future versions, unknown tags, truncation, trailing bytes, and
non-canonical raster records, and enforces configurable byte, pixel, string,
metadata-entry, asset, layer, path, command, and keyframe limits before
allocation. The binary
container has no archive entry paths, so ZIP path traversal is structurally
absent without introducing an archive or JSON dependency.

## Dependency

The only direct project dependency is iiPaintEngine 0.1.0. Its exported CMake
target supplies the raster types, rasterizer, blend modes, transforms, and its
Qt Core/Gui/Qml/Quick platform targets transitively. `AsyncFrameRenderer` uses
Qt Core `QPromise`, `QFutureWatcher`, and `QThreadPool`; `CanvasItem` uses the
public Qt Quick scene graph texture API. These are already-supplied Qt targets,
so iiSharedCanvas performs no second package discovery and adds no third-party
runtime.

[Adobe DNG 1.7.1](https://helpx.adobe.com/camera-raw/desktop/dng-and-file-formats/digital-negative.html)
is used only as a public semantic reference for common RAW concepts; this
module is not a DNG reader or writer and makes no DNG-compliance claim.
[LibRaw](https://github.com/LibRaw/LibRaw) was reviewed for proprietary
camera-file decoding. Its active camera support and LGPL-2.1/CDDL-1.0 dual
license make it a suitable future decoder adapter, but no decoder was requested
here, so its native dependency and distribution surface are not added to the
core package.

[ComfyUI workflow metadata](https://docs.comfy.org/development/api-development/workflow-metadata),
the [Workflow API format](https://docs.comfy.org/development/api-development/workflow-api-format),
and ComfyUI's
[metadata-writing implementation](https://github.com/Comfy-Org/ComfyUI/blob/master/comfy_api/latest/_ui.py)
define the compatibility reference for `prompt`, `workflow`, KSampler inputs,
and extension metadata. Metadata is retained as exact JSON text because graph
nodes and extension payloads are intentionally open-ended.
[nlohmann/json 3.12.0](https://github.com/nlohmann/json) was reviewed: it is an
actively maintained MIT-licensed single-header JSON library, but introducing
its package and install surface solely for syntax checking would expand the
fixed dependency set. The core therefore uses a bounded standard-library JSON
syntax validator and never parses into a graph model, re-emits, normalizes, or
executes the preserved graphs.

AUTOMATIC1111's
[infotext parser](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/infotext_utils.py),
[generation writer](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/processing.py),
[image metadata carriers](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/images.py),
and [checkpoint hash implementation](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/sd_models.py)
were reviewed as the compatibility reference. The implemented parser is a
small standard-library text reader; no AUTOMATIC1111 runtime, Python package,
model code, or image codec becomes a dependency.

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
The small `ok()` inspectors on frame-render and IISC codec result aggregates
are header-inline. Windows shared-library consumers therefore do not depend on
an unexported member symbol when inspecting a result returned by an exported
operation.

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
    ChunkedBitmapEditor.h
    ChunkedBitmapEditor.cpp
  Camera/
    CameraRaw.h
    CameraRaw.cpp
  Document/
    Document.h
    Document.cpp
    DocumentEditor.h
    DocumentEditor.cpp
  Metadata/
    Automatic1111Metadata.h
    Automatic1111Metadata.cpp
    StableDiffusionMetadata.h
    StableDiffusionMetadata.cpp
  QtAdapter/
    AsyncFrameRenderer.h
    AsyncFrameRenderer.cpp
    BitmapItem.h
    BitmapItem.cpp
    CanvasItem.h
    CanvasItem.cpp
  Render/
    FrameRenderer.h
    FrameRenderer.cpp
  Serialization/
    IiscCodec.h
    IiscCodec.cpp
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
document.layers.emplace_back(BitmapLayer{
    {"paint", "Paint", true, 1.0, {}, RasterBlendMode::SourceOver},
    KeyframedSource{{{0, "frame-0"}}},
});

if (!validate(document).ok()) {
    // Reject before serialization or rendering.
}

BitmapEditor editor(document, "frame-0");
editor.setPixel(10, 10, 0xffffcc00U);
editor.beginStroke({20.0, 20.0}, 1.0);
editor.continueStroke({80.0, 40.0}, 1.0);
editor.endStroke({80.0, 40.0}, 1.0);

const FrameRenderResult frame = renderFrame(document, 0);
if (!frame.ok()) {
    // Reject the frame and surface frame.message to the host.
}

const IiscEncodeResult encoded = encodeIisc(document);
const IiscDecodeResult decoded = encoded.ok()
    ? decodeIisc(encoded.bytes)
    : IiscDecodeResult{};
~~~

For Qt Quick hosts, call `registerIiSharedCanvasQmlTypes()` before loading the
engine and import `iiSharedCanvas 1.0`. `Bitmap` is the isolated selected-asset
item and `SharedCanvas` is the mixed-document item. Product QML must continue
to use LVRS for the application UI; this library provides canvas items, not a
second UI framework.

`iiSharedCanvas.BitmapItemRender` performs an offscreen real render and writes
`build/test-output/bitmap-item.png`. Pixel assertions verify the original and
edited ARGB values as well as nearest-neighbor zoom.
`iiSharedCanvas.FrameRenderer` uses exact pixel assertions for mixed static and
keyframed raster/vector output, transforms, opacity, clipping, and every
supported layer blend mode.
`iiSharedCanvas.CanvasItemRender` constructs `SharedCanvas` through a real QML
engine, verifies frame switching, external refresh, selected raster painting,
inverse-transform input, and undo, and writes
`build/test-output/shared-canvas-item.png`.
With `IISHAREDCANVAS_VERIFY_GPU=1` on a windowed platform, the same executable
also requires a hardware Qt Quick backend and verifies a captured scene-graph
tile image; the normal offscreen CTest does not claim hardware execution.
`iiSharedCanvas.IiscCodec` verifies canonical byte-identical round-trip,
frame-by-frame rendered-pixel identity, corruption and future-version failure,
UTF-8, tag, trailing-data, and allocation-limit enforcement.
`iiSharedCanvas.InfiniteCanvas` verifies signed chunk addressing, camera-driven
region growth, sparse paint, rendering, undo/redo, canonical 1.1 persistence,
1.0 finite migration, and the Qt adapter's exact growth margins.
`iiSharedCanvas.AsyncFrameRenderer` verifies a 65,536 by 49,152 sparse and
native-vector document through bounded 128 by 128 LOD output, immutable worker
snapshots, and owner-thread completion.
`iiSharedCanvas.CameraRaw` verifies CFA, monochrome, and linear sample layouts;
active-area anchored patterns; calibration and metadata validation; and
bounds-checked sample, channel, black-level, and white-level access.
`iiSharedCanvas.StableDiffusionMetadata` verifies typed prompt, sampling,
resource, and LoRA fields; strict ComfyUI JSON; duplicate keys; invalid numeric
settings; canonical 1.2 round-trip; legacy 1.1 preservation; and metadata
allocation limits.
`iiSharedCanvas.Automatic1111Metadata` verifies official infotext splitting and
defaults, multiline prompts, quoted and escaped values, Hires projection,
resource/hash compatibility, duplicate and future fields, malformed input,
canonical UTF-8, and byte-exact format-1.2 round-trip.

## Documents

- docs/API.md defines Camera RAW and Stable Diffusion metadata objects, public
  document data ownership, stable-id lookup, validated mutation, failure,
  lifetime, revision, and threading contracts.
- docs/BLUEPRINT.md defines ownership, dependency direction, milestones, and
  completion gates.
- docs/FORMAT.md defines the implemented canonical `.iisc` binary contract.
- AGENTS.md fixes the engineering rules that future changes must preserve.

## License

The project is AGPL-3.0-only because iiPaintEngine is an AGPL-3.0-only public
dependency. See LICENSE and NOTICE.md.
