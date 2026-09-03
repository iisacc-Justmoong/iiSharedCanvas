# Layered document import command

`iisc-import` is the library's installed command-line utility for converting a
supported layered source into a new `.iisc` working file. It uses
`importLayeredDocument()` followed by `DocumentFile::create()`; it is not a
flattened-image importer or a separate document format implementation.

```sh
iisc-import input.psd output.iisc
iisc-import --id-prefix artwork "layered source.ora" "editable result.iisc"
iisc-import --id-prefix=-artwork -- -source.psd -result.iisc
iisc-import --help
```

## Arguments and results

- Exactly two nonempty positional paths are required. The source is detected
  from its contents, independently of its extension. The output must have the
  `.iisc` extension, case-insensitively.
- `--id-prefix PREFIX` or `--id-prefix=PREFIX` sets the deterministic layer and
  asset ID prefix. The default is `import`; the public API's UTF-8, nonempty and
  length limits also apply. Repeated prefix options are rejected. Use the equals
  form for a prefix beginning with `-`.
- `--` ends option parsing, allowing paths beginning with `-`. Unknown options,
  missing values and extra paths fail. `--help` and `-h` are standalone commands.
- A successful conversion returns exit status `0` and reports the detected
  format, layer count and output path on stdout. Import warnings appear on
  stderr as `iisc-import: warning: ...` even when conversion succeeds.
- Invalid command-line arguments return `2`; import or working-file errors
  return `1` with the API error name and message on stderr. Unsupported
  semantics fail instead of silently replacing layers with a composite image.

The initial readers support documented subsets of PSD and OpenRaster. Consult
[media interchange](MEDIA_IO.md) for supported layer kinds, metadata, blend
modes, masks, groups, compression and resource limits. This utility does not
broaden those readers' fidelity claims and adds no external runtime service.

## Persistence and non-destructive operation

The input is opened read-only and remains unchanged. Existing output files,
directories and symbolic links are rejected, including an input reused as the
output. `DocumentFile::create()` exclusively creates the destination, so a file
appearing after the command's preliminary check is not overwritten either.
Output parent directories must already exist.

The result is a SQLite-backed, synchronously committed `.iisc` working file,
not the legacy `encodeIisc()` snapshot container. Successful creation is durable
before the command reports success; neither closing the file nor a later save
is required to persist its layers. Import errors produce no destination file.
Working-file creation uses the existing transactional failure handling.

Qt's offscreen platform is selected only when `QT_QPA_PLATFORM` is unset or
empty. A caller-supplied platform selection is preserved. Qt platform switches
are not command options; use the environment variable when needed.

The host install records its configured external Qt/framework search paths as
well as the sibling library path, so the installed command runs outside the
build directory. It does not bundle Qt, iiPaintEngine or libzip; shipping a
standalone application still requires packaging the actual runtime dependencies.
`install.sh` executes the installed command's help before building its separate
installed-package consumer, detecting missing runtime libraries during validation.

## Verification

`LayeredImportCliTest.cpp` builds an independent two-layer raw PSD in the
configured `build/` test-output directory, invokes the executable through
`QProcess`, and reopens the result with `DocumentFile`. It checks exact names,
bottom-to-top order, IDs, pixels, alpha and offsets, the SQLite working-file
signature, visible metadata warnings, Unicode/spaced/dashed paths, help and
argument errors, unsupported formats/semantics, collisions and source-byte
preservation. The fixture's black composite differs from its layer pixels to
prevent a flattened-composite fallback from passing.
