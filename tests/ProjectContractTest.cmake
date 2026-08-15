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
             "src/Document/Document.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Bitmap/BitmapEditor.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/QtAdapter/BitmapItem.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "src/Validation/Validation.cpp")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "$<BUILD_INTERFACE:\${CMAKE_CURRENT_SOURCE_DIR}/src>")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "BUILD_RPATH \"$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>\"")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/CMakeLists.txt"
             "LINKER:-rpath,$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/cmake/iiSharedCanvasConfig.cmake.in"
             "find_dependency(iiPaintEngine 0.1.0 CONFIG REQUIRED)")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "The only direct project dependency is iiPaintEngine 0.1.0")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/README.md"
             "`BitmapEditor` binds to a raster asset by id")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "No pointer trajectory, curve, dab stream, replay command")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/docs/BLUEPRINT.md"
             "`BitmapItem` is the Qt Quick display boundary")
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

if(EXISTS "${IISHAREDCANVAS_SOURCE_DIR}/include")
    message(FATAL_ERROR "the source tree must not use a separate include/ directory")
endif()
if(NOT IS_DIRECTORY "${IISHAREDCANVAS_SOURCE_DIR}/src")
    message(FATAL_ERROR "co-located public headers and implementations must live under src/")
endif()

foreach(module Bitmap Document QtAdapter Validation)
    if(EXISTS "${IISHAREDCANVAS_SOURCE_DIR}/${module}")
        message(FATAL_ERROR "${module} must live under src/, not at the repository root")
    endif()
    file(GLOB module_headers "${IISHAREDCANVAS_SOURCE_DIR}/src/${module}/*.h")
    file(GLOB module_implementations "${IISHAREDCANVAS_SOURCE_DIR}/src/${module}/*.cpp")
    if(NOT module_headers OR NOT module_implementations)
        message(FATAL_ERROR "src/${module} must contain co-located headers and implementations")
    endif()
    foreach(module_header IN LISTS module_headers)
        get_filename_component(source_name "${module_header}" NAME_WE)
        if(NOT EXISTS "${IISHAREDCANVAS_SOURCE_DIR}/src/${module}/${source_name}.cpp")
            message(FATAL_ERROR "${module_header} must have a co-located implementation")
        endif()
    endforeach()
    foreach(module_implementation IN LISTS module_implementations)
        get_filename_component(source_name "${module_implementation}" NAME_WE)
        if(NOT EXISTS "${IISHAREDCANVAS_SOURCE_DIR}/src/${module}/${source_name}.h")
            message(FATAL_ERROR "${module_implementation} must have a co-located header")
        endif()
    endforeach()
endforeach()

file(GLOB_RECURSE core_sources
     "${IISHAREDCANVAS_SOURCE_DIR}/src/iiSharedCanvas.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Bitmap/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Bitmap/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Document/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Document/*.cpp"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Validation/*.h"
     "${IISHAREDCANVAS_SOURCE_DIR}/src/Validation/*.cpp")
foreach(source_file IN LISTS core_sources)
    file(READ "${source_file}" source_contents)
    if(source_contents MATCHES "#[ \t]*include[ \t]*<Q[A-Za-z]")
        message(FATAL_ERROR "Core source must not directly include Qt: ${source_file}")
    endif()
endforeach()

require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Bitmap/BitmapEditor.cpp"
             "appendRasterDabs")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/Bitmap/BitmapEditor.cpp"
             "paintRasterSamples")
require_text("${IISHAREDCANVAS_SOURCE_DIR}/src/QtAdapter/BitmapItem.cpp"
             "QImage::Format_ARGB32")
