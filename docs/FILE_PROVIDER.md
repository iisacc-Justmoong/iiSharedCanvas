# File creation ownership in iiSharedCanvas

Timeline-package staging, IISC input staging and file locks use provider factories. IISC schemas, validated document edits and media/timeline serialization remain in SharedCanvas.

The common [ownership contract](../../iiFileProvider/docs/FILE_OWNERSHIP.md) defines stream, staging, control-file and
installed-package rules. Source regression tests remain with this module; the
workspace creation audit also checks these boundaries. Build/deployment and target
runtime validation are separate evidence.

Changed boundaries:


- `src/File/IiscInput.cpp`
- `src/Timeline/TimelineInterchange.cpp`
