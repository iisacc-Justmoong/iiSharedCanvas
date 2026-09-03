# Native canvas timeline package command line

The installed `iisc-export-timeline` utility exports a complete native canvas
timeline through `exportTimelineInterchange`. Each canvas layer remains a
separate timeline track, and native hold-key intervals remain clips. The output
is a new directory containing `timeline.xml` (legacy Final Cut Pro XML),
`timeline.fcpxml`, `media/*.png`, `manifest.json`, and the editable `source.iisc`
snapshot. PNG tracks are rendered interchange media; the source snapshot retains
the original raster/vector assets and native editing information.

```sh
iisc-export-timeline drawing.iisc delivery-package
iisc-export-timeline "drawing with spaces.iisc" "delivery package" --name "Scene 01"
iisc-export-timeline --name "Scene 01" -- -input.iisc -delivery-package
iisc-export-timeline --help
```

`--name TEXT` may appear once before or after the two paths, before `--`, and
defaults to `iisc Timeline`. Its nonempty value may contain Unicode, spaces, and
leading dashes. `--` terminates option parsing so dashed paths remain usable.
`--help` and `-h` are accepted on their own. Unknown options, duplicate names,
missing values or paths, extra paths, and URLs are usage errors. The output does
not require a filename extension. There is no overwrite option: existing files,
directories, and symbolic links are always preserved, and the destination's
parent must already exist. The utility does not create missing parent trees.

Exit status is `0` for successful export/help, `1` for input/conversion/I/O errors,
and `2` for usage errors. Success reports the package destination on standard
output. Failures and conversion warnings are written to standard error. Qt
receives no user paths/options and uses the offscreen platform when none is
explicitly configured. No editor application, network access, or codec download
is invoked by this CLI.

## Shared read-only native input

PSD and timeline export use the same private `tools/IiscInput` loader. Its
contract takes only native media limits and the maximum layer count; it has no
PSD or timeline-format dependency. Input format is detected from content, not
the filename. Binary `.iisc` snapshots use bounded reads and `decodeIisc`.

The original SQLite working file is never passed to the authoring
`DocumentFile::open` API. Instead, a `SQLITE_OPEN_READONLY` source connection and
SQLite's existing online backup API produce a consistent snapshot, including
committed, uncheckpointed WAL data. No URI or immutable mode is used. Only that
private backup is opened with `DocumentFile` for canonical schema, digest, and
document validation. Raw filesystem copies of live databases are not used.

Backups live in a private `.iisc-input-XXXXXX` directory beside the intended
output and are removed on success or failure. The original database and WAL
payload are not rewritten, journal mode is unchanged, and no checkpoint is
forced. SQLite may update its normal shared-memory reader bookkeeping. An
unavailable read snapshot fails explicitly: the busy timeout is 250 ms, copying
uses bounded page batches, and the overall copy deadline is 30 seconds.

The input file and regular-file WAL/journal/shared-memory sidecars share the
input byte budget. Sidecar symlinks are rejected. `page_count * page_size` and
the backup's resulting size must fit both input and decoded-byte limits. Native
decoding also applies canvas pixel, total raster storage, and layer-count bounds.
The public package writer validates the complete export before publishing the
new directory and cleans its private staging directory on failure.

## Verification

`TimelineInterchangeCliTest` launches the real executable and checks both XML
documents, decodable PNG media, manifest duration, and exact canonical native
snapshot preservation. It covers animated bitmap and vector layers,
Unicode/spaced/dashed paths, named sequences, headless use, read-only working
files, committed WAL data and an active WAL writer, SHA-256 source preservation,
destination collisions and aliases, missing parents, malformed native input,
unsupported frame rates, invalid XML metadata, and temporary-directory cleanup.
Sequence-name checks target legacy `sequence/name`, FCPXML `project@name`, and
the manifest field. They account for macOS `QProcess` decomposing Unicode
arguments before the CLI receives them; the CLI preserves the received name.
`PsdExportCliTest` remains the regression suite for the shared loader's existing
frame-zero PSD workflow, including overwrite and source-sidecar protection.

The shared loader adds no dependency: both tools reuse the already reviewed
private SQLite dependency and public iiSharedCanvas APIs. See
[SQLite's online backup API](https://sqlite.org/backup.html) and the package
format's library documentation for interchange limitations.
