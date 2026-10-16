# Document previews

`Render/DocumentPreview.h` provides a reusable, Qt-free public API for document
thumbnails. The existing renderer and file adapters remain inside iiSharedCanvas;
consumers do not decode IISC records or choose a representative source layer.

```cpp
#include <iiSharedCanvas/Render/DocumentPreview.h>

auto preview = iiSharedCanvas::readIiscPreview(path, {{512, 512}, 0});
if (preview.ok()) {
    // preview.pixels: bounded straight-alpha ARGB pixels, owned by the result.
    // preview.region: the original document view region in document coordinates.
}
```

`renderDocumentPreview(document, options)` renders a detached document.
`readIiscPreview(path, options, serializationLimits)` reads either an IISC binary
snapshot or a SQLite working file and renders the requested frame (zero by
default). Paths are UTF-8 local filesystem paths, not URLs. Supported legacy
model versions use the existing decoder compatibility contract.

The output fits `maximumExtent` without changing the aspect ratio or enlarging
small documents. Both bounds must be between 1 and 4096 pixels; the default is
512 by 512. Reductions use smooth raster sampling; a native-size projection uses
the editor's nearest pixel sampling so painted masks retain their exact coverage.
Both paths use existing layer composition,
visibility, masks, transforms and artboard backgrounds. The source region is
`documentViewRegion`, including allocated infinite-canvas coordinates and all
artboard bounds. This API returns actual pixels with alpha and adds no UI
checkerboard, placeholder, filename or overlay.

These synchronous calls belong on an application worker. Serialization limits
bound encoded input and decoded document resources; the thumbnail bound limits
the output allocation, not the size of source assets that must be decoded.
Results use `MediaIoResult` for invalid arguments/data, unsupported files,
resource limits and I/O errors. Failure returns empty pixels and a diagnostic.

Reading a working file takes a consistent read-only SQLite backup, including
committed WAL transactions, into a private temporary directory. Only the private
copy is opened for authoring-compatible decoding. The source is never migrated,
checkpointed, edited or given a persisted preview record. A read-only source or
source directory is supported. There is no IISC schema/format change and existing
files need no resave. Applications own display caching and cache invalidation;
include the main file and WAL identity or committed revision in the cache key.

`iiSharedCanvas.DocumentPreview` and the standalone installed-package
`iiSharedCanvas.InstalledDocumentPreview` cover aspect/size/alpha, frame selection,
offset artboards/infinite canvases, legacy/current snapshots, live committed WAL,
read-only inputs, source preservation, corrupt/missing input and resource limits.
