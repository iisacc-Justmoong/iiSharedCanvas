# iiSharedCanvas `.iisc` format version 1

Status: Implemented canonical binary contract.

This document specifies the version-1 **interchange snapshot**, not the live
authoring store. [PERSISTENCE.md](PERSISTENCE.md) specifies SQLite working files
with application id `0x49495343`, schema 1, and incremental write-through edits.
Both use `.iisc`; distinguish their headers. `encodeIisc`/`decodeIisc` preserve
the original snapshot bytes, and `DocumentFile::open` does not rewrite legacy
snapshots. Import explicitly into a new working file before editing it.

## Identity

- Extension: `.iisc`
- Media type: `application/vnd.iisacc.ii-shared-canvas`
- Current model version: major 1, minor 3
- Integer byte order: little-endian
- Floating-point representation: IEEE 754 binary64, stored as little-endian bits
- Raster channel representation: 32-bit ARGB as defined by iiPaintEngine
- Text representation: canonical UTF-8 prefixed by a little-endian `u32` byte count

Version 1 is a single binary container. It is not ZIP and has no entry names or
internal file paths. This removes archive ordering, recursion, and path
traversal from the format without adding an archive or JSON dependency.

## Container header

Every container begins with this exact 32-byte header:

| Offset | Size | Field | Contract |
| ---: | ---: | --- | --- |
| 0 | 8 | magic | `49 49 53 43 0D 0A 1A 0A` (`IISC\r\n\x1A\n`) |
| 8 | 2 | major | document format major |
| 10 | 2 | minor | document format minor |
| 12 | 4 | flags | zero in version 1 |
| 16 | 8 | payload size | exact number of following bytes |
| 24 | 4 | checksum | CRC-32/ISO-HDLC of the payload |
| 28 | 4 | reserved | zero in version 1 |

An exact payload follows immediately. A reader rejects truncated payloads,
bytes after the declared payload, non-zero flags or reserved fields, checksum
mismatch, and unsupported versions before exposing a document.

## Primitive encoding

- `u8`, `u16`, `u32`, `u64`, and `i32` are fixed-width little-endian values.
- `f64` is the little-endian bit representation of an IEEE 754 `double`.
- Boolean values are a `u8` and must be exactly 0 or 1.
- Strings are `u32 byteCount` followed by exactly that many canonical UTF-8
  bytes. Embedded NUL bytes are valid UTF-8 data and do not terminate a string.
- Counts are `u32`; aggregate reader limits are checked before `reserve` or
  pixel allocation.

## Payload order

The payload has one canonical sequence:

~~~text
i32 canvasWidth
i32 canvasHeight
[if minor >= 1]
  u8 canvasMode       // 0 finite, 1 infinite
  [if canvasMode == 1]
    i32 canvasOriginX
    i32 canvasOriginY
    i32 chunkSize
u32 frameRateNumerator
u32 frameRateDenominator
u32 frameCount

u32 assetCount
Asset assets[assetCount]

u32 layerCount
Layer layers[layerCount]

[if minor >= 2]
  bool hasStableDiffusionMetadata
  [if hasStableDiffusionMetadata]
    StableDiffusionMetadata generation
~~~

Assets and layers preserve their document vector order. Layers are ordered
bottom-to-top.

Version 1.1 adds infinite-canvas metadata between the extent and timeline.
`canvasWidth` and `canvasHeight` describe the currently allocated render region,
not a conceptual boundary. Its top-left world coordinate is
`(canvasOriginX, canvasOriginY)`. The chunk size is a power of two from 32
through 4096 pixels. Version 1.0 omits this block and always decodes as a finite
canvas with origin `(0, 0)`.

Version 1.2 appends optional Stable Diffusion generation metadata after the
layer collection. Appending the block leaves every earlier offset and record unchanged.
Version 1.0 and 1.1 end immediately after the layer collection and therefore
decode with no generation metadata.

Version 1.3 appends an optional inclusive frame range to each layer record,
immediately after that layer's complete source payload. Version 1.0 through 1.2
layer records end after their source and contain no range bytes, so their
canonical encoding remains unchanged. The generation-metadata block continues
to follow the complete layer collection.

Within the existing 1.2 metadata record, `generationParametersText` contains the
complete Stable Diffusion generation-parameters text extracted from a compatible
image carrier. Generation-parameters text remains byte-exact during canonical
`.iisc` round-trip; parsing creates
a separate decoded view and never rewrites this string. PNG `parameters`, EXIF
`UserComment`, or other carrier identity is not persisted because extraction
belongs to the importing adapter. Unknown or duplicate parameter pairs remain
recoverable from this authoritative string even when their final semantic value
is also projected into typed fields or `extraParameters`. This compatibility
reader changes no 1.2 binary field or offset and therefore requires no format
version increment.

## Assets

Every asset starts with:

~~~text
u8 kind       // 0 raster, 1 vector, 2 chunked raster (minor >= 1)
string id
~~~

Asset ids are document identities, not paths.

### Raster asset

~~~text
i32 width
i32 height
u64 pixelCount
u8 encoding       // 0 raw ARGB32, 1 run-length ARGB32
u64 encodedBytes
u8 data[encodedBytes]
~~~

`pixelCount` must equal `width * height`. Raw encoding contains exactly one
little-endian `u32` per pixel in row-major order. Run-length encoding contains
records of `u32 runLength, u32 argb`; run lengths are positive, fill the exact
pixel count, and adjacent records cannot repeat the same ARGB value.

The writer uses run-length encoding only when its byte count is strictly less
than raw ARGB32. Otherwise it uses raw encoding. A reader rejects the opposite
choice, so one raster has one canonical representation.

Brush trajectories, pressure samples, curves, dabs, and replay commands cannot
appear here. iiPaintEngine commits brush output first and `.iisc` stores only
the resulting pixels.

### Chunked raster asset

~~~text
u32 chunkCount
repeat chunkCount:
  i32 column
  i32 row
  RasterPayload pixels
~~~

`RasterPayload` is the width, height, pixel count, encoding, encoded byte count,
and data sequence defined by Raster asset above; it does not repeat an id.
Every chunk is exactly `chunkSize` by `chunkSize`. `(column, row)` identifies
world pixel region `[column * chunkSize, (column + 1) * chunkSize)` by
`[row * chunkSize, (row + 1) * chunkSize)`, including negative coordinates.
Chunks are unique and encoded in ascending row then column order. Missing
chunks are transparent and consume no raster payload. A chunked raster is valid
only in an infinite canvas and first appears in version 1.1.

### Vector asset

~~~text
i32 viewportWidth
i32 viewportHeight
u32 pathCount
VectorPath paths[pathCount]
~~~

A vector path is:

~~~text
u32 commandCount
PathCommand commands[commandCount]
bool hasFill
[u32 fillArgb]
bool hasStroke
[u32 strokeArgb, f64 strokeWidth]
~~~

Path command tags and fields are:

| Tag | Operation | Fields |
| ---: | --- | --- |
| 0 | MoveTo | `f64 x, f64 y` |
| 1 | LineTo | `f64 x, f64 y` |
| 2 | QuadraticTo | control point, end point |
| 3 | CubicTo | control 1, control 2, end point |
| 4 | ClosePath | none |

Unknown tags fail closed. A valid path begins with MoveTo and has a visible fill
or stroke. Vector geometry remains native across save/load and migration.

## Layers and timeline

A layer is encoded as:

~~~text
string id
string name
bool visible
f64 opacity
f64 m11, m12, m21, m22, translationX, translationY
u8 blendMode
u8 sourceKind
LayerSource source
[if minor >= 3]
  bool hasFrameRange
  [if hasFrameRange]
    u32 firstFrame
    u32 lastFrame
~~~

Blend tags are 0 source-over, 1 multiply, 2 screen, and 3 overlay.
iiPaintEngine destination-out is reserved for immediate brush erasing and is
not a persisted layer blend mode.

The affine transform maps `(x, y)` as follows:

~~~text
x' = x * m11 + y * m21 + translationX
y' = x * m12 + y * m22 + translationY
~~~

Source kind 0 is static and stores one asset-id string. Its concrete layer type
is the referenced asset kind. Source kind 1 marks an animated layer and stores
the following layer-major wire projection:

~~~text
u8 contentKind       // 0 BitmapLayer, 1 VectorLayer
u32 keyframeCount
repeat keyframeCount:
  u32 frame
  string assetId
~~~

The wire layout remains unchanged for `.iisc` 1.0 through 1.2. In memory,
`Document::frames` owns strictly increasing sparse `Frame` records and every
actual `Keyframe`. `KeyframedSource::frameIndices` is only a validated derived
index containing the exact increasing frame set owned for that layer. The
decoder fills that index from each enclosing wire record, transposes its keys
into `Frame{index, Keyframe{layerId, assetId}}`, and orders simultaneous keys by
layer id. The encoder projects those frame-owned keys back into document layer
order, so a non-lexicographic layer stack still re-encodes to identical bytes.
Empty frames and two keys for the same layer in one frame are invalid because
the existing wire format cannot represent them.
The encoded `contentKind` is the owning layer's canonical type tag, and
validation rejects a key whose layer is static or whose asset kind differs
from the referenced layer.

Version 1 uses hold sampling only. The selected asset is the last frame-owned
key for the requested layer at or before the requested frame. Sparse frame
indices are strictly increasing and remain within `[0, frameCount)`; every
animated layer has a frame-zero key, and one frame may directly own keys for
multiple raster and vector layers.

Frame-range endpoints are zero-based and inclusive. A present range satisfies
`firstFrame <= lastFrame < frameCount`; the layer does not exist and contributes
no rendered pixels outside that interval. An absent range means that the layer
exists throughout the complete timeline. The range applies equally to static
and keyframed bitmap or vector layers. It is a non-destructive existence gate:
keyframes may remain outside the range, and entering the range immediately uses
the last frame-zero-based hold value at or before that frame.

## Stable Diffusion generation metadata

The optional version 1.2 record is encoded in this exact order:

~~~text
string positivePrompt
string negativePrompt
bool hasOutputExtent
[if hasOutputExtent]
  u32 outputWidth
  u32 outputHeight
OptionalU32 batchSize
OptionalU32 clipSkip

u32 samplingPassCount
repeat samplingPassCount:
  string nodeId
  OptionalU64 seed
  OptionalU32 steps
  OptionalF64 cfgScale
  string samplerName
  string scheduler
  OptionalF64 denoiseStrength
  OptionalU32 startStep
  OptionalU32 endStep

u32 modelCount
repeat modelCount:
  string role
  string name
  string hash
  string hashType
  string uri

u32 loraCount
repeat loraCount:
  string name
  string hash
  f64 modelStrength
  f64 clipStrength

string software
string softwareVersion
string createdAt
string generationParametersText
string comfyUiPromptJson
string comfyUiWorkflowJson

u32 comfyUiExtraPngInfoCount
repeat comfyUiExtraPngInfoCount:
  string key
  string jsonValue

u32 extraParameterCount
repeat extraParameterCount:
  string key
  string value
~~~

`OptionalU32`, `OptionalU64`, and `OptionalF64` are a canonical boolean presence
byte followed by the indicated primitive only when present. Output extent,
batch size, CLIP skip, and step count are positive when present. CFG is finite
and non-negative; denoise strength is finite from zero through one; start step
does not exceed end step. A sampling pass contains at least one setting.

Model resources require a role and name. A role may identify a checkpoint, VAE,
ControlNet, or another consumer-understood model class. Hash, hash type, and URI
are provenance strings rather than trusted resolvers. LoRA strengths are finite.
No checkpoint, VAE, LoRA, embedding, or custom-node binary is embedded by this
record.

ComfyUI `prompt` and `workflow` values are independent, optional JSON objects.
Their UTF-8 bytes are retained exactly; the writer does not parse and re-emit
them. `prompt` represents the API execution graph and `workflow` represents the
UI-restoration graph. Extra PNG-info keys are non-empty, unique, and cannot be
the reserved `prompt` or `workflow` names; their values are complete JSON
values. Generic extra-parameter keys are non-empty and unique, while their
values are opaque UTF-8.

An attached metadata record must contain at least one prompt, setting,
resource, compatibility payload, or extension. It is untrusted authoring and
provenance data: decoding never executes a graph, resolves a URI, loads a model,
or changes rendered pixels.

## Render contract

Raster and vector assets use nearest-neighbor affine sampling and clip to the
document canvas. A singular matrix produces an empty layer footprint. Vector
fills use the even-odd rule; strokes use round footprints. M/L/Q/C/Z paths use
deterministic CPU rasterization with a 4x4 coverage grid per pixel. Resolved
layers are composited bottom-to-top using iiPaintEngine opacity and blend
semantics.

Layer-parallel rendering does not change the persisted layer order. Isolated
layer tiles, worker completion order, resident texture caches, and the final
composed tile are runtime state only. Composition always restores the encoded
bottom-to-top order before applying opacity and blend mode.

## Authoring-only state

`TimelineProject` is not encoded by `.iisc` version 1.3. The video-editing
model has multiple sequences, source representations, typed streams, tracks,
clips, effects, markers, and render profiles whose references and media timing
do not belong to the canvas payload above. Adding durable video-project storage
requires a separately versioned format with explicit resource limits and
lossless round-trip tests; merely adding the in-memory model does not change
`CurrentFormatMinor`.

Container and codec descriptors in `TimelineProject` are declarations, not
proof that the host can probe, decode, encode, or mux them. Those operations
remain adapter responsibilities and write no hidden state into `.iisc`.

Brush input is not a persisted content kind. `BitmapEditor` may retain a
transient iiPaintEngine dab stream only while a pointer gesture is active.
`BitmapItem` and `CanvasItem` viewport state, layer selection, undo history,
input events, and UI tool state are not encoded.

`CameraRawData` is not encoded by `.iisc` version 1.1 and remains outside
version 1.3. It is decoded/import state containing sensor samples and
capture/calibration metadata, not a canvas asset or a third layer kind. A host
must explicitly process it into committed
ARGB pixels and then create a `RasterAsset` if the result belongs in a document.
No manufacturer RAW bytes, CFA payload, RAW profile, or implicit demosaicing
settings are added to the current container.

## Default reader limits

`SerializationLimits` is caller-configurable. Defaults are:

| Resource | Default maximum |
| --- | ---: |
| Container bytes | 1 GiB |
| Canvas or vector viewport pixels | 256 Mi pixels |
| Total raster pixels | 256 Mi pixels |
| Sparse raster chunks | 1,048,576 |
| Assets | 65,536 |
| Layers | 65,536 |
| Sparse frames that own keys | 262,144 |
| One string | 1 MiB |
| One generation-metadata string | 16 MiB |
| Total string bytes | 64 MiB |
| Generation-metadata entries across passes, models, LoRAs, and extension lists | 65,536 |
| Vector paths | 1,048,576 |
| Path commands | 16,777,216 |
| Keyframes | 16,777,216 |

The reader checks declared counts and aggregate totals before allocating the
corresponding collections or pixel buffers. It derives the unique sparse-frame
count from bounded pending keyframes and checks the frame limit before reserving
or materializing `Document::frames`. Because decoding materializes the
enclosing layer id into every frame-owned `Keyframe`, the total-string limit
also counts those derived copies on both encode and decode; a small wire record
therefore cannot amplify into unbounded repeated layer-id storage. A product
may lower these limits for its device class.
Layer-range endpoints allocate no collections, but both endpoints are checked
against the timeline's bounded `frameCount` before a decoded document is
exposed.

## Compatibility and migration

- A reader accepts the current major and a minor no newer than it implements.
- A newer major or minor fails before payload parsing.
- Header flags and reserved fields make future opt-in changes explicit.
- A writer emits one canonical representation for its implemented version.
- Migration must preserve raster pixels, vector geometry, layer order,
  transforms, and timeline references. It must never silently rasterize vector
  assets, discard animation, or introduce retained brush trajectories.

Version 1.0 is the first physical format. Version 1.1 adds finite/infinite mode,
an allocated-region world origin, a chunk size, and canonical sparse raster
assets. A 1.0 document migrates on decode to finite mode at origin zero without
changing pixels or rendering. Version 1.2 appends optional Stable Diffusion
generation metadata. A 1.0 or 1.1 document decodes without that optional value
and re-encodes byte-identically unless a caller explicitly attaches metadata.
Version 1.3 appends one optional inclusive existence range to each layer
record. A 1.0, 1.1, or 1.2 layer decodes with no range and re-encodes
byte-identically; adding a range requires upgrading the document to 1.3.
`DocumentEditor` upgrades edited documents atomically to the current minor.
Future minor or major
support requires an explicit decoder and canonical re-encoder plus
rendered-frame equivalence tests.

Moving keyframe ownership from each in-memory layer source to
`Document::frames` does not change the version-1 wire layout or increment the
minor version. The layer-major record and frame-major aggregate are lossless
transposes of the same `(layerId, frame, assetId)` relation.

The public C++ aggregate change that introduced frame-owned keys was shipped as
package 0.2.0 with SOVERSION 0.2 and requires a consumer rebuild from 0.1.x.
Adding `LayerProperties::frameRange` changes that aggregate layout again, so it
was shipped as package 0.3.0 with SOVERSION 0.3 and requires a consumer rebuild
from 0.2.x.
File-bound editing adds the separate working-file owner in package 0.4.0 with
SOVERSION 0.4; rebuild consumers against that package. It does not change these
canonical snapshot bytes.
Package ABI versioning is separate from this file format: `.iisc` is now 1.3,
while canonical 1.0, 1.1, and 1.2 fixtures continue to re-encode
byte-identically.

Media interchange in package 0.5.0 does not change the snapshot wire format or
working-file schema. Foreign images/videos become committed ARGB raster assets
and frame-owned keys; SVG becomes native path commands. Original codec bytes,
SVG markup, packet timestamps, audio tracks and decoder state are not embedded
in `.iisc`. SVG/PDF/image/video exports are explicitly separate interchange
operations; they never replace write-through editing of the working file.

Layer-preserving import in package 0.6.0 also leaves snapshot version 1.3 and
working-file schema 1 unchanged. Supported OpenRaster and PSD pixel layers
become ordinary static `BitmapLayer` / `RasterAsset` pairs. Layer properties
and committed pixels use the same native records as a newly authored canvas;
foreign ZIP/PSD payloads, editor-specific objects and unsupported drawing
semantics are not embedded or silently replaced with previews. The
`iisc-import` utility creates new working files through `DocumentFile::create`.

PSD export in package 0.7.0 also leaves snapshot version 1.3 and working-file
schema 1 unchanged. It projects `Document::timeline` at frame zero. Embedded PDF
Smart Object payloads exist only in the exported PSD, never in native assets.
Native paths, future keyframes, layer lifetimes and canvas origins remain in the
authoritative `.iisc`; PSD is an interchange snapshot, not a native round-trip
container. The separate `TimelineProject` audio/video model is not exported.

Timeline interchange in package 0.8.0 also leaves native formats unchanged.
Its version-1 `manifest.json` records exact frame intervals, rational frame
rate, layer IDs, asset IDs, generated media paths and projection warnings.
Legacy XML and FCPXML reference independent PNG layer states; `source.iisc`
retains the complete canonical native document. This is an outbound editor
exchange, not an XML-to-native round-trip or TimelineProject serialization.
