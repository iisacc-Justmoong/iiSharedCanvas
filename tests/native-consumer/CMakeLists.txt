cmake_minimum_required(VERSION 3.31)
project(NativeCanvasConsumer LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/iiSharedCanvasTestWorkDir.cmake")
iisc_configure_test_work_dir(_iisc_test_work "${CMAKE_CURRENT_SOURCE_DIR}/../../resources")
set(CMAKE_DISABLE_FIND_PACKAGE_Qt5 ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Qt6 ON)
find_package(iiSharedCanvasNative CONFIG REQUIRED)
if(TARGET Qt6::Core OR TARGET Qt6::Gui OR TARGET Qt5::Core)
    message(FATAL_ERROR "The native installed consumer imported Qt")
endif()
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE iiSharedCanvas::Native)
enable_testing()
add_test(NAME NativeCanvas.TestWorkDirContract COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/../.." "-DWORK_DIR=${_iisc_test_work}" "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../NativeTestWorkDirContract.cmake")
add_test(NAME NativeCanvas.Relocated COMMAND consumer "${_iisc_test_work}")
add_executable(async-frame-render ../NativeAsyncFrameRenderTest.cpp)
target_link_libraries(async-frame-render PRIVATE iiSharedCanvas::Native)
add_test(NAME NativeCanvas.AsyncFrameRender COMMAND async-frame-render)
set_tests_properties(NativeCanvas.AsyncFrameRender PROPERTIES ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
find_package(iiSDKCore 0.18 CONFIG REQUIRED COMPONENTS Runtime)
add_executable(async-runtime ../NativeAsyncRuntimeTest.cpp)
target_link_libraries(async-runtime PRIVATE iiSharedCanvas::Native iiSDKCore::Runtime)
add_test(NAME NativeCanvas.AsyncRuntime COMMAND async-runtime)
set_tests_properties(NativeCanvas.AsyncRuntime PROPERTIES TIMEOUT 120 ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
add_executable(svg-export ../NativeSvgExportTest.cpp)
target_link_libraries(svg-export PRIVATE iiSharedCanvas::Native)
target_compile_definitions(svg-export PRIVATE IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/svg-output")
add_test(NAME NativeCanvas.SvgExport COMMAND svg-export)
set_tests_properties(NativeCanvas.SvgExport PROPERTIES TIMEOUT 120 ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
add_executable(pdf-export ../NativePdfExportTest.cpp)
target_link_libraries(pdf-export PRIVATE iiSharedCanvas::Native)
target_compile_definitions(pdf-export PRIVATE IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/pdf-output")
add_test(NAME NativeCanvas.PdfExport COMMAND pdf-export)
set_tests_properties(NativeCanvas.PdfExport PROPERTIES TIMEOUT 120 ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
find_package(iiJson 0.1 CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_executable(timeline-interchange ../NativeTimelineInterchangeTest.cpp)
target_link_libraries(timeline-interchange PRIVATE iiSharedCanvas::Native iiJson::iiJson Threads::Threads)
if(MINGW)
    target_link_options(timeline-interchange PRIVATE -municode)
endif()
target_compile_definitions(timeline-interchange PRIVATE IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/timeline-output")
add_test(NAME NativeCanvas.TimelineInterchange COMMAND timeline-interchange)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME NativeCanvas.TimelineOracle COMMAND ${CMAKE_COMMAND}
        "-DDRIVER=$<TARGET_FILE:timeline-interchange>" "-DPYTHON=${Python3_EXECUTABLE}"
        "-DORACLE=${CMAKE_CURRENT_SOURCE_DIR}/../verify_timeline_interchange.py"
        "-DWORK=${_iisc_test_work}/timeline-oracle-output"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../NativeTimelineOracle.cmake")
    set_tests_properties(NativeCanvas.TimelineOracle PROPERTIES TIMEOUT 300 ENVIRONMENT_MODIFICATION
        "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
endif()
set_tests_properties(NativeCanvas.TimelineInterchange PROPERTIES TIMEOUT 300 ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
add_executable(bitmap-input ../BitmapInputTest.cpp)
target_link_libraries(bitmap-input PRIVATE iiSharedCanvas::Native)
target_compile_definitions(bitmap-input PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/input-output")
add_test(NAME NativeCanvas.BitmapInput COMMAND bitmap-input)
add_executable(bitmap-wet ../BitmapWetBrushTest.cpp)
target_link_libraries(bitmap-wet PRIVATE iiSharedCanvas::Native)
target_compile_definitions(bitmap-wet PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/wet-output")
add_test(NAME NativeCanvas.BitmapWetBrush COMMAND bitmap-wet)
add_executable(bitmap-clock ../BitmapClockTest.cpp)
target_link_libraries(bitmap-clock PRIVATE iiSharedCanvas::Native)
target_compile_definitions(bitmap-clock PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/clock-output")
add_test(NAME NativeCanvas.BitmapClock COMMAND bitmap-clock)
add_executable(bitmap-history ../BitmapHistoryTest.cpp)
target_link_libraries(bitmap-history PRIVATE iiSharedCanvas::Native)
target_compile_definitions(bitmap-history PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/history-output")
add_test(NAME NativeCanvas.BitmapHistory COMMAND bitmap-history)
add_test(NAME NativeCanvas.BitmapHistory8K COMMAND bitmap-history --large)
add_executable(bitmap-history-allocation ../BitmapHistoryAllocationTest.cpp)
target_link_libraries(bitmap-history-allocation PRIVATE iiSharedCanvas::Native)
add_test(NAME NativeCanvas.BitmapHistoryAllocation COMMAND bitmap-history-allocation)
option(IISHAREDCANVAS_TEST_BRUSH_AUTHORING "Test the optional upper authoring consumer" OFF)
if(IISHAREDCANVAS_TEST_BRUSH_AUTHORING)
    find_package(iiBrushTools 0.1 CONFIG REQUIRED)
    add_executable(brush-authoring authoring.cpp)
    target_link_libraries(brush-authoring PRIVATE iiSharedCanvas::Native iiBrushTools::iiBrushTools)
    add_test(NAME NativeCanvas.BrushAuthoring COMMAND brush-authoring)
    set_tests_properties(NativeCanvas.BrushAuthoring PROPERTIES ENVIRONMENT_MODIFICATION
        "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
endif()
set_tests_properties(NativeCanvas.Relocated NativeCanvas.BitmapInput NativeCanvas.BitmapWetBrush NativeCanvas.BitmapClock PROPERTIES
    ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
set_tests_properties(NativeCanvas.BitmapHistory NativeCanvas.BitmapHistory8K NativeCanvas.BitmapHistoryAllocation PROPERTIES
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(bitmap-pixel-preview ../BitmapPixelPreviewTest.cpp)
target_link_libraries(bitmap-pixel-preview PRIVATE iiSharedCanvas::Native)
target_compile_definitions(bitmap-pixel-preview PRIVATE IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/pixel-preview-output")
add_test(NAME NativeCanvas.BitmapPixelPreview COMMAND bitmap-pixel-preview)
add_test(NAME NativeCanvas.BitmapPixelPreview8K COMMAND bitmap-pixel-preview --large)
set_tests_properties(NativeCanvas.BitmapPixelPreview NativeCanvas.BitmapPixelPreview8K PROPERTIES ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(working-tiles ../WorkingFileTilesTest.cpp)
target_link_libraries(working-tiles PRIVATE iiSharedCanvas::Native)
target_compile_definitions(working-tiles PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/tiles-output"
    IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
add_test(NAME NativeCanvas.WorkingTiles COMMAND working-tiles)
find_program(_working_cli iisc-working HINTS "${iiSharedCanvasNative_DIR}/../../../bin" NO_DEFAULT_PATH REQUIRED)
add_test(NAME NativeCanvas.WorkingCLI COMMAND ${CMAKE_COMMAND}
    "-DCLI=${_working_cli}" "-DDRIVER=$<TARGET_FILE:working-tiles>"
    "-DWORK=${_iisc_test_work}/cli-output"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../NativeWorkingFileCli.cmake")
set_tests_properties(NativeCanvas.WorkingTiles NativeCanvas.WorkingCLI PROPERTIES
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(bitmap-file-region ../BitmapFileRegionTest.cpp)
target_link_libraries(bitmap-file-region PRIVATE iiSharedCanvas::Native)
target_compile_definitions(bitmap-file-region PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/region-output"
    IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
add_test(NAME NativeCanvas.BitmapFileRegion COMMAND bitmap-file-region)
add_test(NAME NativeCanvas.BitmapFileRegionLarge COMMAND bitmap-file-region --large)
set_tests_properties(NativeCanvas.BitmapFileRegion NativeCanvas.BitmapFileRegionLarge PROPERTIES
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(working-snapshot-allocation ../WorkingSnapshotAllocationTest.cpp)
target_link_libraries(working-snapshot-allocation PRIVATE iiSharedCanvas::Native)
target_compile_definitions(working-snapshot-allocation PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/snapshot-output")
add_test(NAME NativeCanvas.WorkingSnapshotAllocation COMMAND working-snapshot-allocation)
set_tests_properties(NativeCanvas.WorkingSnapshotAllocation PROPERTIES
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(layer-file-edit ../LayerFileEditTest.cpp)
target_link_libraries(layer-file-edit PRIVATE iiSharedCanvas::Native)
target_compile_definitions(layer-file-edit PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/layer-output"
    IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
add_test(NAME NativeCanvas.LayerFileEdit COMMAND layer-file-edit)
set_tests_properties(NativeCanvas.LayerFileEdit PROPERTIES
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(document-history ../DocumentHistoryTest.cpp)
target_link_libraries(document-history PRIVATE iiSharedCanvas::Native)
target_compile_definitions(document-history PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/document-history-output")
add_test(NAME NativeCanvas.DocumentHistory COMMAND document-history)
add_test(NAME NativeCanvas.DocumentHistory8K COMMAND document-history --large)
set_tests_properties(NativeCanvas.DocumentHistory NativeCanvas.DocumentHistory8K PROPERTIES
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(document-history-preview ../DocumentHistoryPreviewTest.cpp)
target_link_libraries(document-history-preview PRIVATE iiSharedCanvas::Native)
target_compile_definitions(document-history-preview PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/document-history-preview-output")
add_test(NAME NativeCanvas.DocumentHistoryPreview COMMAND document-history-preview)
add_test(NAME NativeCanvas.DocumentHistoryPreview8K COMMAND document-history-preview --large)
set_tests_properties(NativeCanvas.DocumentHistoryPreview NativeCanvas.DocumentHistoryPreview8K PROPERTIES
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(mask-region ../MaskRegionTest.cpp)
target_link_libraries(mask-region PRIVATE iiSharedCanvas::Native)
add_test(NAME NativeCanvas.MaskRegion COMMAND mask-region)
add_test(NAME NativeCanvas.MaskRegionCapture COMMAND mask-region --capture)
add_test(NAME NativeCanvas.MaskRegion8K COMMAND mask-region --large)
add_test(NAME NativeCanvas.MaskRegionCost COMMAND mask-region --cost)
set_tests_properties(NativeCanvas.MaskRegion NativeCanvas.MaskRegionCapture NativeCanvas.MaskRegion8K NativeCanvas.MaskRegionCost PROPERTIES
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(mask-region-file-8k ../MaskRegionFile8KTest.cpp)
target_link_libraries(mask-region-file-8k PRIVATE iiSharedCanvas::Native)
target_compile_definitions(mask-region-file-8k PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/mask-file-8k-output")
add_test(NAME NativeCanvas.MaskRegionFile8K COMMAND mask-region-file-8k)
set_tests_properties(NativeCanvas.MaskRegionFile8K PROPERTIES TIMEOUT 900 ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(layer-structure-file ../LayerStructureFileTest.cpp)
target_link_libraries(layer-structure-file PRIVATE iiSharedCanvas::Native)
target_compile_definitions(layer-structure-file PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/structure-output"
    IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
add_test(NAME NativeCanvas.LayerStructureFile COMMAND layer-structure-file)
add_test(NAME NativeCanvas.LayerStructureFileCost COMMAND layer-structure-file --cost)
add_test(NAME NativeCanvas.LayerStructureFile8K COMMAND layer-structure-file --large)
set_tests_properties(NativeCanvas.LayerStructureFile NativeCanvas.LayerStructureFileCost NativeCanvas.LayerStructureFile8K PROPERTIES
    TIMEOUT 900 ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(document-layer-history ../DocumentLayerHistoryTest.cpp)
target_link_libraries(document-layer-history PRIVATE iiSharedCanvas::Native)
target_compile_definitions(document-layer-history PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/layer-history-output")
add_test(NAME NativeCanvas.DocumentLayerHistory COMMAND document-layer-history)
add_test(NAME NativeCanvas.DocumentLayerHistory8K COMMAND document-layer-history --large)
set_tests_properties(NativeCanvas.DocumentLayerHistory NativeCanvas.DocumentLayerHistory8K PROPERTIES
    TIMEOUT 900 ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(document-asset-layer-history ../DocumentAssetLayerHistoryTest.cpp)
target_link_libraries(document-asset-layer-history PRIVATE iiSharedCanvas::Native)
target_compile_definitions(document-asset-layer-history PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/asset-layer-history-output"
    IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
add_test(NAME NativeCanvas.DocumentAssetLayerHistory COMMAND document-asset-layer-history)
set_tests_properties(NativeCanvas.DocumentAssetLayerHistory PROPERTIES TIMEOUT 900 ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(document-stroke-history ../DocumentStrokeHistoryTest.cpp)
target_link_libraries(document-stroke-history PRIVATE iiSharedCanvas::Native)
target_compile_definitions(document-stroke-history PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/stroke-history-output")
add_test(NAME NativeCanvas.DocumentStrokeHistory COMMAND document-stroke-history)
add_test(NAME NativeCanvas.DocumentStrokeHistory8K COMMAND document-stroke-history --large)
set_tests_properties(NativeCanvas.DocumentStrokeHistory NativeCanvas.DocumentStrokeHistory8K PROPERTIES TIMEOUT 900 ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(document-compound-history ../DocumentCompoundHistoryTest.cpp)
target_link_libraries(document-compound-history PRIVATE iiSharedCanvas::Native)
target_compile_definitions(document-compound-history PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/compound-history-output"
    IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
add_test(NAME NativeCanvas.DocumentCompoundHistory COMMAND document-compound-history)
add_test(NAME NativeCanvas.DocumentCompoundHistory8K COMMAND document-compound-history --large)
set_tests_properties(NativeCanvas.DocumentCompoundHistory NativeCanvas.DocumentCompoundHistory8K PROPERTIES TIMEOUT 900 ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(document-growing-stroke ../DocumentGrowingStrokeTest.cpp)
target_link_libraries(document-growing-stroke PRIVATE iiSharedCanvas::Native)
target_compile_definitions(document-growing-stroke PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/growing-stroke-output"
    IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
add_test(NAME NativeCanvas.DocumentGrowingStroke COMMAND document-growing-stroke)
set_tests_properties(NativeCanvas.DocumentGrowingStroke PROPERTIES TIMEOUT 900 ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

add_executable(audio-preview ../AudioPreviewNativeTest.cpp)
target_link_libraries(audio-preview PRIVATE iiSharedCanvas::Native)
target_compile_definitions(audio-preview PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/audio-preview-output")
add_test(NAME NativeCanvas.AudioPreview COMMAND audio-preview)
add_executable(camera-raw ../CameraRawTest.cpp)
target_link_libraries(camera-raw PRIVATE iiSharedCanvas::Native)
add_test(NAME NativeCanvas.CameraRaw COMMAND camera-raw)
add_executable(video-frame-decoder ../NativeVideoFrameDecoderTest.cpp)
target_link_libraries(video-frame-decoder PRIVATE iiSharedCanvas::Native)
add_test(NAME NativeCanvas.VideoFrameDecoder COMMAND video-frame-decoder)
set_tests_properties(NativeCanvas.VideoFrameDecoder PROPERTIES ENVIRONMENT_MODIFICATION
    "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
add_executable(native-media ../NativeMedia.cpp)
target_link_libraries(native-media PRIVATE iiSharedCanvas::Native)
add_test(NAME NativeCanvas.Media COMMAND native-media "${_iisc_test_work}")
add_executable(iisc-upgrade-test ../NativeIiscUpgradeTest.cpp)
target_link_libraries(iisc-upgrade-test PRIVATE iiSharedCanvas::Native)
if(MINGW)
    target_link_options(iisc-upgrade-test PRIVATE -municode)
endif()
target_compile_definitions(iisc-upgrade-test PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/iisc-upgrade-output")
add_test(NAME NativeCanvas.IiscUpgrade COMMAND iisc-upgrade-test)
find_program(_upgrade_cli iisc-upgrade HINTS "${iiSharedCanvasNative_DIR}/../../../bin" NO_DEFAULT_PATH REQUIRED)
find_program(_import_cli iisc-import HINTS "${iiSharedCanvasNative_DIR}/../../../bin" NO_DEFAULT_PATH REQUIRED)
find_program(_psd_cli iisc-export-psd HINTS "${iiSharedCanvasNative_DIR}/../../../bin" NO_DEFAULT_PATH REQUIRED)
find_program(_timeline_cli iisc-export-timeline HINTS "${iiSharedCanvasNative_DIR}/../../../bin" NO_DEFAULT_PATH REQUIRED)
add_test(NAME NativeCanvas.UpgradeCLI COMMAND ${CMAKE_COMMAND}
    "-DCLI=${_upgrade_cli}" "-DDRIVER=$<TARGET_FILE:iisc-upgrade-test>"
    "-DWORK=${_iisc_test_work}/upgrade-cli-output"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../NativeUpgradeCli.cmake")
add_test(NAME NativeCanvas.LayeredCLI COMMAND ${CMAKE_COMMAND}
    "-DIMPORTER=${_import_cli}" "-DEXPORTER=${_psd_cli}"
    "-DDRIVER=$<TARGET_FILE:iisc-upgrade-test>" "-DWORK=${_iisc_test_work}/layered-cli-output"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../NativeLayeredCli.cmake")
add_test(NAME NativeCanvas.TimelineCLI COMMAND ${CMAKE_COMMAND}
    "-DCLI=${_timeline_cli}" "-DDRIVER=$<TARGET_FILE:iisc-upgrade-test>"
    "-DWORK=${_iisc_test_work}/timeline-cli-output"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../NativeTimelineCli.cmake")
if(Python3_Interpreter_FOUND)
    add_test(NAME NativeCanvas.TimelineWAL COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/../NativeTimelineWalTest.py"
        "${_timeline_cli}" "$<TARGET_FILE:iisc-upgrade-test>"
        "${_iisc_test_work}/timeline-wal-output")
    set_tests_properties(NativeCanvas.TimelineWAL PROPERTIES TIMEOUT 900 ENVIRONMENT_MODIFICATION
        "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
endif()
set_tests_properties(NativeCanvas.AudioPreview NativeCanvas.CameraRaw NativeCanvas.Media NativeCanvas.IiscUpgrade NativeCanvas.UpgradeCLI NativeCanvas.LayeredCLI NativeCanvas.TimelineCLI PROPERTIES
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

# Keep every test-created temporary artifact under this build tree, even when
# the invoking shell's system TMPDIR points outside the workspace.
file(MAKE_DIRECTORY "${_iisc_test_work}/native-tmp")
get_property(_consumer_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_consumer_tests} PROPERTIES
    ENVIRONMENT "TMPDIR=${_iisc_test_work}/native-tmp;TMP=${_iisc_test_work}/native-tmp;TEMP=${_iisc_test_work}/native-tmp")

# Keep process temporary files in the same private resource workspace.
file(MAKE_DIRECTORY "${_iisc_test_work}/consumer-tmp")
get_property(_iisc_consumer_tests DIRECTORY PROPERTY TESTS)
if(_iisc_consumer_tests)
    set_property(TEST ${_iisc_consumer_tests} APPEND PROPERTY ENVIRONMENT
        "TMPDIR=${_iisc_test_work}/consumer-tmp" "TMP=${_iisc_test_work}/consumer-tmp" "TEMP=${_iisc_test_work}/consumer-tmp")
endif()
