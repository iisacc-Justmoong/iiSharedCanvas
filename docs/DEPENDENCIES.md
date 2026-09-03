# Dependency review

## PSD export (2026-09-03)

The frame-zero writer reuses the already linked
[Qt PDF paint device](https://doc.qt.io/qt-6/qpdfwriter.html) for the embedded
vector source, and the native renderer for cached layer/composite pixels.
No PDF serializer, vector rasterizer, JavaScript/Python runtime, or additional
link dependency is introduced. Qt's existing licensing and deployment boundary
is unchanged. PSD structures follow the
[Adobe specification](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/)
and are independently checked against maintained implementations and fixtures.

The external-writer review considered
[ag-psd](https://github.com/Agamnentzar/ag-psd), a maintained MIT JavaScript
reader/writer. Its core runtime dependencies are base64-js and pako, but using it
here would additionally require a JavaScript runtime/bridge and still require
the native canvas-to-cache mapping. It does not render layer/composite changes
itself. Its placed/linked-layer implementation provides a useful compatibility
reference, not a new shipped dependency. The previously reviewed BSD-3-Clause
[PhotoshopAPI](https://github.com/EmilDohne/PhotoshopAPI) has a much larger C++20
dependency graph and an unsupported/warning path for vector placed-layer data;
adopting it would not remove the specific PDF Smart Object mapping required
here. The existing psd_sdk reader limitations remain relevant to import, not a
claim that export uses that SDK. Its
[public export API](https://github.com/MolecularMatters/psd_sdk/blob/master/src/Psd/PsdExport.h)
exposes planar pixel layers and metadata/composites, but no embedded Smart
Object authoring surface. The native canvas-to-PDF/PSD mapping therefore remains
a small project-owned domain adapter over existing codecs.

Python [psd-tools](https://github.com/psd-tools/psd-tools) is used only as an
optional development oracle to recognize exported Smart Objects, resolve their
embedded PDF payloads/UUIDs and inspect first-frame pixels. Its MIT package and
Python/Pillow/NumPy/attrs dependencies are not linked, installed by `install.sh`,
or needed by the library, command-line tools, ordinary CTest or consumers.
The optional oracle also uses [pypdf](https://github.com/py-pdf/pypdf), a
maintained BSD-3-Clause, pure-Python PDF reader, to check actual path operators
and absence of raster Image XObjects. Its core requires no extra packages on
Python 3.11+; cryptography/image/font extras are unused. The inspected bundled
copy occupies approximately 3.3 MB. Both development parsers are outside the
shipping runtime dependency graph.

The export CLI reuses SQLite's
[online backup API](https://www.sqlite.org/backup.html) to obtain a consistent
private snapshot from a read-only working-file connection, including committed
WAL content. Only the temporary copy is opened by the authoring `DocumentFile`
owner. This avoids changing the source database's journal mode and reuses the
reviewed platform SQLite dependency; no shell database tool is launched.

## Layer-preserving document import (2026-09-03)

- [libzip](https://libzip.org/) supplies the OpenRaster ZIP reader through the
  private CMake target `libzip::zip`. It is an actively maintained, ZIP-specific
  dependency under the [BSD-3-Clause license](https://libzip.org/license/).
  The package uses `find_package(libzip 1.7.3 CONFIG REQUIRED)`; this is an API
  floor, not a recommendation to deploy an old security patch level. The
  verified host provides 1.11.4 (Homebrew revision 1), occupying approximately
  1.2 MB for the whole keg. Nothing is downloaded or vendored by this project.
- Upstream's [build documentation](https://libzip.org/guides/building/) requires
  zlib and permits optional additional codecs/cryptography. A minimal build
  needs only the already-reviewed zlib. The inspected host's dynamic library
  also links system bzip2 and package-provided xz/zstd; shipping applications
  must package their actual runtime dependency closure and third-party notices.
  The ORA adapter accepts only unencrypted stored/DEFLATE entries, never calls
  an extraction API, and independently bounds names/counts/expanded bytes. The
  installed CMake package resolves libzip for static as well as shared consumers.
- The deployed libzip 1.11.4
  [filename reader](https://github.com/nih-at/libzip/blob/v1.11.4/lib/zip_io_util.c)
  normalizes an embedded filename NUL into a space, including for its raw-name
  API. A bounded classic-ZIP envelope preflight therefore validates byte-exact
  central/local names before libzip sees them. It also rejects ZIP64 and
  multi-disk layouts and bounds the directory count. Ordinary space-containing
  names remain supported. This is a targeted workaround for an observed
  dependency behavior, not a second ZIP codec; decompression, payload parsing
  and CRC verification remain in libzip. See [OPENRASTER_IMPORT.md](OPENRASTER_IMPORT.md).
- The [OpenRaster layout](https://www.openraster.org/baseline/file-layout-spec.html)
  and [layer semantics](https://www.openraster.org/baseline/layer-stack-spec.html)
  define a reusable canvas-domain mapping over existing Qt XML/PNG primitives.
  No application-specific bridge, shell unzip, private Qt ZIP API, or second
  XML library is introduced. Isolated groups are not equivalent to independent
  native layers; unsupported compositing fails closed.
- PSD parsing follows the [Adobe file specification](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/)
  for a bounded v1 / 8-bit RGB pixel-layer subset. Existing zlib handles ZIP
  channel compression, with no new compression implementation. The zlib API
  minimum is 1.2.9 for `uncompress2`, which checks consumed compressed bytes.
  A separate
  maintained parser was reviewed first: [psd_sdk](https://github.com/MolecularMatters/psd_sdk)
  is BSD-2-Clause and has recent upstream maintenance, but its
  [memory-file reader](https://raw.githubusercontent.com/MolecularMatters/psd_sdk/master/src/Psd/PsdMemoryFile.cpp)
  guards a raw `memcpy` only with an assertion, and the
  [synchronous reader](https://raw.githubusercontent.com/MolecularMatters/psd_sdk/master/src/Psd/PsdSyncFileReader.cpp)
  does not propagate read failure. Direct adoption would not meet this API's
  recoverable malformed-input and allocation-limit contract without a maintained
  fork. The in-library mapper instead checks every bounded field and refuses
  unimplemented drawing semantics. This is a specific dependency defect review,
  not a claim that a small parser implements all Photoshop behavior.
- [PhotoshopAPI](https://github.com/EmilDohne/PhotoshopAPI) is BSD-3-Clause and
  C++20, but its dependency graph includes OpenImageIO, libdeflate, Eigen, fmt,
  UUID, memory-mapping and SIMD/string helpers. Python
  [psd-tools](https://github.com/psd-tools/psd-tools) is MIT and maintained, but
  introduces Python, Pillow, NumPy and attrs (plus additional optional
  compositing packages). Neither surface is justified for the defined native
  pixel-layer subset, so neither is linked, launched or downloaded by the
  product. Optional development-only PSD export verification is described above.

The canvas model and `.iisc` format are unchanged. Layered readers produce
detached native values, and the converter uses the existing `DocumentFile`
creation boundary. The new module does not depend on any consumer product.

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
complete SVG feature fidelity. `importBitmap` decodes only the PSD flattened
composite; the separate `importLayeredDocument` reader supports the explicitly
documented PSD raster-layer subset. EPS/AI, XCF, KRA, PSB and other unsupported
layered formats are not advertised as editable imports.

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

## Layered timeline interchange (2026-09-03)

The timeline package uses existing Qt Core XML/URL/JSON primitives and the
reviewed PNG writer. No new linked library, Python runtime, NLE plug-in,
online conversion service, or executable is required at export time.

- [OpenTimelineIO](https://github.com/AcademySoftwareFoundation/OpenTimelineIO)
  was reviewed before implementing the adapters. Its C++ timeline core and
  Python bindings are maintained under
  [Apache-2.0](https://github.com/AcademySoftwareFoundation/OpenTimelineIO/blob/main/LICENSE.txt).
  The inspected [PyPI release](https://pypi.org/project/opentimelineio/0.18.1/)
  is 0.18.1. Its macOS arm64 CPython 3.12 wheel is approximately 1.24 MB
  compressed, and its source archive is approximately 2.92 MB; these figures
  exclude the Python runtime and are not installed-footprint measurements.
  Upstream's [native build dependencies](https://github.com/AcademySoftwareFoundation/OpenTimelineIO/blob/main/src/deps/CMakeLists.txt)
  include Imath, RapidJSON and minizip-ng; Python bindings additionally use
  pybind11. Reusing the core would also introduce a second persisted editorial
  model beside this library's integer-frame, rational-rate canvas model.
- OTIO's interchange [adapters are Python plug-ins](https://github.com/AcademySoftwareFoundation/OpenTimelineIO/blob/main/docs/tutorials/adapters.md),
  not a C++ FCPXML writer API. The separately distributed Apache-2.0
  [FCP 7 adapter](https://github.com/OpenTimelineIO/otio-fcp-adapter) and
  [FCP X adapter](https://github.com/OpenTimelineIO/otio-fcpx-xml-adapter)
  support multiple tracks, gaps and nesting, but both explicitly mark
  audio/video effects unsupported. Their generic model mapping therefore
  does not implement this contract's editable layer opacity and compositing.
  Their 1.0.0 Python wheels are approximately 28 KB and 19 KB compressed,
  respectively, in addition to OTIO. The inspected FCP 7 repository has 2026
  maintenance; the FCP X repository's latest push is June 2024. In particular,
  [OTIO-Plugins 0.18.0 removed FCP X from its batteries-included set](https://github.com/OpenTimelineIO/OpenTimelineIO-Plugins/releases/tag/v0.18.0).
  The current metapackage installs eight other adapters and does not remove
  the need to select, maintain and test FCP X separately.
- Direct XML generation is limited to the already-defined canvas-domain
  mapping: layer order, static/hold-keyframed exposure ranges, canvas-sized
  PNG media, visibility, opacity and the documented blend subset. Qt handles
  XML escaping and file URL encoding. This is not a replacement general
  editorial timeline engine or an unbounded XML parser. OTIO remains a
  reasonable optional development oracle or future dependency for broader
  editorial interchange, but is not installed or launched automatically.
  The package's native `source.iisc` backup uses the existing serializer.
  Before validation or serialization creates source-sized temporary data,
  an allocation-free preflight conservatively charges all source assets,
  raster chunks, vector commands, names, owned keys and nested generation
  metadata against the snapshot memory allowance, including unused content.
  It does not rely on compression or a post-encoding file-size check to enforce
  that allowance; callers can explicitly raise the limit for larger documents.
- Two format adapters are necessary. Adobe's current
  [Premiere import documentation](https://helpx.adobe.com/premiere/desktop/organize-media/import-files/migrate-from-final-cut-pro-x.html)
  explicitly excludes direct `.fcpxml` import; legacy Final Cut XML and
  modern FCPXML must not be treated as interchangeable. Legacy output follows
  Apple's [Final Cut XML reference](https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/FinalCutPro_XML/Elements/Elements.html).
  Final Cut output follows Apple's
  [FCPXML document model](https://developer.apple.com/documentation/professional-video-applications/creating-fcpxml-documents)
  and versioned DTD. Apple DTDs are validation references, not a new runtime
  dependency or a vendored component. The installed Final Cut Pro application
  provides `FCPXMLv1_9.dtd` for optional independent `xmllint` validation.
  Schema validity alone does not prove target-app import, still-media timing,
  blend rendering, color-management equivalence or an app-specific track UI.

No OTIO code, adapter code, Apple DTD, or NLE binary is copied into the
distributed library. Existing Qt and PNG deployment/license obligations are
unchanged. The `.iisc` document remains authoritative; interchange packages
preserve editorial layers and exposure timing without claiming native vector
path editing in the receiving NLE.
