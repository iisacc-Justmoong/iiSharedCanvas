# Native canvas to layered PSD command line

The installed `iisc-export-psd` utility exports frame zero of a native canvas as
independent PSD layers using `exportPsd`. It accepts both canonical binary `.iisc`
snapshots and SQLite `.iisc` working files, detected from content rather than the
source filename. Raster and vector layers use the PSD writer's documented
bounded conversion rules; vector/timeline conversion warnings are printed to
standard error rather than hidden.

```sh
iisc-export-psd drawing.iisc drawing.psd
iisc-export-psd --overwrite "drawing with spaces.iisc" "delivery with spaces.psd"
iisc-export-psd -- -input.iisc -output.psd
iisc-export-psd --help
```

Only `--overwrite` and the option terminator `--` are supported. `--help` and
`-h` are accepted on their own. Duplicate options, unknown switches, extra or
missing paths, and non-`.psd` output extensions are usage errors. Unicode and
space-containing local paths are supported. URLs, shell commands, and Qt-specific
command-line switches are not interpreted. In the absence of an explicitly
configured Qt platform the utility uses the offscreen platform.

Exit status is `0` for successful export/help, `1` for input/conversion/I/O errors,
and `2` for usage errors. Successful export reports the destination on standard
output. Failures and conversion warnings use standard error.

## Source and destination protection

PSD and timeline export share the private `tools/IiscInput` loader, whose
input contract uses media limits and a maximum layer count without coupling to
either export format. The original source is never passed to `DocumentFile::open`: that API is an
authoring owner and configures a read/write SQLite connection. Snapshot bytes are
read and validated with `decodeIisc`. Working files instead use SQLite's existing
online backup API through a `SQLITE_OPEN_READONLY` source connection, with no URI
or immutable mode. A read transaction fixes the snapshot, including committed WAL
content. Raw filesystem copying of a live SQLite database is not used.

The backup lives in a private `.iisc-input-XXXXXX` temporary directory beside
the output. Only this consistent copy is opened with `DocumentFile` to validate
the canonical schema, record hashes, and document model. The copy and directory
are removed on both success and failure. The source database and WAL payload are
not rewritten; SQLite may update its normal shared-memory reader bookkeeping when
the source uses WAL. Export does not change the source's journal mode or force a
checkpoint. A locked source that cannot supply a read snapshot fails explicitly.

The source file plus any WAL/journal/shared-memory sidecars and the logical backup
size are bounded by the configured default media input/decoded budgets. Sidecars
must be regular local files, not symbolic links. The backup checks
`page_count * page_size` before
copying, uses bounded page batches, a 250 ms SQLite busy timeout, and a 30-second
overall copy deadline. Binary snapshots use bounded reads and serialization
limits. No source path is written and no network resource is requested.

Existing outputs are preserved by default. `--overwrite` authorizes replacing
only the destination PSD using the PSD writer's atomic publication. The source
itself, symlink destinations, and non-regular outputs remain protected. Failed
encoding never publishes a partial PSD or replaces an existing output. The
library's native-file/database replacement protection continues to apply.

## Verification

`PsdExportCliTest` launches the real executable and verifies working-file and
binary-snapshot input, read-only source permissions, Unicode/spaced/dashed paths,
separate bitmap/vector layers, explicit frame-zero sampling, default collision
handling, overwrite and failed-export atomicity, source SHA-256 preservation,
and temporary-directory cleanup. A live WAL fixture commits changed raster
records without checkpointing; exported pixels must reflect that commit while
the original database and WAL hashes remain unchanged.

This utility adds no dependency: it reuses the existing reviewed private SQLite
dependency and the public native document and PSD APIs.

Reference: [SQLite online backup API](https://sqlite.org/backup.html).
