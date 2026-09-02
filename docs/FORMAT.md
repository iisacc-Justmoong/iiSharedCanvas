# iiSharedCanvas `.iisc` format version 1

Status: Implemented canonical binary contract.

## Identity

- Extension: `.iisc`
- Media type: `application/vnd.iisacc.ii-shared-canvas`
- Current model version: major 1, minor 2
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

Within the existing 1.2 metadata record, `automatic1111Parameters` contains the
complete infotext extracted from an AUTOMATIC1111 image carrier.
AUTOMATIC1111 infotext remains byte-exact during canonical `.iisc` round-trip; parsing creates
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
is the referenced asset kind. Source kind 1 is keyframed and stores:

~~~text
u8 contentKind       // 0 BitmapLayer, 1 VectorLayer
u32 keyframeCount
repeat keyframeCount:
  u32 frame
  string assetId
~~~

The decoder materializes every entry as a type-distinguished `BitmapLayer` or
`VectorLayer`. The in-memory `KeyframedSource` does not duplicate this type;
the encoded keyframed `contentKind` is the owning layer's canonical type tag.
Validation rejects a static or keyframed reference whose asset kind differs from
the owning layer.

Version 1 uses hold sampling only. The selected asset is the last keyframe at
or before the requested frame. The first keyframe is zero, positions are
strictly increasing, frames remain within `[0, frameCount)`, and one track never
mixes raster and vector assets.

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
string automatic1111Parameters
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

Brush input is not a persisted content kind. `BitmapEditor` may retain a
transient iiPaintEngine dab stream only while a pointer gesture is active.
`BitmapItem` and `CanvasItem` viewport state, layer selection, undo history,
input events, and UI tool state are not encoded.

`CameraRawData` is not encoded by `.iisc` version 1.1 and remains outside
version 1.2. It is decoded/import state containing sensor samples and
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
| One string | 1 MiB |
| One generation-metadata string | 16 MiB |
| Total string bytes | 64 MiB |
| Generation-metadata entries across passes, models, LoRAs, and extension lists | 65,536 |
| Vector paths | 1,048,576 |
| Path commands | 16,777,216 |
| Keyframes | 16,777,216 |

The reader checks declared counts and aggregate totals before allocating the
corresponding collections or pixel buffers. A product may lower these limits
for its device class.

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
and re-encodes byte-identically unless a caller explicitly attaches metadata;
`DocumentEditor` then upgrades it atomically to 1.2. Future minor or major
support requires an explicit decoder and canonical re-encoder plus
rendered-frame equivalence tests.
