# OpenRaster layered import

`decodeLayeredDocument` and `importLayeredDocument` recognize OpenRaster (`.ora`)
by ZIP content plus its `image/openraster` mimetype. The importer returns a detached,
validated `Document`; it neither extracts archive members nor reads external
resources, invokes a process, or writes a working document.

## Editable mapping

The supported profile is OpenRaster versions 0.0.1 through 0.0.6 with PNG raster
layers. Each source becomes an independent `RasterAsset` and `BitmapLayer`, even
when multiple layers reference one PNG. Names, visibility, opacity, signed integer
offsets, and native PNG pixels are retained. Layer order reverses from OpenRaster
top-first to iiSharedCanvas bottom-first. IDs are deterministic:
`<idPrefix>-asset-0` / `<idPrefix>-layer-0` identify the bottom layer.

The supported composite operations map exactly:

| OpenRaster | iiSharedCanvas |
| --- | --- |
| `svg:src-over` | `SourceOver` |
| `svg:multiply` | `Multiply` |
| `svg:screen` | `Screen` |
| `svg:overlay` | `Overlay` |

Other blends, masks, filters, text, vector layer sources, unknown drawing
elements/attributes, and isolated or non-neutral groups fail closed. The importer
never substitutes `mergedimage.png` for the requested editable layers.
`svg:dst-out` is also rejected: `DestinationOut` is a bitmap brush blend enum,
but the native canvas document deliberately does not permit it as a layer blend.

A non-root group may be flattened only when it explicitly declares
`isolation="auto"`, is visible and fully opaque, and uses `svg:src-over` (or its
default). This safe pass-through conversion emits a hierarchy/name-loss warning.
Groups are isolated by default in OpenRaster, so omission of `isolation` is not
treated as pass-through. Legacy group `x`/`y` attributes in versions before 0.0.6
are ignored with a warning, as the specification requires: they never add to child
offsets. Version 0.0.6 group coordinates are rejected.

The root stack always has the specification's fixed isolated rendering. Although
the current specification tells writers to omit its attributes, the importer
accepts redundant root `name`, `opacity`, `visibility`, `composite-op`, and
`isolation` attributes from existing writers, ignores them, and reports this
compatibility handling as a warning. Unknown root attributes still fail closed.

Image title, physical print resolution, and layer PNG text metadata have no native
document field and produce warnings when present. Layer names are retained.
Missing merged preview or thumbnail also produces a warning, since neither is
used for editable conversion. Native PNG color/depth conversion warnings propagate
from the existing bounded bitmap decoder.

## Untrusted-input contract

Reviewed libzip performs ZIP parsing, strict consistency checks, decompression,
and CRC verification. A bounded classic-ZIP end-record/directory/local-name
preflight additionally rejects embedded NULs before libzip 1.11.4 can normalize
them into spaces. It checks raw UTF-8 names, local/central byte-exact agreement,
directory bounds and the entry budget before the dependency allocates its
directory. ZIP64 and split/multi-disk archives are explicitly unsupported; no
extra-field or compressed-payload parser is reimplemented. Valid spaces in
archive names remain supported.

Every entry is read through to EOF for checksum validation,
including entries not referenced by the layer stack. Only `STORED` and `DEFLATED`
members are accepted. Both the first physical local file and the first indexed
entry must be the uncompressed `mimetype`; a fixed envelope check prevents central
directory reordering from bypassing the first-file requirement.
Encrypted entries, duplicate names, invalid UTF-8 names, unsafe relative paths,
symlinks, special files, malformed XML, DTDs, entities, processing instructions,
unknown versions, missing image references, and unsupported rendering semantics
are rejected before any partial document is returned.

`maxInputBytes` bounds the archive; `maxArchiveEntries` bounds its directory;
`maxXmlDepth` bounds XML nesting; `maxLayers` bounds editable layers;
`maxPixelsPerFrame` bounds both canvas and individual PNG extents.
`maxDecodedBytes` conservatively bounds the sum of all declared expanded archive
bytes plus the cumulative retained raster pixels, including independently copied
assets for repeated PNG references. PNG integrity and size are checked through
`decodeBitmap` with extended codecs disabled. No archive path is resolved against
the filesystem. Failure always returns an empty document.

## Verification

`OpenRasterImportTest` generates archives in memory with the reviewed libzip
writer and PNGs with the public native bitmap codec. It verifies exact golden
rendered pixels, native `.iisc` encode/decode preservation, names/order/offsets/
opacity/visibility/blends, explicit pass-through group handling, and rejection of
corrupt or unsupported archives and resource-limit violations.

Specification references:

- [OpenRaster layer stack specification](https://www.openraster.org/baseline/layer-stack-spec.html)
- [OpenRaster file layout specification](https://www.openraster.org/baseline/file-layout-spec.html)
- [libzip documentation](https://libzip.org/documentation/)

Dependency maintenance, license, and footprint review is recorded in
`DEPENDENCIES.md`. This reader implements an explicitly bounded subset, not full
OpenRaster baseline editing or arbitrary layered-format compatibility.
