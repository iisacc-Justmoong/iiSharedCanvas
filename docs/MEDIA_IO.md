# Media interchange

The media adapters import committed pixels or native paths; they never put a
foreign file, pointer trajectory, or video decoder into a document layer.
They are independent of the write-through `DocumentFile` owner. To insert an
import into an open working file, add its assets/layers/keys inside one
`DocumentFile::edit` transaction. Imports themselves do not mutate a document.
Explicit interchange exports do not replace manual-save-free working-file I/O.

## Bitmap

`bitmapFormats()` reports actual Qt plugins and optional FFmpeg extended image
codecs, independently for each direction. `decodeBitmap`/`encodeBitmap` accept bytes;
`importBitmap`/`exportBitmap` use local files; `exportBitmapFrame` composes one
canvas frame. PNG, JPEG, BMP, portable bitmap formats, icons, TIFF, WebP, HEIC
and JPEG 2000 depend on the deployed Qt build. The optional extended adapter
adds TGA, QOI, OpenEXR, DPX, Radiance HDR, PCX and SGI read/write plus PSD and DDS
readers, only when those codecs are present. PSD imports its composite image,
not Photoshop layers. DDS imports one surface, not a texture/cubemap asset.
`extendedCodecs = false` disables this backend; `bitmapFormats(backend, false)`
queries only Qt. Neither Camera RAW decoding nor PSD export is claimed.
A name is not a guarantee for
every subtype, page layout, color model, or encoder option.

Pixels become straight 8-bit ARGB in sRGB. On the Qt path, tagged profiles are converted,
EXIF orientation is applied by default, and bit-depth/color conversions are
reported. Untagged input is interpreted as sRGB. High-bit-depth/HDR/CMYK authoring
data is not retained. Extended codecs do not apply source ICC profiles; they
report the loss of metadata, color-profile fidelity and auxiliary channels.
EXR/HDR use scene-linear RGB with sRGB primaries, convert to display sRGB, and
clamp to SDR without HDR tone mapping. Exporting them expands the 8-bit canvas
to linear floating-point storage; it cannot recover original HDR values.
Their 16-bit-per-channel RGBA intermediate buffers also obey `maxDecodedBytes`.
Opaque outputs use an explicit matte and report alpha
loss. PNG text carriers are available as UTF-8 key/value data, including
generation-parameters text; other metadata is not claimed as preserved.
`imageIndex` selects a page/frame; animation belongs to `importVideo`.
An optional `format` hint supports formats without a reliable content signature
(such as old TGA). File import tries content first, then a known suffix only if
no bitmap reader was identified. JPEG 2000 readers without a pre-decode size
query use validated JP2 header/codestream dimensions for the allocation check.
PNG chunk framing and CRCs are checked before decoding, including a complete
IEND footer; lenient repair of truncated PNG is not accepted. This is not a
claim that every other third-party decoder rejects every damaged input.
PNG values retain whitespace and UTF-8; PNG keywords follow the standard's
1..79-byte printable Latin-1 restrictions. No EXIF/XMP/IPTC preservation is
claimed beyond reader-applied orientation and color conversion.
Export `format` defaults to PNG and is not inferred from the destination suffix.
`quality` is -1 (backend default) or 0..100 for Qt encoders; the extended codecs
use their explicit fixed pixel format and codec defaults, not this quality knob.

## Vector

`decodeSvg`/`importSvg` read SVG and gzip SVGZ. Supported solid paths and shapes
map to editable M/L/Q/C/Z commands. Unsupported drawing features fail the whole
import, never silently return a partial picture. XML entities, external
resources, scripts, and unbounded nesting are rejected.

The editable subset includes absolute/relative M/L/H/V/Q/T/C/S/A/Z path syntax,
rectangles (including rounded corners), circles, ellipses, lines, polylines and
polygons. Groups may apply affine matrix/translate/scale/rotate/skew transforms,
inherited solid paints and inline presentation styles. Viewports support
viewBox, `preserveAspectRatio` meet alignments or none, and px/in/cm/mm/pt/pc
lengths; percentages require a known viewport. Elliptic arcs become cubic
Beziers with a warning. Nonzero fills are normalized to the native even-odd
model, and unsupported stroke caps/joins/dashes or nonuniform stroke transforms
become editable filled outlines. Those conversions can flatten curves and do
not preserve source path identity. Native round strokes and even-odd paths
retain linear/quadratic/cubic commands directly.

Gradients, text, CSS classes/stylesheets, definitions/use references, clipping,
masks, filters, nested viewports and nontrivial group opacity are not silently
approximated by the editable reader. `title`, `desc`, authoring attributes and
metadata are not retained as canvas objects. Use the explicit raster fallback
when retaining editability is unnecessary; its fidelity depends on Qt's plugin.

`encodeSvg`/`exportSvg` emit native editable SVG or SVGZ. `exportPdf` emits
vector paths and bitmap content as separate drawing operations, with optional
multiple pages selected by inclusive frame range. PDF is not the working file.
It uses 96 canvas pixels per inch. Isolated vector-layer opacity is rasterized
per layer with a warning; unsupported blend modes reject export unless
`rasterizeUnsupportedBlending` explicitly permits full-frame rasterization.
`rasterizeVectorFile` is a separate, explicitly lossy bitmap import using an
available Qt SVG/PDF image plugin at caller-specified pixel dimensions.
Editable PDF/EPS/AI import is not provided. SVG export takes one `VectorAsset`;
mixed-canvas export is available through PDF, bitmap frames or video.

## Video

`videoCapabilities` queries the configured FFmpeg runtime, not a hard-coded
promise that every host ships every codec. `probeVideo` inspects local media.
`importVideo` decodes into a bitmap layer with frame-owned keys, normalizing
timestamps to a rational constant frame rate. `firstFrame`/`frameCount` select
the sampled range and rebase it to zero. Audio and subtitle tracks are not
canvas pixels and are not imported; audio presence is reported.

`exportVideo` renders a selected inclusive canvas range in order and streams
RGBA frames to FFmpeg, without dumping a raw-frame directory. Default output
is lossless FFV1/BGRA in Matroska. Container, encoder and pixel format are
explicit strings; file extensions do not choose them. MP4/H.264 and HEVC,
WebM/VP9, MOV/QuickTime Animation and ProRes, AVI/FFV1,
GIF and APNG are interoperability test profiles when those encoders exist.
H.264/YUV420 and similar profiles require encoder-compatible dimensions
(commonly even width/height). Unsupported combinations fail; no implicit codec
or pixel-format substitution is performed. Opaque pixel formats use the
configured matte. GIF reports palette/time quantization, and lossy video
formats report color/precision reduction. Audio is not exported.
This is canvas-animation interchange, not a `TimelineProject` audio mixer,
sequence renderer, or project serializer.

## Safety and execution

All paths are local, all process arguments are passed without a shell, and
media inputs cannot request network protocols. Exports refuse collisions unless
`overwrite` is explicitly enabled, and always refuse working `.iisc` files.
Failed exports do not publish partial destinations. Limits bound encoded bytes,
pixel count, total decoded frame storage, vector commands and XML depth.
FFmpeg calls have timeouts and cooperative cancellation. Limits are not an OS
sandbox for third-party codecs: use maintained codec builds for untrusted data.
In particular, extended image probing can decode one frame to obtain its size;
limits bound returned data and adapter buffers, not peak codec-process memory.
`timeoutMs` applies to each subprocess, not the complete multi-process operation.
Calls are synchronous, reentrant for independent inputs, and may run on a
consumer-owned worker; a file-bound edit still runs on its owner's thread.
Initialize Qt (a `QGuiApplication` for the full PDF/image feature set) before
using adapters. Optional backends are never downloaded automatically.

## Usage

Import first, then insert into the working file's validated transaction. Asset
and layer ids are caller-owned and must be unique in the destination document:

```cpp
BitmapImportOptions input;
input.assetId = "photo-1";
auto photo = importBitmap("/art/photo.tiff", input);
if (!photo.ok()) { /* surface photo.result and stop */ }
else {
    auto changed = file.edit([&](Document &draft) {
        draft.assets.emplace_back(photo.asset);
        draft.layers.emplace_back(BitmapLayer{
            {"photo-layer-1", "Photo"}, StaticSource{photo.asset.id}});
        return true;
    });
    // Check changed.ok(); success is already written, with no save call.
}
```

An explicit movie profile keeps container, codec and alpha policy visible:

```cpp
VideoExportOptions movie;
movie.container = "mp4";
movie.codec = "libx264";
movie.pixelFormat = "yuv420p";
movie.matteArgb = 0xff000000U;
auto exported = exportVideo(document, "/art/preview.mp4", movie);
// Check exported.ok() and surface exported.warnings.
```

`BitmapCodecTest`, `VectorCodecTest`, `VideoCodecTest` and the installed-package
consumer exercise real bytes/files and independently constructed fixtures.
The validation host uses Qt 6.8.3 and FFmpeg 9.0.1; capability-gated tests do not
establish that the same optional codecs are present on a consumer's machine.

See [DEPENDENCIES.md](DEPENDENCIES.md) for runtime packaging and licenses.
