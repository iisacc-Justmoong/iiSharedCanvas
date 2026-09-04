# iiSharedCanvas

iiSharedCanvas is a C++20 document and authoring foundation for composing
raster artwork, native vector paths, and frame-based raster or vector animation
on one canvas. It also provides format-neutral decoded Camera RAW authoring
data for import pipelines and a format-neutral video-editing timeline model.
It is the authoritative canvas standard for iisacc
products; applications adopt its model and format through application-owned
adapters.

The repository contains the versioned in-memory model, validation rules,
deterministic keyframe evaluation, bounded mixed raster/vector tile rendering,
an editable raster-asset boundary, sparse infinite-canvas chunks, asynchronous
Qt Quick scene-graph presentation, CMake package export, canonical `.iisc`
serialization, write-through document files, and contract tests. Cross-platform
device profiling, partial decode, and hardware power-loss testing remain later
milestones.

Consumer applications do not shape this public contract in parallel. A library
change is completed and verified here first; consumers then conform to that
fixed version through their own bridges. Existing product ViewModels, QML names,
session-layer models, and tool conventions are not compatibility requirements
for iiSharedCanvas.

## Write-through authoring

`DocumentFile` owns a working canvas file. Bind the structural, vector, bitmap,
chunked-bitmap, or Qt editors to that file: every accepted content change is
written before its editing call returns, including stroke increments, cancel,
undo, and redo. Changed records and pixel spans are updated transactionally;
there is no manual save, delayed autosave, or whole-document dump per edit.

`CanvasItem::createFile` and `openFile` expose the working-file path to Qt Quick.
`DocumentFile::edit` covers application-defined aggregate edits with the same
validation and failure rollback. The committed document is const; standalone
`Document` and pathless canvas creation remain explicitly in-memory APIs.
Working files use SQLite, while `encodeIisc` and `decodeIisc` retain the legacy
binary snapshot contract for explicit interchange. Existing snapshot files
are never silently converted or overwritten. See [the persistence contract](docs/PERSISTENCE.md).

## Content contract

| Authoring input | Stored render content | Initial behavior |
| --- | --- | --- |
| Bitmap image | iiPaintEngine RasterLayer | Static layer |
| iiPaintEngine brush | Committed ARGB pixels | Static or keyframed raster asset |
| Infinite raster paint | Committed ARGB pixels in touched world chunks | Static sparse raster layer |
| Decoded Camera RAW | Unsigned sensor samples plus calibration and capture metadata | Import-side aggregate, not a document layer |
| Stable Diffusion recipe | Typed generation settings plus exact workflow metadata | Optional document metadata, never render content |
| Vector path | M/L/Q/C/Z path commands with solid fill or stroke | Static layer |
| Raster keyframes | Frame-owned raster asset references | Integer-frame hold sampling |
| Vector keyframes | Frame-owned vector asset references | Integer-frame hold sampling |

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

`parseStableDiffusionGenerationParameters` reads Stable Diffusion generation-
parameters text without making a producer or image codec part of the canvas
model. AUTOMATIC1111 is the reference-compatible producer, not the public type
identity. The parser keeps the complete source text, multiline positive and
negative prompts, and every ordered key/value pair, including duplicate and
future extension keys. JSON-
quoted values decode commas, colons, escapes, and Unicode for typed access;
`findStableDiffusionGenerationParameter` follows AUTOMATIC1111's last-value-wins behavior.
The common projection maps output size, batch and CLIP settings, base and Hires
sampler passes, checkpoint/VAE/Hires/refiner resources, software version, and
10-character SHA-256 prefixes. Unmapped settings remain unique generic extras,
while the raw infotext remains authoritative and byte-exact.

The text format alone cannot prove which application wrote it. Parsing does
not fill `StableDiffusionMetadata::software`; a carrier adapter that has
verified producer information may set that provenance field explicitly.

The compatibility boundary follows AUTOMATIC1111's official
[`parse_generation_parameters`](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/infotext_utils.py),
including the three-field terminal-line test, default CLIP skip of 1, default
`Automatic` schedule, inherited Hires sampler/scheduler, and Hires step value
zero meaning the main step count. Its official
[image metadata implementation](https://github.com/AUTOMATIC1111/stable-diffusion-webui/blob/master/modules/images.py)
places this text in PNG `parameters` or an image-format metadata carrier such
as EXIF `UserComment`. The metadata parser accepts the extracted UTF-8 string
without a codec dependency. The bitmap adapter now extracts PNG text carriers;
it does not add Pillow or claim generic EXIF `UserComment` extraction.

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

## Video editing timeline data

`TimelineProject` is the application-neutral authoring model for a non-linear
video editor. It owns media sources and their original/proxy representations,
typed video/audio/subtitle/data streams, any number of sequences, typed tracks
and clips, transitions, effects and parameter automation, markers, bins, link
groups, and independent render profiles. This model is deliberately separate
from `Document::timeline`: the latter remains the compact integer-frame domain
for canvas layers, while a video project needs multiple time bases, media
references, audio sample timing, and delivery settings.

Time values are signed 64-bit ticks interpreted by an explicit positive
`TimelineTimeBase`. Frame rates are exact rationals, so sequence and delivery
settings may independently use 24, 30, 60, 24000/1001, 30000/1001, or
60000/1001 without persisted floating-point timestamps. Source video timing
separately retains constant- or variable-frame-rate mode, nominal/average/
minimum/maximum rates, and per-sample PTS, optional DTS, duration, keyframe,
byte offset, and byte size. Timecode counting, including drop-frame counting,
does not replace media timestamps. Media, nested-sequence, and generated clip
sources each supply a source time base. With no loop or time map, the rational
playback rate must exactly relate source duration to sequence duration; an
explicit time map is authoritative and therefore requires a unit playback rate.

Container and codec identifiers are open strings rather than closed enums.
Common technical fields cover container brands, MIME types and extensions;
codec profile, level, tag, implementation and bitrate; video extent, pixel
aspect, pixel format, chroma, scan, alpha and HDR color data; and audio sample
rate, layout, format and loudness. Ordered typed option lists preserve future
muxer, decoder, and encoder parameters without changing the public API.
Original, proxy, and optimized representations reuse logical stream ids only
when stream kind, time base, start, and duration are identical; switching a
representation can therefore change codec or resolution without changing a
clip's source-trim meaning.

`validateTimelineProject` fails closed on invalid rationals and numeric values,
duplicate identities or properties, missing references, stream/track kind
mismatches, invalid ranges and samples, automation order, transitions, and
render profiles. Visual clips require a sequence canvas, audio clips require a
mix sample rate/layout, transition alignment is checked against its adjacent
cut, and subtitle image cues must resolve representation attachments.
`TimelineEditor` is the validated structural mutation API for
sources, sequences, tracks, clips, and render profiles. It also changes FPS,
container, and video/audio codecs atomically; a rejected edit preserves both
the project and editor revision. An invalid rebind also preserves the existing
binding and revision instead of detaching the editor.

These timeline objects remain standard-library-only authoring data, not a
sequence renderer or audio mixer. The separate `Video/VideoCodec.h` adapter
now probes, decodes and encodes canvas animation using an optional FFmpeg
runtime. `TimelineProject` is not encoded by `.iisc` version 1.3.

## Layered document, bitmap, vector and video interchange

The installed public API includes real byte/file import and export adapters:

| Content | Import | Export |
| --- | --- | --- |
| Layered document | OpenRaster PNG layers; PSD v1 8-bit RGB pixel layers, with names, order, visibility, opacity, offsets and supported blending | PSD at frame zero; vectors as embedded PDF Smart Objects, bitmaps as pixel layers; native `.iisc` through `DocumentFile::create` or `encodeIisc` |
| Bitmap | Qt readers: PNG, JPEG, BMP, TIFF, WebP, HEIC, JP2, icons and portable bitmaps; optional extended TGA/QOI/EXR/DPX/HDR/PCX/SGI, PSD composite and DDS | Available Qt writers plus extended TGA/QOI/EXR/DPX/HDR/PCX/SGI; alpha matte and PNG text controls |
| Editable vector | Solid SVG/SVGZ paths, shapes, transforms, linear/quadratic/cubic segments and arc conversion | Native SVG/SVGZ; PDF with vector paths and separate bitmap drawing |
| Rasterized vector | Explicit SVG/PDF page rasterization through an installed Qt plugin | Bitmap/frame output at the canvas extent |
| Canvas animation | FFmpeg local video/animation to a bitmap layer with frame-owned keys | FFV1/Matroska, MP4/H.264 or HEVC, WebM/VP9, MOV/ProRes or Animation, AVI, GIF, APNG and other supported container/encoder combinations |

`bitmapFormats()` and `videoCapabilities()` report actual runtime support,
separately for reading and writing. They do not promise every profile/subtype
or redistribute optional codecs. Imports return detached assets/documents;
insert imported values through `DocumentFile::edit` for immediate durable
updates. Exports never substitute for write-through document editing.

Conversions report losses such as alpha removal, reduced precision, stroke
outlining, rasterization, resampled video timing and omitted audio. Unsupported
SVG drawing features fail the complete editable import; they are not silently
dropped. Defaults protect existing exports and all working `.iisc` files.
Limits, codec deployment, supported SVG details and examples are specified in
[MEDIA_IO.md](docs/MEDIA_IO.md).

`importLayeredDocument(path)` and `decodeLayeredDocument(bytes)` return a
validated native document with separate bitmap layers, not one merged preview.
Unrepresentable layer semantics fail closed; supported ungrouping and omitted
metadata are reported as warnings. `layeredDocumentFormats()` lists only the
implemented subset readers and PSD writer. The installed `iisc-import input.ora output.iisc`
command creates a new immediately editable working file and refuses existing
destinations. See [LAYERED_IMPORT_CLI.md](docs/LAYERED_IMPORT_CLI.md),
[OPENRASTER_IMPORT.md](docs/OPENRASTER_IMPORT.md) and
[PSD_IMPORT.md](docs/PSD_IMPORT.md) for precise format and safety contracts.

`exportPsd(document, "/art/output.psd")` exports native frame zero, retaining
vectors as embedded vector PDF Smart Objects, names, order, visibility and
supported blending. Animation is intentionally reduced to its first frame.
Integer-translated bitmaps keep their pixels/offsets; other transforms/chunks
are baked into canvas-clipped pixel layers, with loss warnings.
`encodePsd(document)` returns bytes without filesystem I/O. Neither
mutates the native document, and file export refuses existing destinations by
default. See [PSD_EXPORT.md](docs/PSD_EXPORT.md) and
[PSD_EXPORT_CLI.md](docs/PSD_EXPORT_CLI.md). The strict PSD importer does not
yet import Smart Objects; keep `.iisc` as the complete editable master.

## Data access and structural editing

Every persisted field remains directly available through the public aggregate
types in `Document/Document.h`. Stable-id lookup helpers expose typed raster and
vector assets, sparse frames, exact keyframes, collection indices, and every
static or keyframed reference to an asset.

The ordered layer stack is a `BitmapLayer | VectorLayer` variant. Both concrete
types have the same `LayerProperties + LayerSource` structure, while validation
and `DocumentEditor` reject every cross-type asset reference. Bitmap layers may
hold only finite or chunked raster assets; vector layers may hold only native
vector assets. `LayerProperties::frameRange` optionally stores an inclusive
`LayerFrameRange {firstFrame, lastFrame}`. Both boundaries belong to the layer;
an absent range means that the layer exists throughout the current document
timeline. A range limits rendering, not animation storage, so frame-owned
keyframes outside it remain valid and become effective again if the range is
later extended. `Document::frames` owns the sparse ordered frame records, and
every `Frame` directly owns its `{layerId, assetId}` keyframes. A frame may
therefore hold raster and vector keys together without either layer owning a
second keyframe collection. `KeyframedSource::frameIndices` is a derived
secondary index, not keyframe ownership: it contains the exact increasing set
of frames that own a key for that layer. Validation rejects every missing,
extra, duplicate, or out-of-order index entry.

`Frame::keyframes` uses canonical ascending `layerId` order. Exact lookup and
hold sampling therefore use binary search over the layer's derived owner-frame
index and the selected frame. Direct aggregate authors must update the frame
owners and derived index together and then call `validate`; `DocumentEditor`
and `decodeIisc` maintain both sides automatically.

`DocumentEditor` is the validated structural mutation API. It edits canvas
extent, frame rate/count, assets, layer properties/order/sources, keyframes, and
vector path collections. Successful operations keep the complete document
valid and increment a monotonic editor revision once. Rejected operations
return a typed code, path, and message without partially changing the document.
Asset renaming rewrites all references atomically; referenced asset removal is
rejected instead of cascading silently. An invalid rebind preserves the current
document and revision. `insertKeyframedLayer` inserts an animated layer and all
of its frame-owned keys as one validated commit. `setLayerFrameRange` sets or
clears the optional inclusive existence range atomically and upgrades an older
document to format 1.3 only when a range is committed. A rejected range restores
both the prior layer and format version. Shrinking `frameCount` is likewise
rejected if it would exclude an explicit layer boundary or a keyframe.

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

## Native vector editing

`VectorEditor` binds to a vector asset by id and resolves that stable identity
for every operation. It creates, inserts, reorders, and removes paths; edits the
vector viewport and fill/stroke paint; and works on native path commands without
rasterizing them. Linear segments use `appendLineTo`, while quadratic and cubic
Bezier segments use `appendQuadraticBezierTo` and `appendCubicBezierTo`.
`setAnchorPoint` moves the endpoint of M/L/Q/C commands and `setControlPoint`
addresses the one quadratic or two cubic control points.

Every command edit is performed on a path copy and committed through the
validated `DocumentEditor` replacement boundary. Non-finite coordinates,
missing paint, invalid stroke widths, missing assets, wrong content kinds, and
out-of-range indices therefore fail without partially changing the document or
advancing `revision()`. `ClosePath` has no editable anchor, and direct batch
mutation remains available through the public aggregate followed by
`validate(document)`. The editor owns no hidden geometry, input trajectory, or
serialized history.

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
performs the separate deterministic composition step. A present layer
`frameRange` includes both `firstFrame` and `lastFrame`; outside that range the
isolated layer keeps its identity and order but reports `visible == false`,
allocates no tiles, and contributes nothing to composition. An absent range
means the layer is eligible for the whole timeline.
`renderFrameRegion` renders one world region into an explicitly bounded output
extent. `renderFrameTiles` renders a request batch as a compatibility
convenience built on that layer boundary.
`AsyncFrameRenderer` validates each immutable request snapshot exactly once on
the thread pool before dispatching any layer worker. An invalid snapshot returns
that preflight error without producing partial layer output; a valid snapshot
assigns layer work to a bounded number of global thread-pool workers and retains
both the isolated batch and its composite. LOD output dimensions may be
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
opacity, transform, blend mode, static references, and sparse frames that
directly own animated layer references.
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
absent from the canonical codec. The separate OpenRaster importer uses libzip
and validates foreign entry paths without extracting them.

## Dependency

The direct project dependencies are iiPaintEngine 0.1.0, SQLite 3.26 or newer,
zlib 1.2.9 or newer, and libzip 1.7.3 or newer (CMake target `libzip::zip`).
SQLite is a private implementation dependency for durable working files;
[the review](docs/DEPENDENCIES.md) covers maintenance, public-domain licensing,
footprint, and platform packaging. The API minimum does not pin an old runtime.
zlib supplies SVGZ/PSD compression and PNG integrity checks; libzip supplies
bounded OpenRaster ZIP reads. Optional FFmpeg/ffprobe executables are supplied
by the application; this package neither downloads nor bundles them. Their
maintenance, configuration-dependent licensing and footprint are reviewed in
[DEPENDENCIES.md](docs/DEPENDENCIES.md).
iiPaintEngine's exported CMake
target supplies the raster types, rasterizer, blend modes, transforms, and its
Qt Core/Gui/Qml/Quick platform targets transitively. `AsyncFrameRenderer` uses
Qt Core `QPromise`, `QFutureWatcher`, and `QThreadPool`; `CanvasItem` uses the
public Qt Quick scene graph texture API. These are already-supplied Qt targets,
so these rendering features add no further package discovery or runtime.

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
layout, from $HOME/.local/SDK/iiPaintEngine.
The generated library records the selected iiPaintEngine runtime directory and
the standard sibling-install fallback so Debug, Release, and installed
consumers resolve the same engine binary.
The small `ok()` inspectors on frame-render and IISC codec result aggregates
are header-inline. Windows shared-library consumers therefore do not depend on
an unexported member symbol when inspecting a result returned by an exported
operation.

The current C++ package version is 0.8.0 with SOVERSION 0.8 and exact-version
CMake package matching. Consumers must rebuild against the new installed
package to adopt the layered-document APIs. The canonical snapshot model remains version 1.3;
1.0 through 1.3 compatibility is tested with fixed legacy goldens. Working-file
schema 1 is identified separately by its SQLite header and application id.

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
    StableDiffusionGenerationParameters.h
    StableDiffusionGenerationParameters.cpp
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
  Vector/
    VectorEditor.h
    VectorEditor.cpp
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
    KeyframedSource{{0}},
});
document.frames.push_back({0, {{"paint", "frame-0"}}});

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
frame-owned keyframed raster/vector output, transforms, opacity, clipping, and every
supported layer blend mode.
`iiSharedCanvas.CanvasItemRender` constructs `SharedCanvas` through a real QML
engine, verifies frame switching, external refresh, selected raster painting,
inverse-transform input, and undo, and writes
`build/test-output/shared-canvas-item.png`.
With `IISHAREDCANVAS_VERIFY_GPU=1` on a windowed platform, the same executable
also requires a hardware Qt Quick backend and verifies a captured scene-graph
tile image; the normal offscreen CTest does not claim hardware execution.
`iiSharedCanvas.IiscCodec` verifies canonical byte-identical round-trip,
fixed 1.0 through 1.2 golden containers emitted by package 0.2.0,
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
`iiSharedCanvas.StableDiffusionGenerationParameters` verifies official infotext splitting and
defaults, multiline prompts, quoted and escaped values, Hires projection,
resource/hash compatibility, duplicate and future fields, malformed input,
canonical UTF-8, and byte-exact format-1.2 round-trip.

## Documents

- docs/OPENRASTER_IMPORT.md and docs/PSD_IMPORT.md define layer-preserving
  import subsets, safety limits, unsupported features and metadata handling.
- docs/LAYERED_IMPORT_CLI.md defines the installed file-to-file converter.
- docs/API.md defines Camera RAW and Stable Diffusion metadata objects, public
  document data ownership, stable-id lookup, validated mutation, failure,
  lifetime, revision, and threading contracts.
- docs/BLUEPRINT.md defines ownership, dependency direction, milestones, and
  completion gates.
- docs/FORMAT.md defines the implemented canonical `.iisc` binary contract.
- AGENTS.md fixes the engineering rules that future changes must preserve.

## Editable timeline exchange

`exportTimelineInterchange` and the installed `iisc-export-timeline` CLI create
a new directory with legacy XML (Premiere/Resolve), FCPXML (Final Cut Pro),
separate layer-state PNGs and `source.iisc`. Native layers remain independent
tracks/lanes and every hold-key interval remains a clip with exact timing.
This is an XML import workflow, not a native `.iisc` plug-in. Vector geometry
and spatial transforms are projected into clip pixels; the native snapshot
retains full editability. See [the contract](docs/TIMELINE_INTERCHANGE.md) and
[CLI usage](docs/TIMELINE_INTERCHANGE_CLI.md).

## License

The project is AGPL-3.0-only because iiPaintEngine is an AGPL-3.0-only public
dependency. See LICENSE and NOTICE.md.

### SDK installation layout

The checkout is `Workspace/SDK/iiSharedCanvas`. Both direct CMake configuration
and `./install.sh` default to `~/.local/SDK/iiSharedCanvas`; iiPaintEngine is
resolved from the sibling SDK installation. `IISHAREDCANVAS_INSTALL_PREFIX`
and `IISHAREDCANVAS_IIPAINTENGINE_PREFIX` retain explicit custom paths. The
installer regenerates build and consumer CMake caches after checkout moves.
`iiSharedCanvas.InstallScript` tests the default and custom installation prefix.

Public headers resolve the generated visibility header as
`iiSharedCanvas/Export.h`. The exported include directories support this
qualified path alongside the existing short public includes. This prevents
collisions when another installed SDK also exports an `Export.h`. Both the
public API compile test and installed consumer put an unrelated `Export.h`
first in the include search path as a regression check.

On macOS, the installed CMake target also supplies its runtime search path when
`LIBRARY_PATH` makes the package an implicit linker directory. The installer
builds its consumer with that condition and runs it without `DYLD_LIBRARY_PATH`
or `DYLD_FALLBACK_LIBRARY_PATH` to verify normal package loading.
