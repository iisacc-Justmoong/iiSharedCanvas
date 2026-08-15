cmake_minimum_required(VERSION 3.31)
project(iiSharedCanvas VERSION 0.1.0 LANGUAGES CXX)

set(IISHAREDCANVAS_LICENSE_SPDX "AGPL-3.0-only")

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(GenerateExportHeader)

if(EXISTS "$ENV{HOME}/.local/iiPaintEngine")
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local/iiPaintEngine")
endif()

find_package(iiPaintEngine 0.1.0 CONFIG REQUIRED)

include(cmake/iiSharedCanvasAppleQtCompatibility.cmake)

option(IISHAREDCANVAS_BUILD_SHARED "Build iiSharedCanvas as a shared library." ON)

set(IISHAREDCANVAS_LIBRARY_TYPE STATIC)
if(IISHAREDCANVAS_BUILD_SHARED AND NOT EMSCRIPTEN)
    set(IISHAREDCANVAS_LIBRARY_TYPE SHARED)
endif()

add_library(iiSharedCanvas ${IISHAREDCANVAS_LIBRARY_TYPE}
        src/Bitmap/BitmapEditor.cpp
        src/Bitmap/BitmapEditor.h
        src/Document/Document.cpp
        src/Document/Document.h
        src/QtAdapter/BitmapItem.cpp
        src/QtAdapter/BitmapItem.h
        src/Validation/Validation.cpp
        src/Validation/Validation.h)
add_library(iiSharedCanvas::iiSharedCanvas ALIAS iiSharedCanvas)

generate_export_header(iiSharedCanvas
        EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/Export.h")

target_compile_features(iiSharedCanvas PUBLIC cxx_std_20)
target_include_directories(iiSharedCanvas PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas>)
target_link_libraries(iiSharedCanvas PUBLIC
        iiPaintEngine::iiPaintEngine
        Qt6::Qml
        Qt6::Quick)
if(APPLE OR UNIX)
    target_link_options(iiSharedCanvas INTERFACE
            "LINKER:-rpath,$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>")
endif()

set_target_properties(iiSharedCanvas PROPERTIES
        AUTOMOC ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
set_property(TARGET iiSharedCanvas APPEND PROPERTY
        BUILD_RPATH "$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>")
set_property(TARGET iiSharedCanvas APPEND PROPERTY
        INSTALL_RPATH "$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>")
if(APPLE)
    set_property(TARGET iiSharedCanvas APPEND PROPERTY INSTALL_RPATH
            "@loader_path/../../iiPaintEngine/platforms/macos/lib"
            "@loader_path/../../iiPaintEngine/lib")
elseif(UNIX)
    set_property(TARGET iiSharedCanvas APPEND PROPERTY INSTALL_RPATH
            "$ORIGIN/../../iiPaintEngine/platforms/linux/lib"
            "$ORIGIN/../../iiPaintEngine/lib")
endif()

install(TARGETS iiSharedCanvas
        EXPORT iiSharedCanvasTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/iiSharedCanvas.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas)
install(DIRECTORY src/Bitmap src/Document src/QtAdapter src/Validation
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas
        FILES_MATCHING PATTERN "*.h")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/Export.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas)
install(EXPORT iiSharedCanvasTargets
        FILE iiSharedCanvasTargets.cmake
        NAMESPACE iiSharedCanvas::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvas)
install(FILES cmake/iiSharedCanvasAppleQtCompatibility.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvas)
install(FILES LICENSE NOTICE.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/iiSharedCanvas)
install(FILES README.md docs/BLUEPRINT.md docs/FORMAT.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/iiSharedCanvas)

configure_package_config_file(
        cmake/iiSharedCanvasConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvas)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvas)

if(BUILD_TESTING)
    add_executable(iiSharedCanvasDocumentModelTest
            tests/DocumentModelTest.cpp)
    target_link_libraries(iiSharedCanvasDocumentModelTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.DocumentModel
            COMMAND iiSharedCanvasDocumentModelTest)

    add_executable(iiSharedCanvasValidationTest
            tests/ValidationTest.cpp)
    target_link_libraries(iiSharedCanvasValidationTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.Validation
            COMMAND iiSharedCanvasValidationTest)

    add_executable(iiSharedCanvasPublicApiTest
            tests/PublicApiTest.cpp)
    target_link_libraries(iiSharedCanvasPublicApiTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.PublicApi
            COMMAND iiSharedCanvasPublicApiTest)

    add_executable(iiSharedCanvasBitmapEditorTest
            tests/BitmapEditorTest.cpp)
    target_link_libraries(iiSharedCanvasBitmapEditorTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.BitmapEditor
            COMMAND iiSharedCanvasBitmapEditorTest)

    add_executable(iiSharedCanvasBitmapItemRenderTest
            tests/BitmapItemRenderTest.cpp)
    target_compile_definitions(iiSharedCanvasBitmapItemRenderTest PRIVATE
            IISHAREDCANVAS_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output")
    target_link_libraries(iiSharedCanvasBitmapItemRenderTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.BitmapItemRender
            COMMAND iiSharedCanvasBitmapItemRenderTest)
    set_tests_properties(iiSharedCanvas.BitmapItemRender PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_test(NAME iiSharedCanvas.ProjectContract
            COMMAND ${CMAKE_COMMAND}
                    -DIISHAREDCANVAS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ProjectContractTest.cmake)
endif()
