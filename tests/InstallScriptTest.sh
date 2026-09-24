#!/usr/bin/env bash
set -euo pipefail
source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${source_root}/build/install-script-test"
mkdir -p "${work}/bin" "${work}/home/.local/SDK/iiPaintEngine/lib/cmake/iiPaintEngine"
touch "${work}/home/.local/SDK/iiPaintEngine/lib/cmake/iiPaintEngine/iiPaintEngineConfig.cmake"
cat > "${work}/bin/cmake" <<'MOCK'
#!/usr/bin/env bash
printf '%s\n' "$@" > "${INSTALL_TEST_ARGUMENTS}"
exit 91
MOCK
chmod +x "${work}/bin/cmake"
for mode in default override dependencies; do
    expected="${work}/home/.local/SDK/iiSharedCanvas"
    options=("PATH=${work}/bin:${PATH}" "QT_PREFIX_PATH=${work}/qt" "CMAKE_PREFIX_PATH=")
    dependency_prefix="${work}/dependency one;${work}/dependency two"
    if [[ "${mode}" == dependencies ]]; then
        options+=("CMAKE_PREFIX_PATH=${work}/dependency one:${work}/dependency two")
    fi
    if [[ "${mode}" == override ]]; then
        expected="${work}/custom prefix"
        options+=("IISHAREDCANVAS_INSTALL_PREFIX=${expected}")
    fi
    set +e
    env -u IISHAREDCANVAS_INSTALL_PREFIX -u IISHAREDCANVAS_IIPAINTENGINE_PREFIX \
        HOME="${work}/home" PATH="${work}/bin:${PATH}" \
        INSTALL_TEST_ARGUMENTS="${work}/arguments" "${options[@]}" \
        bash "${source_root}/install.sh" > "${work}/output" 2>&1
    result=$?
    set -e
    [[ ${result} == 91 ]] || { cat "${work}/output"; exit 1; }
    grep -Fx -- "-DCMAKE_INSTALL_PREFIX=${expected}" "${work}/arguments"
    grep -Fx -- "${source_root}/build" "${work}/arguments"
    if [[ "${mode}" == dependencies ]]; then
        grep -Fx -- "-DCMAKE_PREFIX_PATH=${work}/home/.local/SDK/iiPaintEngine;${work}/qt;${dependency_prefix}" "${work}/arguments"
    fi
done
