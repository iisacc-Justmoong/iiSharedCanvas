SPDX-License-Identifier: Apache-2.0

Copyright (c) 2026 iisacc

iiSharedCanvas is licensed under the Apache License, Version 2.0. The complete license text is included in LICENSE. Attribution for this distribution is in NOTICE.

iiSDK modules, including the required iiPaintEngine dependency, are Apache-2.0. iisacc products under Product remain licensed under the GNU Affero General Public License (AGPL).

Working-file persistence additionally links the platform-provided SQLite
library, whose delivered code is dedicated to the public domain. No SQLite
source is vendored here. See docs/DEPENDENCIES.md for the dependency review.

SVGZ support links the platform/package-provided zlib under the zlib license.
OpenRaster ZIP reading links the platform/package-provided libzip under its
BSD-3-Clause license. No libzip source is vendored. Its optional codec runtime
dependencies retain their own licenses and must accompany deployed binaries.
Existing Qt image plugins retain their Qt and third-party codec licenses.
PSD Smart Object export reuses Qt PDF writing. Optional psd-tools development
verification is not a product dependency and is not redistributed here.
The optional video adapter executes a separately supplied FFmpeg/ffprobe build;
no FFmpeg binary or source is redistributed by this package. Its LGPL/GPL terms
depend on the supplied build. See docs/DEPENDENCIES.md for packaging boundaries.

The vendored fast_float header under third_party/fast_float keeps the upstream
MIT License or the Boost License. See third_party/fast_float/LICENSE-MIT.
