if(NOT DEFINED IISHAREDCANVAS_SOURCE_DIR)
    message(FATAL_ERROR "IISHAREDCANVAS_SOURCE_DIR is required")
endif()

function(require_text file_path expected)
    file(READ "${file_path}" contents)
    string(FIND "${contents}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${file_path} must contain: ${expected}")
    endif()
endfunction()

file(READ "${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt" cmake_lists)
string(REGEX MATCHALL "find_package\\(" direct_find_packages "${cmake_lists}")
list(LENGTH direct_find_packages direct_dependency_count)
if(NOT direct_dependency_count EQUAL 1)
    message(FATAL_ERROR "iiSharedCanvas must have exactly one direct find_package dependency")
endif()

require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "find_package(iiPaintEngine 0.1.0 CONFIG REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "iiPaintEngine::iiPaintEngine")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "BUILD_RPATH \"$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>\"")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "LINKER:-rpath,$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/cmake/iiSharedCanvasConfig.cmake.in"
             "find_dependency(iiPaintEngine 0.1.0 CONFIG REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "The only direct project dependency is iiPaintEngine 0.1.0")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "No pointer trajectory, curve, dab stream, replay command")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "Version 1 uses hold sampling only")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/FORMAT.md"
             "Brush input is not a manifest content kind")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/install.sh"
             "ctest --test-dir")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/LICENSE"
             "GNU AFFERO GENERAL PUBLIC LICENSE")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/NOTICE.md"
             "SPDX-License-Identifier: AGPL-3.0-only")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakePresets.json"
             "\"binaryDir\": \"\${sourceDir}/build\"")

file(GLOB_RECURSE core_sources
     "${IISHAREDCANVAS_SOURCE_DIR}/include/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/*.cpp")
foreach(source_file IN LISTS core_sources)
    file(READ "${source_file}" source_contents)
    if(source_contents MATCHES "#[ \t]*include[ \t]*[<\"]Q[A-Za-z]")
        message(FATAL_ERROR "Core source must not directly include Qt: ${source_file}")
    endif()
endforeach()
