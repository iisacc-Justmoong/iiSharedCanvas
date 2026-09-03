cmake_minimum_required(VERSION 3.31)
project(iiSharedCanvas VERSION 0.8.0 LANGUAGES CXX)

set(IISHAREDCANVAS_LICENSE_SPDX "AGPL-3.0-only")

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(GenerateExportHeader)

if(EXISTS "$ENV{HOME}/.local/iiPaintEngine")
    list(PREPEND CMAKE_PREFIX_PATH "$ENV{HOME}/.local/iiPaintEngine")
endif()

find_package(iiPaintEngine 0.1.0 CONFIG REQUIRED)
find_package(ZLIB 1.2.9 REQUIRED)
find_package(libzip 1.7.3 CONFIG REQUIRED)
block(SCOPE_FOR VARIABLES)
    # Prefer the SDK/library headers over SQLite copies embedded in unrelated frameworks.
    set(CMAKE_FIND_FRAMEWORK LAST)
    find_package(SQLite3 3.26 REQUIRED)
endblock()
if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

include(cmake/iiSharedCanvasAppleQtCompatibility.cmake)

option(IISHAREDCANVAS_BUILD_SHARED "Build iiSharedCanvas as a shared library." ON)

set(IISHAREDCANVAS_LIBRARY_TYPE STATIC)
if(IISHAREDCANVAS_BUILD_SHARED AND NOT EMSCRIPTEN)
    set(IISHAREDCANVAS_LIBRARY_TYPE SHARED)
endif()

add_library(iiSharedCanvas ${IISHAREDCANVAS_LIBRARY_TYPE}
        src/Bitmap/BitmapCodec.cpp
        src/Bitmap/BitmapCodec.h
        src/Bitmap/ExtendedBitmapCodec.cpp
        src/Bitmap/ExtendedBitmapCodec_p.hpp
        src/Bitmap/BitmapEditor.cpp
        src/Bitmap/BitmapEditor.h
        src/Bitmap/ChunkedBitmapEditor.cpp
        src/Bitmap/ChunkedBitmapEditor.h
        src/Camera/CameraRaw.cpp
        src/Camera/CameraRaw.h
        src/Document/Document.cpp
        src/Document/Document.h
        src/Document/DocumentEditor.cpp
        src/Document/DocumentEditor.h
        src/File/DocumentFile.cpp
        src/File/DocumentFile.h
        src/Layered/LayeredDocumentCodec.cpp
        src/Layered/LayeredDocumentCodec.h
        src/Layered/OpenRasterParser.cpp
        src/Layered/OpenRasterParser_p.hpp
        src/Layered/PsdParser.cpp
        src/Layered/PsdParser_p.hpp
        src/Layered/PsdWriter.cpp
        src/Layered/PsdWriter_p.hpp
        src/Media/MediaIo.cpp
        src/Media/MediaIo.h
        src/Media/MediaIo_p.hpp
        src/Metadata/StableDiffusionGenerationParameters.cpp
        src/Metadata/StableDiffusionGenerationParameters.h
        src/Metadata/StableDiffusionMetadata.cpp
        src/Metadata/StableDiffusionMetadata.h
        src/QtAdapter/CanvasItem.cpp
        src/QtAdapter/CanvasItem.h
        src/QtAdapter/BitmapItem.cpp
        src/QtAdapter/BitmapItem.h
        src/QtAdapter/AsyncFrameRenderer.cpp
        src/QtAdapter/AsyncFrameRenderer.h
        src/Render/FrameRenderer.cpp
        src/Render/FrameRenderer.h
        src/Serialization/IiscCodec.cpp
        src/Serialization/IiscCodec.h
        src/Timeline/TimelineEditor.cpp
        src/Timeline/TimelineEditor.h
        src/Timeline/TimelineInterchange.cpp
        src/Timeline/TimelineInterchange.h
        src/Timeline/TimelineXmlWriter.cpp
        src/Timeline/TimelineXmlWriter_p.hpp
        src/Timeline/TimelineProject.cpp
        src/Timeline/TimelineProject.h
        src/Validation/Validation.cpp
        src/Validation/Validation.h
        src/Vector/VectorEditor.cpp
        src/Vector/VectorEditor.h
        src/Vector/SvgParser.cpp
        src/Vector/SvgParser_p.hpp
        src/Vector/VectorCodec.cpp
        src/Vector/VectorCodec.h
        src/Video/VideoCodec.cpp
        src/Video/VideoCodec.h)
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
target_link_libraries(iiSharedCanvas PRIVATE SQLite3::SQLite3 ZLIB::ZLIB libzip::zip)
if(APPLE OR UNIX)
    target_link_options(iiSharedCanvas INTERFACE
            "LINKER:-rpath,$<TARGET_FILE_DIR:iiPaintEngine::iiPaintEngine>")
endif()

set_target_properties(iiSharedCanvas PROPERTIES
        AUTOMOC ON
        VERSION ${PROJECT_VERSION}
        SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
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

add_executable(iisc-import tools/iisc-import.cpp)
target_link_libraries(iisc-import PRIVATE iiSharedCanvas::iiSharedCanvas)
# Keep the configured external Qt/framework runtime paths in a host install,
# in addition to the relocatable sibling library path below.
set_target_properties(iisc-import PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
if(APPLE)
    set_property(TARGET iisc-import APPEND PROPERTY INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
    set_property(TARGET iisc-import APPEND PROPERTY INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS iisc-import RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(iisc-export-psd tools/iisc-export-psd.cpp)
target_sources(iisc-export-psd PRIVATE tools/IiscInput.cpp tools/IiscInput_p.hpp)
target_link_libraries(iisc-export-psd PRIVATE iiSharedCanvas::iiSharedCanvas SQLite3::SQLite3)
set_target_properties(iisc-export-psd PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
if(APPLE)
    set_property(TARGET iisc-export-psd APPEND PROPERTY INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
    set_property(TARGET iisc-export-psd APPEND PROPERTY INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS iisc-export-psd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(iisc-export-timeline tools/iisc-export-timeline.cpp)
target_sources(iisc-export-timeline PRIVATE tools/IiscInput.cpp tools/IiscInput_p.hpp)
target_link_libraries(iisc-export-timeline PRIVATE iiSharedCanvas::iiSharedCanvas SQLite3::SQLite3)
set_target_properties(iisc-export-timeline PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
if(APPLE)
    set_property(TARGET iisc-export-timeline APPEND PROPERTY INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
    set_property(TARGET iisc-export-timeline APPEND PROPERTY INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS iisc-export-timeline RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS iiSharedCanvas
        EXPORT iiSharedCanvasTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/iiSharedCanvas.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas)
install(DIRECTORY src/Bitmap src/Camera src/Document src/File src/Layered src/Media src/Metadata src/QtAdapter src/Render src/Serialization src/Timeline src/Validation src/Vector src/Video
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
install(FILES README.md docs/API.md docs/BLUEPRINT.md docs/FORMAT.md docs/DEPENDENCIES.md docs/PERSISTENCE.md docs/MEDIA_IO.md
        docs/OPENRASTER_IMPORT.md docs/PSD_IMPORT.md docs/LAYERED_IMPORT_CLI.md
        docs/PSD_EXPORT.md docs/PSD_EXPORT_CLI.md
        docs/TIMELINE_INTERCHANGE.md docs/TIMELINE_INTERCHANGE_CLI.md docs/TIMELINE_XML.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/iiSharedCanvas)

configure_package_config_file(
        cmake/iiSharedCanvasConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvas)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY ExactVersion)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvas)

if(BUILD_TESTING)
    foreach(import_test LayeredDocumentCodec OpenRasterImport PsdLayeredImport LayeredImportCli PsdExport PsdExportCli TimelineInterchange TimelineInterchangeCli)
        add_executable(iiSharedCanvas${import_test}Test tests/${import_test}Test.cpp)
        target_compile_definitions(iiSharedCanvas${import_test}Test PRIVATE
                IISHAREDCANVAS_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        target_link_libraries(iiSharedCanvas${import_test}Test PRIVATE iiSharedCanvas::iiSharedCanvas)
        add_test(NAME iiSharedCanvas.${import_test} COMMAND iiSharedCanvas${import_test}Test)
        set_tests_properties(iiSharedCanvas.${import_test} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    target_link_libraries(iiSharedCanvasOpenRasterImportTest PRIVATE libzip::zip)
    target_link_libraries(iiSharedCanvasPsdLayeredImportTest PRIVATE ZLIB::ZLIB)
    target_compile_definitions(iiSharedCanvasLayeredImportCliTest PRIVATE
            IISHAREDCANVAS_IMPORT_EXECUTABLE="$<TARGET_FILE:iisc-import>")
    add_dependencies(iiSharedCanvasLayeredImportCliTest iisc-import)
    target_link_libraries(iiSharedCanvasPsdExportCliTest PRIVATE SQLite3::SQLite3)
    target_compile_definitions(iiSharedCanvasPsdExportCliTest PRIVATE
            IISHAREDCANVAS_EXPORT_PSD_EXECUTABLE="$<TARGET_FILE:iisc-export-psd>")
    add_dependencies(iiSharedCanvasPsdExportCliTest iisc-export-psd)
    target_link_libraries(iiSharedCanvasTimelineInterchangeCliTest PRIVATE SQLite3::SQLite3)
    target_compile_definitions(iiSharedCanvasTimelineInterchangeCliTest PRIVATE
            IISHAREDCANVAS_EXPORT_TIMELINE_EXECUTABLE="$<TARGET_FILE:iisc-export-timeline>")
    add_dependencies(iiSharedCanvasTimelineInterchangeCliTest iisc-export-timeline)
    add_executable(iiSharedCanvasTimelineXmlWriterTest tests/TimelineXmlWriterTest.cpp src/Timeline/TimelineXmlWriter.cpp)
    target_link_libraries(iiSharedCanvasTimelineXmlWriterTest PRIVATE iiSharedCanvas::iiSharedCanvas)
    target_compile_definitions(iiSharedCanvasTimelineXmlWriterTest PRIVATE
            IISHAREDCANVAS_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output")
    add_test(NAME iiSharedCanvas.TimelineXmlWriter COMMAND iiSharedCanvasTimelineXmlWriterTest)

    foreach(codec Bitmap Vector Video)
        add_executable(iiSharedCanvas${codec}CodecTest tests/${codec}CodecTest.cpp)
        target_compile_definitions(iiSharedCanvas${codec}CodecTest PRIVATE
                IISHAREDCANVAS_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        target_link_libraries(iiSharedCanvas${codec}CodecTest PRIVATE iiSharedCanvas::iiSharedCanvas)
        add_test(NAME iiSharedCanvas.${codec}Codec COMMAND iiSharedCanvas${codec}CodecTest)
        set_tests_properties(iiSharedCanvas.${codec}Codec PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    set_tests_properties(iiSharedCanvas.VideoCodec PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(iiSharedCanvasDocumentFileTest tests/DocumentFileTest.cpp)
    target_compile_definitions(iiSharedCanvasDocumentFileTest PRIVATE
            IISHAREDCANVAS_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output")
    target_link_libraries(iiSharedCanvasDocumentFileTest PRIVATE
            iiSharedCanvas::iiSharedCanvas SQLite3::SQLite3)
    add_test(NAME iiSharedCanvas.DocumentFile COMMAND iiSharedCanvasDocumentFileTest)
    set_tests_properties(iiSharedCanvas.DocumentFile PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(iiSharedCanvasStableDiffusionGenerationParametersTest
            tests/StableDiffusionGenerationParametersTest.cpp)
    target_link_libraries(iiSharedCanvasStableDiffusionGenerationParametersTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.StableDiffusionGenerationParameters
            COMMAND iiSharedCanvasStableDiffusionGenerationParametersTest)

    add_executable(iiSharedCanvasStableDiffusionMetadataTest
            tests/StableDiffusionMetadataTest.cpp)
    target_link_libraries(iiSharedCanvasStableDiffusionMetadataTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.StableDiffusionMetadata
            COMMAND iiSharedCanvasStableDiffusionMetadataTest)

    add_executable(iiSharedCanvasCameraRawTest
            tests/CameraRawTest.cpp)
    target_link_libraries(iiSharedCanvasCameraRawTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.CameraRaw
            COMMAND iiSharedCanvasCameraRawTest)

    add_executable(iiSharedCanvasTimelineProjectTest
            tests/TimelineProjectTest.cpp)
    target_link_libraries(iiSharedCanvasTimelineProjectTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.TimelineProject
            COMMAND iiSharedCanvasTimelineProjectTest)

    add_executable(iiSharedCanvasTimelineEditorTest
            tests/TimelineEditorTest.cpp)
    target_link_libraries(iiSharedCanvasTimelineEditorTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.TimelineEditor
            COMMAND iiSharedCanvasTimelineEditorTest)

    add_executable(iiSharedCanvasDocumentModelTest
            tests/DocumentModelTest.cpp)
    target_link_libraries(iiSharedCanvasDocumentModelTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.DocumentModel
            COMMAND iiSharedCanvasDocumentModelTest)

    add_executable(iiSharedCanvasDocumentEditorTest
            tests/DocumentEditorTest.cpp)
    target_link_libraries(iiSharedCanvasDocumentEditorTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.DocumentEditor
            COMMAND iiSharedCanvasDocumentEditorTest)

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

    add_executable(iiSharedCanvasVectorEditorTest
            tests/VectorEditorTest.cpp)
    target_link_libraries(iiSharedCanvasVectorEditorTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.VectorEditor
            COMMAND iiSharedCanvasVectorEditorTest)

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

    add_executable(iiSharedCanvasFrameRendererTest
            tests/FrameRendererTest.cpp)
    target_link_libraries(iiSharedCanvasFrameRendererTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.FrameRenderer
            COMMAND iiSharedCanvasFrameRendererTest)

    add_executable(iiSharedCanvasCanvasItemRenderTest
            tests/CanvasItemRenderTest.cpp)
    target_compile_definitions(iiSharedCanvasCanvasItemRenderTest PRIVATE
            IISHAREDCANVAS_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/test-output")
    target_link_libraries(iiSharedCanvasCanvasItemRenderTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.CanvasItemRender
            COMMAND iiSharedCanvasCanvasItemRenderTest)
    set_tests_properties(iiSharedCanvas.CanvasItemRender PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(iiSharedCanvasIiscCodecTest
            tests/IiscCodecTest.cpp)
    target_link_libraries(iiSharedCanvasIiscCodecTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.IiscCodec
            COMMAND iiSharedCanvasIiscCodecTest)

    add_executable(iiSharedCanvasInfiniteCanvasTest
            tests/InfiniteCanvasTest.cpp)
    target_link_libraries(iiSharedCanvasInfiniteCanvasTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.InfiniteCanvas
            COMMAND iiSharedCanvasInfiniteCanvasTest)
    set_tests_properties(iiSharedCanvas.InfiniteCanvas PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(iiSharedCanvasAsyncFrameRendererTest
            tests/AsyncFrameRendererTest.cpp)
    target_link_libraries(iiSharedCanvasAsyncFrameRendererTest PRIVATE
            iiSharedCanvas::iiSharedCanvas)
    add_test(NAME iiSharedCanvas.AsyncFrameRenderer
            COMMAND iiSharedCanvasAsyncFrameRendererTest)

    add_test(NAME iiSharedCanvas.ProjectContract
            COMMAND ${CMAKE_COMMAND}
                    -DIISHAREDCANVAS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ProjectContractTest.cmake)
endif()
