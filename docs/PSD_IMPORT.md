# Layer-preserving PSD import

`decodeLayeredDocument(bytes, options)` and `importLayeredDocument(path, options)`
identify PSD by its `8BPS` signature and return a detached `Document`. File
extensions do not determine the parser. This path is independent of the
flattened PSD support in `decodeBitmap`: the merged image is never substituted
for editable layers, and a PSD without supported layer records is rejected.

## Supported contract

The initial reader accepts PSD version 1, 8-bit RGB, positive-size raster
layers, three RGB channels and an optional transparency channel. Header
dimensions must be within the PSD v1 range. Channel data may use raw bytes,
PackBits RLE, ZIP, or ZIP with the 8-bit horizontal predictor. ZIP is decoded
by the existing zlib dependency. Predictor state restarts on every scanline.

Each record becomes one `RasterAsset` and one static `BitmapLayer`. Asset
storage is straight ARGB, including the RGB components of fully transparent
pixels. Layer opacity is not baked into pixels. Names, visibility, opacity,
signed integer placement, and normal/multiply/screen/overlay blend modes map
to existing canvas properties. PSD binary record order is already native
bottom-to-top order and is retained. IDs are `<idPrefix>-asset-<index>` and
`<idPrefix>-layer-<index>`, with index zero at the bottom.

Unicode `luni` names are decoded from UTF-16, including surrogate pairs; a
single terminal null is permitted. Malformed surrogate pairs and embedded
nulls are rejected. In the absence of `luni`, only ASCII legacy names are
accepted: choosing an undocumented legacy code page could silently rename a
layer. Empty names remain empty. Original editor IDs and lock state are not
preserved; callers receive a warning when those supported metadata blocks
are encountered.

The default color interpretation is sRGB. A valid explicitly embedded sRGB
profile is accepted without altering channel bytes. Other profiles, invalid
profiles, intentionally untagged color data (image resource 1041 set to 1),
and non-square pixel aspect ratios are rejected. The untagged-profile flag
must contain exactly one canonical Boolean byte; setting it to 0 permits
the default sRGB interpretation, while 1 is rejected even beside an explicit
sRGB profile. Image resources
such as DPI and thumbnails are not retained and are disclosed as warnings.

## Explicitly unsupported

PSB, 16/32-bit channels, grayscale/CMYK/Lab/indexed color, document auxiliary
channels, empty/non-pixel layers, groups, clipping layers, masks, non-default
blend ranges, fill opacity below 255, adjustment/text/vector/smart-object
layers, effects, layer comps, animation metadata, global masks, and global
additional layer-information blocks do not pass this reader. A layer's
unknown additional-information key also fails closed with its four-character
key in the error message. No cached raster is used to pretend that an
unsupported editable object was converted faithfully.

Besides `luni`, the small layer-tag allowlist consists of editor-only
`lyid`, `lspf`, `lyvr`, `lclr`, and `fxrp`, plus default `iOpa`.
Consequently some otherwise simple files from specific PSD producers can be
rejected until their extra records are explicitly reviewed and tested. This
is a bounded compatibility subset, not a claim of full Photoshop fidelity.

## Validation and resource boundaries

Every parser reads a length-bounded span. Channel IDs, signatures, counts,
dimensions, record lengths and name encoding are validated before pixel
allocation. All declared channel spans are checked before constructing any
asset. A negative signed layer count requires four document channels: RGB
plus merged transparency. A fourth channel with a positive layer count is
an unsupported auxiliary alpha channel and is not silently discarded.
Encoded bytes use `maxInputBytes`; canvas and layer sizes use
`maxPixelsPerFrame`; count uses `maxLayers`. The aggregate retained ARGB
storage, largest one-channel scratch plane and UTF-8 names must fit
`maxDecodedBytes`. Names transfer into native layers without retaining a
second decoded copy. The limit is a decoded-content budget, not a process RSS
ceiling: allocator overhead, small record objects and zlib state are separate.

RLE runs cannot cross a row boundary, and each row must produce exactly its
width. ZIP must end normally, consume its entire declared channel span, and
produce exactly the declared plane size. The optional merged preview is not
decoded into an asset; when present, raw/RLE/ZIP framing and decoded length
are validated with constant scratch storage. A producer may omit the merged
preview entirely when maximize compatibility is disabled.

Failure returns no assets or layers. Successful values must pass the same
public document validation as native canvas documents. Import never writes
the input file, extracts resources, starts an external decoder, or mutates a
working document. Persist through the normal native serializer or validated
`DocumentFile` workflow after import.

`PsdLayeredImportTest` constructs independent binary fixtures before calling
the reader. Coverage includes all four compression modes, transparent RGB,
surrogate-pair names, layer order and properties, negative offsets, supported
blend modes, native `.iisc` round-trip, a rendered golden frame that differs
from the embedded preview, fail-closed unsupported semantics, truncated
records, resource bounds, merged-versus-auxiliary alpha semantics,
intentionally untagged color rejection and content-based file-path imports.

## Dependency review (2026-09-03)

The [Molecular Matters PSD SDK](https://github.com/MolecularMatters/psd_sdk)
was considered first. Its [BSD-2-Clause license](https://github.com/MolecularMatters/psd_sdk/blob/master/LICENSE)
is suitable for redistribution, and its focused C++ implementation is much
smaller than a full image-editing stack. However, the current upstream
[`MemoryFile::DoRead`](https://github.com/MolecularMatters/psd_sdk/blob/master/src/Psd/PsdMemoryFile.cpp)
uses an assertion followed by `memcpy`, with no recoverable bounds-error
return path and unchecked addition in its bounds expression. These observed
checks do not establish the required untrusted-input, limit-aware contract.
Adoption would require a maintained hardening fork and a broader parser audit.

No SDK source is copied or vendored. The small strict mapping implemented
here follows the [Adobe PSD format specification](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/)
and reuses the already-reviewed zlib for ZIP plus Qt for ICC recognition.
There is no new PSD runtime dependency, no executable download, and no
change to public document or raster model ownership.

Binary layer ordering was also cross-checked against the primary
[psd-tools record reader](https://github.com/psd-tools/psd-tools/blob/main/src/psd_tools/psd/layer_and_mask.py)
and [tree reconstruction](https://github.com/psd-tools/psd-tools/blob/main/src/psd_tools/api/psd_image.py):
the sequential file records become the API's bottom-to-top sequence without
reversal. The order differs from OpenRaster's XML stack order.
