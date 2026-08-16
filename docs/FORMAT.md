# iiSharedCanvas `.iisc` format version 1

Status: Implemented canonical binary contract.

## Identity

- Extension: `.iisc`
- Media type: `application/vnd.iisacc.ii-shared-canvas`
- Model version: major 1, minor 0
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
u32 frameRateNumerator
u32 frameRateDenominator
u32 frameCount

u32 assetCount
Asset assets[assetCount]

u32 layerCount
Layer layers[layerCount]
~~~

Assets and layers preserve their document vector order. Layers are ordered
bottom-to-top.

## Assets

Every asset starts with:

~~~text
u8 kind       // 0 raster, 1 vector
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

Source kind 0 is static and stores one asset-id string. Source kind 1 is
keyframed and stores:

~~~text
u8 contentKind       // 0 raster, 1 vector
u32 keyframeCount
repeat keyframeCount:
  u32 frame
  string assetId
~~~

Version 1 uses hold sampling only. The selected asset is the last keyframe at
or before the requested frame. The first keyframe is zero, positions are
strictly increasing, frames remain within `[0, frameCount)`, and one track never
mixes raster and vector assets.

## Render contract

Raster and vector assets use nearest-neighbor affine sampling and clip to the
document canvas. A singular matrix produces an empty layer footprint. Vector
fills use the even-odd rule; strokes use round footprints. M/L/Q/C/Z paths use
deterministic CPU rasterization with a 4x4 coverage grid per pixel. Resolved
layers are composited bottom-to-top using iiPaintEngine opacity and blend
semantics.

## Authoring-only state

Brush input is not a persisted content kind. `BitmapEditor` may retain a
transient iiPaintEngine dab stream only while a pointer gesture is active.
`BitmapItem` and `CanvasItem` viewport state, layer selection, undo history,
input events, and UI tool state are not encoded.

## Default reader limits

`SerializationLimits` is caller-configurable. Defaults are:

| Resource | Default maximum |
| --- | ---: |
| Container bytes | 1 GiB |
| Canvas or vector viewport pixels | 256 Mi pixels |
| Total raster pixels | 256 Mi pixels |
| Assets | 65,536 |
| Layers | 65,536 |
| One string | 1 MiB |
| Total string bytes | 64 MiB |
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

Version 1.0 is the first physical format, so no earlier physical version needs
migration. Future minor or major support requires an explicit decoder and
canonical re-encoder plus rendered-frame equivalence tests.
