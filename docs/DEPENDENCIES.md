# Dependency review

## Media interchange (2026-09-03)

- Existing Qt Gui/Core supplies image codecs, XML tokenization, path geometry,
  color conversion, PDF drawing and process I/O. Its maintained
  [image plugin API](https://doc.qt.io/qt-6/qtimageformats-index.html) discovers
  optional TIFF/WebP/HEIC/JP2 support at runtime. No second image framework is
  linked or vendored. Optional plugins retain their Qt and third-party licenses;
  applications must deploy only maintained plugins and their notices.
- zlib is the small additional link dependency for standards-compatible SVGZ
  compression and PNG chunk CRC validation, using the platform/package
  `ZLIB::ZLIB` target. The upstream
  [project](https://zlib.net/) is maintained and uses the permissive zlib license.
  It supplies DEFLATE/gzip and checksums; no handwritten compression is added.
  The installed platform library is reused, with no bundled fork or service.
- FFmpeg/ffprobe provide video and extended bitmap codecs as optional,
  application-selected **runtime executables**,
  not CMake link dependencies. Upstream publishes maintained
  [releases](https://ffmpeg.org/download.html) and stable
  [CLI documentation](https://ffmpeg.org/ffmpeg.html). Executables are invoked
  with argument arrays, bounded pipes, deadlines and no network protocols.
  No binary download, install, or redistribution happens in this repository.
  The inspected host's FFmpeg 9.0.1 prefix is 52 MB, excluding its dependent
  libraries; shipping codecs is materially larger than this adapter.
  [Licensing depends on configuration](https://ffmpeg.org/legal.html): the
  base is LGPL-2.1-or-later, optional components can require GPL, and nonfree
  builds have redistribution restrictions. The host test build enables GPL
  and version 3. Execution is not evidence that a consumer's shipping bundle
  satisfies distribution or codec patent obligations.
- Editable SVG is a canvas-domain mapping over Qt's XML and path primitives.
  Qt SVG renders but does not expose an editable scene. NanoSVG was reviewed
  ([upstream](https://github.com/memononen/nanosvg)); its deliberately limited,
  permissive parser silently drops unsupported constructs and normalizes paths
  to cubic curves. That conflicts with this library's fail-closed import and
  linear/quadratic identity contract. No new SVG parser dependency is adopted.
  The limited supported SVG vocabulary is documented and tested explicitly.

Qt PDF and SVG rasterizers are optional deployed image plugins, not additional
linked Qt modules. Raster import advertises neither editable PDF objects nor
complete SVG feature fidelity. PSD decoding is only the flattened composite,
not editable Photoshop layers. EPS/AI and proprietary layered document formats
are not advertised as editable imports without actual decoders.

## SQLite for write-through document files (2026-09-03)

SQLite is the additional direct dependency for `DocumentFile`. iiPaintEngine
remains the sole painting dependency; it never depends on iiSharedCanvas.
The SQLite C API is private to the implementation, with no SQL or SQLite types
in public headers. CMake uses the platform/package-provided `SQLite::SQLite3`
target (named `SQLite3::SQLite3` in newer CMake). No server, service, Qt SQL
plugin, download, or vendored fork is added.
On macOS, discovery locally prefers SDK/library headers over unrelated
framework-embedded SQLite copies. It does not change the consumer's framework
search policy outside SQLite discovery. Use a fresh CMake configuration when
replacing an older cached header/library selection.

- Maintenance: the upstream [release history](https://www.sqlite.org/changes.html)
  shows continuing maintenance. Prefer a maintained vendor runtime. The 3.26
  minimum is an API/header compatibility floor, not a recommended runtime pin.
- License: upstream dedicates the delivered library to the
  [public domain](https://www.sqlite.org/copyright.html), including commercial
  redistribution. iiSharedCanvas's own AGPL-3.0-only license is unchanged.
- Size: upstream's dated [footprint examples](https://www.sqlite.org/footprint.html)
  are below 1 MB; actual platform builds and optional features vary. This build
  links the existing native library and does not enable extensions or ship a
  second copy. Cross-compiled consumers must provide their target's SQLite.
- Scope: SQLite supplies locking, page writes, rollback, and durable commit.
  iiSharedCanvas supplies only its document record mapping and edit boundary.
  Reimplementing an atomic storage engine or repeatedly replacing an entire
  serialized canvas would add avoidable maintenance or write amplification.

Working files use DELETE rollback journaling, `synchronous=EXTRA`, and
`fullfsync=ON`. Unlike deferred checkpointing, completed changes are in the main
file before an edit returns. A transient `-journal` file is required for crash
recovery and must not be removed while a file is in use. Guarantees depend on
the underlying filesystem honoring locks and synchronization; see SQLite's
[atomic commit assumptions](https://www.sqlite.org/atomiccommit.html) and
[synchronous modes](https://www.sqlite.org/pragma.html#pragma_synchronous).

The incremental [BLOB API](https://www.sqlite.org/c3ref/blob_write.html) writes
changed byte spans in an equal-sized record. SQLite still journals/writes
physical pages; a four-byte logical pixel edit is not a claim of four bytes of
physical device I/O.
