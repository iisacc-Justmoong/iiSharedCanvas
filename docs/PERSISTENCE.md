# Write-through document files

## Authoring contract

`File/DocumentFile.h` owns a canvas's working file and committed `Document`.
Create or open the file once, then bind any of `DocumentEditor`, `VectorEditor`,
`BitmapEditor`, `ChunkedBitmapEditor`, `BitmapItem`, or `CanvasItem` to that
`DocumentFile`. Every accepted content mutation commits to the file before the
editing call returns. There is no save method, debounce timer, dirty-document
queue, full-document file replacement, or close-time dump.

```cpp
iiSharedCanvas::DocumentFile file;
auto result = file.create("/workspace/drawing.iisc", initialDocument);
if (!result.ok()) {
    reportError(result.message);
    return;
}
iiSharedCanvas::DocumentEditor structure(file);
auto renamed = structure.setLayerName("paint-layer", "Foreground");
iiSharedCanvas::BitmapEditor bitmap(file, "paint");
if (!renamed.ok() || !bitmap.setPixel(10, 20, 0xff336699U)) {
    // Report the operation's error; the previous committed state is retained.
    return;
}
// A second process can now open the file and observe both edits.
// No save(), flush(), close(), or destructor is necessary to commit them.
```

Brush begin/continue/end calls persist the pixels produced by that call,
including while the gesture is still active. A gesture remains one in-memory
undo step. Cancellation, undo, redo, patches, clears, raster replacements,
vector changes, metadata edits, layer ranges, frame-owned keys, and audio
assets/tracks/clips all use the
same transaction boundary. Only pixels and native model fields are stored,
never brush input, transient dab streams, or undo/redo history.

For application-defined operations, `file.edit([](Document &draft) { ...;
return true; })` validates and commits the complete edit atomically. This is an
editing operation, not a later request to save accumulated changes. A false
return, exception, validation error, stale writer, or failed disk transaction
rejects the draft. Nested file edits are rejected; `close()` during a callback
does not invalidate the active transaction. The draft and references into it
must not escape the callback. Local editors may be used inside the callback.

`document()` exposes only a const committed view. An editor or canvas bound to
a file returns null from its legacy **mutable** `document()` overload; use its
const overload or `file.document()` for inspection. This prevents supported
application code from mutating the live aggregate outside the write-through
boundary. Standalone `Document` aggregates and `bind(Document &)` remain
explicitly in-memory APIs for import, tests, temporary canvases, and detached
render snapshots; raw C++ field assignments cannot be intercepted. They must
not be used as the application's persistent authoring owner.
References into the committed document may be invalidated by any successful
content edit; retain stable ids, not pointers into asset/layer collections.

## Qt Quick adoption

`CanvasItem::createFile(path, width, height, frameCount)` creates and selects one
transparent raster layer in a working file. `openFile(path)` opens an existing
working file; selection is explicit after opening. `filePath` identifies the
active working file. `bind(DocumentFile &)` supports caller-owned files with
arbitrary valid raster/vector/infinite-canvas documents. `BitmapItem` accepts
the same binding plus a raster asset id. Existing pathless `createDocument`,
`createRasterDocument`, `createInfiniteRasterDocument`, and `createBitmap`
construct transient in-memory content, not unnamed persistent documents.

The application chooses the working path before authoring (for example, its
document catalog allocates one for a new canvas). No file picker or manual
save is required by the library. An externally edited file owner may need a
canvas `refresh()` for display invalidation; persistence has already completed
and never depends on refresh, render completion, or pointer release.

The file must outlive its bound editors/items. Close/reopen invalidates prior
editor bindings even if the new document has identical asset ids. All editing
is on one owning thread; Qt items stay on the GUI thread. Worker renderers use
detached immutable snapshots, never a writable file connection.

## Transactions and failure

Working files use SQLite with `journal_mode=DELETE`, `synchronous=EXTRA`, and
`fullfsync=ON`; the reviewed dependency is documented in `DEPENDENCIES.md`.
Accepted mutations are in the main file when the call returns, not in a WAL
awaiting a later checkpoint. SQLite's transient rollback journal may exist
during a transaction or after an interrupted transaction; do not delete or
separate it from its file during recovery. Close does not write pending edits.

The persistent file revision advances only after a changed transaction commits.
No-op operations write no records and do not advance it. Structural editors
return `DocumentEditCode::PersistenceFailed` on storage failure. Bitmap editors
return false and provide `lastError()`; their void `cancelStroke()` preserves
the active gesture and reports `lastError()` if cancellation cannot commit.
Failed writes preserve document state, editor revisions, dirty bounds, and undo
history. If a failed commit's disk outcome cannot be established, the file
detaches and must be reopened instead of exposing memory as confirmed storage.

An immediate transaction plus persistent revision and SQLite `data_version`
checks prevents another open session from overwriting a newer commit. A stale
session must reopen and rebind. Busy/read-lock failures return promptly and may
be retried after the lock is released. Filesystem/permission/disk errors are
never converted to memory-only success. File creation refuses existing paths;
invalid input is rejected before creating a file.

Durability relies on the filesystem and device honoring synchronization and
locking. Use a local filesystem; this is not a network collaboration protocol.
The tests cover competing connections, rollback, fail-closed corruption and
schema checks, reopening after every editing path, and process termination
without destructors. They are not hardware power-cut certification.

## Physical working-file format, schema 1

A working `.iisc` file begins with SQLite's `SQLite format 3\0` header, has
`application_id=0x49495343` and `user_version=1`, and contains exactly these two
application tables. Schema versions are independent of the canvas model's
`FormatVersion` (currently 1.4). Unknown identities/versions/schema objects,
invalid record identities/order, failed checksums, invalid references, and
configured resource-limit violations fail closed.

`canvas_state` contains one row (`singleton=1`, nonnegative signed-64-bit
`revision`). `canvas_records` has an internal rowid and columns `kind`, `id`,
`position`, `data`, and `digest`, unique on `(kind,id)`. Data is a BLOB; digest
is SHA-256 of that BLOB. Position is the zero-based order within a kind. Record
ids are bound SQL text values, never SQL syntax or filesystem paths.

| Kind | Record | Payload |
| --- | --- | --- |
| 0 | One header, empty id | Model major/minor as two u16 values, i32 chunk size (also retained while finite), then the snapshot payload prefix through asset count |
| 1 | One record per asset, asset id | Snapshot asset record, always **raw** little-endian ARGB32 for raster/chunk pixels |
| 2 | One layer count, empty id | u32 layer count |
| 3 | One record per layer, layer id | Snapshot layer record, including projected frame-owned keys and optional range |
| 4 | One metadata trailer, empty id | The version's optional generation metadata payload, empty before model 1.2 |
| 5 | One audio asset count, empty id, model 1.4+ only | u32 audio asset count |
| 6 | One record per audio asset, asset id, model 1.4+ only | Snapshot audio asset record, including raw interleaved signed PCM16 |
| 7 | One audio track count, empty id, model 1.4+ only | u32 audio track count |
| 8 | One record per audio track, track id, model 1.4+ only | Snapshot audio track record, including clips, trims, mute, enabled state, and gain |

The field encodings follow `FORMAT.md`, except raw raster storage is required
even where the interchange codec would choose run-length encoding. Stable ids
separate records from collection positions: reordering does not rewrite pixel
payloads. Unchanged raster and audio assets are not serialized again. Equal-size changed
records use incremental BLOB writes for changed spans; a resized record is
replaced individually. No complete canvas dump is written for an edit.

Model 1.4 retains schema 1 because the existing table already supports typed
records without a restricted kind range. Records 0–4 and their encodings remain
unchanged. Model 1.0–1.3 working files omit records 5–8 and continue to reopen
without audio. A model 1.4 file requires both audio count records, even when
empty. Earlier readers reject the newer model/kinds and cannot silently discard
audio. Audio PCM has its own record, so clip timing, source trim, track mute and
gain edits never rewrite the associated audio payload. A changed PCM16 scalar
sample writes at most two logical payload bytes through the same BLOB patcher.

`lastWriteStatistics()` reports logical record/payload writes for the last
operation, excluding SQLite pages, hashes, indices, and journal overhead. A
single changed pixel can write at most four payload bytes; this is not a claim
about physical disk I/O. Authoring transactions currently copy a document draft
in memory for rollback; undo snapshots are immutable/shared across transaction
copies. Large-document latency and cross-device sync costs require profiling.

## Legacy interchange

`encodeIisc` / `decodeIisc` retain their canonical version-1.0–1.4 binary
snapshot contract and fixed golden compatibility tests. They are explicit
import/export APIs, not the working-file editing path. Detect the header, not
just the extension. `DocumentFile::open` rejects a legacy snapshot without
modifying it. To adopt one, decode and validate it, then call `create` with a
**new** working-file path and that document; preserve the original snapshot.
To export a snapshot, call `encodeIisc(*file.document())` explicitly.

`CameraRawData` and the separate `TimelineProject` remain import/adjacent models,
not fields of the persisted canvas. Native model 1.4 audio belongs to the canvas
timeline; it does not serialize that separate video-project model or add a RAW
codec, and it does not edit any downstream application.
