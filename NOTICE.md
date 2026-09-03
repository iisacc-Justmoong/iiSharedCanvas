SPDX-License-Identifier: AGPL-3.0-only

Copyright (c) 2026 iisacc

iiSharedCanvas is licensed under the GNU Affero General Public License,
version 3 only. The complete license text is included in LICENSE.

This license matches the AGPL-3.0-only license of the required iiPaintEngine
dependency.

Working-file persistence additionally links the platform-provided SQLite
library, whose delivered code is dedicated to the public domain. No SQLite
source is vendored here. See docs/DEPENDENCIES.md for the dependency review.

SVGZ support links the platform/package-provided zlib under the zlib license.
Existing Qt image plugins retain their Qt and third-party codec licenses.
The optional video adapter executes a separately supplied FFmpeg/ffprobe build;
no FFmpeg binary or source is redistributed by this package. Its LGPL/GPL terms
depend on the supplied build. See docs/DEPENDENCIES.md for packaging boundaries.
