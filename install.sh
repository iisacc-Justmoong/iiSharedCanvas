#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
INSTALL_PREFIX="${IISHAREDCANVAS_INSTALL_PREFIX:-${HOME}/.local/iiSharedCanvas}"
PAINT_ENGINE_PREFIX="${IISHAREDCANVAS_IIPAINTENGINE_PREFIX:-${HOME}/.local/iiPaintEngine}"
CONSUMER_BUILD_DIR="${BUILD_DIR}/consumer"

if [[ ! -f "${PAINT_ENGINE_PREFIX}/lib/cmake/iiPaintEngine/iiPaintEngineConfig.cmake" ]]; then
    echo "iiPaintEngine CMake package is required: ${PAINT_ENGINE_PREFIX}" >&2
    exit 1
fi

echo "Configuring iiSharedCanvas in ${BUILD_DIR}"
cmake --fresh \
    -S "${PROJECT_ROOT}" \
    -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
    -DCMAKE_PREFIX_PATH="${PAINT_ENGINE_PREFIX}"

echo "Building iiSharedCanvas"
cmake --build "${BUILD_DIR}" --config Release --parallel

echo "Running iiSharedCanvas tests"
ctest --test-dir "${BUILD_DIR}" --build-config Release --output-on-failure --parallel

echo "Installing iiSharedCanvas into ${INSTALL_PREFIX}"
cmake --install "${BUILD_DIR}" --prefix "${INSTALL_PREFIX}" --config Release

test -f "${INSTALL_PREFIX}/include/iiSharedCanvas/iiSharedCanvas.h"
test -f "${INSTALL_PREFIX}/include/iiSharedCanvas/Document/Document.h"
test -f "${INSTALL_PREFIX}/include/iiSharedCanvas/Validation/Validation.h"
test -f "${INSTALL_PREFIX}/lib/cmake/iiSharedCanvas/iiSharedCanvasConfig.cmake"

installed_library="$(find "${INSTALL_PREFIX}" -maxdepth 3 -type f \
    \( -name 'libiiSharedCanvas.*' -o -name 'iiSharedCanvas.dll' \) | head -n 1)"
if [[ -z "${installed_library}" ]]; then
    echo "Installed iiSharedCanvas library was not found under ${INSTALL_PREFIX}" >&2
    exit 1
fi

echo "Configuring standalone installed-package consumer"
cmake --fresh \
    -S "${PROJECT_ROOT}/tests/consumer" \
    -B "${CONSUMER_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${INSTALL_PREFIX};${PAINT_ENGINE_PREFIX}"
cmake --build "${CONSUMER_BUILD_DIR}" --config Release --parallel

consumer_executable=""
for candidate in \
    "${CONSUMER_BUILD_DIR}/iiSharedCanvasConsumer" \
    "${CONSUMER_BUILD_DIR}/Release/iiSharedCanvasConsumer" \
    "${CONSUMER_BUILD_DIR}/iiSharedCanvasConsumer.exe" \
    "${CONSUMER_BUILD_DIR}/Release/iiSharedCanvasConsumer.exe"; do
    if [[ -x "${candidate}" ]]; then
        consumer_executable="${candidate}"
        break
    fi
done

if [[ -z "${consumer_executable}" ]]; then
    echo "Standalone consumer executable was not produced" >&2
    exit 1
fi

"${consumer_executable}"
echo "Verified installed package: ${installed_library}"
