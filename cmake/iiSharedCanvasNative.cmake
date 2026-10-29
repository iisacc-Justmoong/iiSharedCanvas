find_package(iiFileProviderNative 0.11.1 CONFIG REQUIRED)
find_package(iiFileProviderCompatibility 0.9 CONFIG REQUIRED)
find_package(iiImageCodec 0.1 CONFIG REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(iiPaintEngine 0.5.4 CONFIG REQUIRED)
find_package(iiSDKCore 0.18 CONFIG REQUIRED COMPONENTS Contracts)
file(GLOB _native_control_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ControlNet/*.cpp")
add_library(iiSharedCanvasNative STATIC
    src/Audio/AudioCodec.cpp src/Camera/CameraRaw.cpp
    src/Document/Document.cpp src/Document/DocumentEditor.cpp src/Document/DocumentHistory.cpp src/Document/AssetHistorySnapshot_p.cpp src/Document/ArtboardEditor.cpp
    src/Document/LayerAppearance.cpp src/Document/CanvasSampling.cpp
    src/Bitmap/BitmapEditor.cpp src/Bitmap/ChunkedBitmapEditor.cpp src/Bitmap/BitmapEditJournal_p.cpp
    src/Bitmap/BitmapProcessing.cpp src/Bitmap/RasterProcessingTypes.cpp
    src/Vector/VectorEditor.cpp src/Vector/SvgExport.cpp src/Vector/PdfExport.cpp src/Render/FrameRenderer.cpp src/Render/DocumentPreview.cpp src/Render/AsyncFrameRenderQueue.cpp
    src/Serialization/IiscCodec.cpp src/Validation/Validation.cpp
    src/Timeline/TimelineEditor.cpp src/Timeline/TimelineProject.cpp src/Video/VideoFrameDecoder.cpp src/Video/VideoFrameEncoder.cpp
    src/Metadata/StableDiffusionMetadata.cpp src/Metadata/StableDiffusionGenerationParameters.cpp
    src/Native/DocumentFile.cpp src/Native/WorkingFileTiles_p.cpp src/Native/IiscInput.cpp src/Native/IiscUpgrade.cpp
    src/Native/Media.cpp src/Native/BitmapCodec.cpp src/Native/VectorPdf.cpp src/Native/TimelineXmlWriter.cpp src/Native/TimelineInterchange.cpp src/Native/PsdParser.cpp src/Native/PsdWriter.cpp src/Native/LayeredDocumentCodec.cpp src/Native/OpenRasterParser.cpp ${_native_control_sources})
add_library(iiSharedCanvas::Native ALIAS iiSharedCanvasNative)
add_library(iiSharedCanvas::Core ALIAS iiSharedCanvasNative)
target_compile_features(iiSharedCanvasNative PUBLIC cxx_std_23)
target_compile_definitions(iiSharedCanvasNative PUBLIC IISHAREDCANVAS_NATIVE_API=1 IISHAREDCANVAS_STATIC_DEFINE)
target_link_libraries(iiSharedCanvasNative PUBLIC iiPaintEngine::iiPaintEngine iiFileProvider::Native iiSDKCore::Contracts iiFileProvider::Compatibility iiImageCodec::iiImageCodec LibXml2::LibXml2 ZLIB::ZLIB)
target_include_directories(iiSharedCanvasNative PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas>)
set_target_properties(iiSharedCanvasNative PROPERTIES EXPORT_NAME Native POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF INTERFACE_IISHAREDCANVAS_ABI native)
set_property(TARGET iiSharedCanvasNative APPEND PROPERTY COMPATIBLE_INTERFACE_STRING IISHAREDCANVAS_ABI)
generate_export_header(iiSharedCanvasNative BASE_NAME iiSharedCanvas
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iiSharedCanvas/Export.h")
option(IISHAREDCANVAS_BUILD_NATIVE_CLI "Build the native JSON working-file CLI" ON)
if(IISHAREDCANVAS_BUILD_NATIVE_CLI)
    add_executable(iiSharedCanvasWorkingCli src/Native/WorkingFileCli.cpp)
    target_link_libraries(iiSharedCanvasWorkingCli PRIVATE iiSharedCanvas::Native)
    set_target_properties(iiSharedCanvasWorkingCli PROPERTIES OUTPUT_NAME iisc-working CXX_EXTENSIONS OFF INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(MINGW)
        target_link_options(iiSharedCanvasWorkingCli PRIVATE -municode)
    endif()
    install(TARGETS iiSharedCanvasWorkingCli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    add_executable(iiSharedCanvasNativeUpgradeCli tools/iisc-upgrade.cpp)
    target_link_libraries(iiSharedCanvasNativeUpgradeCli PRIVATE iiSharedCanvas::Native)
    set_target_properties(iiSharedCanvasNativeUpgradeCli PROPERTIES OUTPUT_NAME iisc-upgrade CXX_EXTENSIONS OFF INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(MINGW)
        target_link_options(iiSharedCanvasNativeUpgradeCli PRIVATE -municode)
    endif()
    install(TARGETS iiSharedCanvasNativeUpgradeCli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    add_executable(iiSharedCanvasNativeImportCli src/Native/LayeredImportCli.cpp)
    add_executable(iiSharedCanvasNativePsdExportCli src/Native/PsdExportCli.cpp)
    add_executable(iiSharedCanvasNativeTimelineExportCli src/Native/TimelineExportCli.cpp)
    foreach(_cli IN ITEMS iiSharedCanvasNativeImportCli iiSharedCanvasNativePsdExportCli iiSharedCanvasNativeTimelineExportCli)
        target_link_libraries(${_cli} PRIVATE iiSharedCanvas::Native)
        set_target_properties(${_cli} PROPERTIES CXX_EXTENSIONS OFF INSTALL_RPATH_USE_LINK_PATH TRUE)
        if(MINGW)
            target_link_options(${_cli} PRIVATE -municode)
        endif()
    endforeach()
    set_target_properties(iiSharedCanvasNativeImportCli PROPERTIES OUTPUT_NAME iisc-import)
    set_target_properties(iiSharedCanvasNativePsdExportCli PROPERTIES OUTPUT_NAME iisc-export-psd)
    set_target_properties(iiSharedCanvasNativeTimelineExportCli PROPERTIES OUTPUT_NAME iisc-export-timeline)
    install(TARGETS iiSharedCanvasNativeImportCli iiSharedCanvasNativePsdExportCli iiSharedCanvasNativeTimelineExportCli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(BUILD_TESTING)
    # On macOS, count actual SQLite scan work as well as verifying the edited
    # asset, unrelated pixels, undo/redo and the persisted snapshot.
    if(APPLE)
        find_package(SQLite3 3.27 REQUIRED)
        if(NOT TARGET SQLite3::SQLite3) # CMake 3.31 through 4.2
            add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
        endif()
        add_library(iiSharedCanvasNativeSQLiteScanProbe SHARED tests/NativeSQLiteScanProbe.cpp)
        target_compile_features(iiSharedCanvasNativeSQLiteScanProbe PRIVATE cxx_std_23)
        target_link_libraries(iiSharedCanvasNativeSQLiteScanProbe PRIVATE SQLite3::SQLite3)
        add_executable(iiSharedCanvasNativeAssetTileScanTest tests/NativeAssetTileScanTest.cpp)
        target_link_libraries(iiSharedCanvasNativeAssetTileScanTest PRIVATE iiSharedCanvas::Native SQLite3::SQLite3)
        file(MAKE_DIRECTORY "${_iisc_test_work}/native-asset-tile-scan-tmp")
        add_test(NAME iiSharedCanvas.Native.AssetTileScanControl COMMAND ${CMAKE_COMMAND} -E env
            "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:iiSharedCanvasNativeSQLiteScanProbe>"
            $<TARGET_FILE:iiSharedCanvasNativeAssetTileScanTest> --control)
        add_test(NAME iiSharedCanvas.Native.AssetTileScan COMMAND ${CMAKE_COMMAND} -E env
            "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:iiSharedCanvasNativeSQLiteScanProbe>"
            $<TARGET_FILE:iiSharedCanvasNativeAssetTileScanTest>
            "${_iisc_test_work}/native-asset-tile-scan-output")
        set_tests_properties(iiSharedCanvas.Native.AssetTileScanControl iiSharedCanvas.Native.AssetTileScan PROPERTIES
            TIMEOUT 900
            ENVIRONMENT "TMPDIR=${_iisc_test_work}/native-asset-tile-scan-tmp;TMP=${_iisc_test_work}/native-asset-tile-scan-tmp;TEMP=${_iisc_test_work}/native-asset-tile-scan-tmp"
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
    endif()
    # The application-level test composes the two SDK interfaces; the native
    # canvas library itself only needs the core Contracts component.
    find_package(iiSDKCore 0.18 CONFIG REQUIRED COMPONENTS Runtime)
    add_executable(iiSharedCanvasNativeAsyncRuntimeTest tests/NativeAsyncRuntimeTest.cpp)
    target_link_libraries(iiSharedCanvasNativeAsyncRuntimeTest PRIVATE iiSharedCanvas::Native iiSDKCore::Runtime)
    add_test(NAME iiSharedCanvas.Native.AsyncRuntime COMMAND iiSharedCanvasNativeAsyncRuntimeTest)
    set_tests_properties(iiSharedCanvas.Native.AsyncRuntime PROPERTIES TIMEOUT 120)
    add_executable(iiSharedCanvasNativeDocumentCommitTest tests/DocumentCommitTest.cpp)
    target_link_libraries(iiSharedCanvasNativeDocumentCommitTest PRIVATE iiSharedCanvas::Native)
    add_test(NAME iiSharedCanvas.Native.DocumentCommit COMMAND iiSharedCanvasNativeDocumentCommitTest)
    add_executable(iiSharedCanvasNativeMedia tests/NativeMedia.cpp)
    target_link_libraries(iiSharedCanvasNativeMedia PRIVATE iiSharedCanvas::Native)
    add_test(NAME iiSharedCanvas.Native.Media COMMAND iiSharedCanvasNativeMedia "${_iisc_test_work}")
    add_executable(iiSharedCanvasNativeContracts tests/NativeContracts.cpp)
    target_link_libraries(iiSharedCanvasNativeContracts PRIVATE iiSharedCanvas::Native)
    add_test(NAME iiSharedCanvas.Native.Contracts COMMAND iiSharedCanvasNativeContracts "${_iisc_test_work}")
    set_tests_properties(iiSharedCanvas.Native.Contracts PROPERTIES TIMEOUT 60)
    find_package(iiJson 0.1 CONFIG REQUIRED)
    find_package(Threads REQUIRED)
    foreach(_case ArtboardRegion AudioPreviewNative CameraRaw NativeIiscUpgrade NativeAsyncFrameRender NativeSvgExport NativePdfExport NativeTimelineXmlWriter NativeTimelineInterchange NativeVideoFrameDecoder NativeVideoFrameEncoder BitmapEditor BitmapInput BitmapWetBrush BitmapClock BitmapHistory BitmapHistoryAllocation BitmapPixelPreview BitmapFileRegion WorkingFileTiles WorkingSnapshotAllocation LayerFileEdit DocumentHistory DocumentHistoryAllocation DocumentHistoryPreview DocumentHistoryPreviewAllocation DocumentStagedPixelPreview DocumentStagedPixelPreviewAllocation DocumentStagedPixelPreview8K BitmapProcessing CanvasMedia DocumentAudio DocumentEditor
            DocumentModel DocumentLayerHistory DocumentAssetLayerHistory DocumentAssetLayerHistoryAllocation DocumentStrokeHistory DocumentStrokeHistoryAllocation DocumentGrowingStroke DocumentGrowingStrokeAllocation DocumentGrowingStroke8K DocumentCompoundHistory DocumentCompoundHistoryAllocation FrameRenderer FrameOverlayRender FrameOverlayRender8K FrameOverlayRenderManyChunks FrameOverlayFile IiscCodec LayerCompositing MaskStack MaskRegion MaskRegionFile8K LayerStructureFile RasterSampling
            StableDiffusionGenerationParameters StableDiffusionMetadata TimelineEditor TimelineProject Validation VectorEditor)
        add_executable(iiSharedCanvasNative${_case}Test "tests/${_case}Test.cpp")
        target_link_libraries(iiSharedCanvasNative${_case}Test PRIVATE iiSharedCanvas::Native)
        target_compile_definitions(iiSharedCanvasNative${_case}Test PRIVATE
            IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/native-test-output"
            IISHAREDCANVAS_TEST_RESOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources")
        if(_case STREQUAL "DocumentStagedPixelPreviewAllocation" OR _case STREQUAL "DocumentStrokeHistoryAllocation"
                OR _case STREQUAL "DocumentGrowingStrokeAllocation" OR _case STREQUAL "DocumentCompoundHistoryAllocation")
            add_test(NAME iiSharedCanvas.Native.${_case} COMMAND iiSharedCanvasNative${_case}Test --memory-only)
        else()
            add_test(NAME iiSharedCanvas.Native.${_case} COMMAND iiSharedCanvasNative${_case}Test)
        endif()
        set_tests_properties(iiSharedCanvas.Native.${_case} PROPERTIES TIMEOUT 60)
    endforeach()
    target_link_libraries(iiSharedCanvasNativeNativeTimelineInterchangeTest PRIVATE iiJson::iiJson Threads::Threads)
    set_tests_properties(iiSharedCanvas.Native.NativeTimelineInterchange PROPERTIES TIMEOUT 300)
    add_test(NAME iiSharedCanvas.Native.DocumentStagedPixelPreviewFileAllocation
        COMMAND iiSharedCanvasNativeDocumentStagedPixelPreviewAllocationTest --file-only)
    set_tests_properties(iiSharedCanvas.Native.DocumentStagedPixelPreviewFileAllocation PROPERTIES TIMEOUT 900)
    foreach(_operation Begin Continue End Cancel)
        string(TOLOWER "${_operation}" _operation_arg)
        add_test(NAME iiSharedCanvas.Native.DocumentStrokeHistoryAllocationFile${_operation}
            COMMAND iiSharedCanvasNativeDocumentStrokeHistoryAllocationTest --file-only --operation=${_operation_arg})
        set_tests_properties(iiSharedCanvas.Native.DocumentStrokeHistoryAllocationFile${_operation} PROPERTIES TIMEOUT 900)
    endforeach()
    foreach(_operation Begin Continue End Cancel)
        string(TOLOWER "${_operation}" _operation_arg)
        add_test(NAME iiSharedCanvas.Native.DocumentGrowingStrokeAllocationFile${_operation}
            COMMAND iiSharedCanvasNativeDocumentGrowingStrokeAllocationTest --file-only --operation=${_operation_arg})
        set_tests_properties(iiSharedCanvas.Native.DocumentGrowingStrokeAllocationFile${_operation} PROPERTIES TIMEOUT 900)
    endforeach()
    foreach(_operation Undo Redo)
        string(TOLOWER "${_operation}" _operation_arg)
        foreach(_group Control Lower Upper)
            string(TOLOWER "${_group}" _group_arg)
            add_test(NAME iiSharedCanvas.Native.DocumentGrowingStrokeAllocationFile${_operation}${_group}
                COMMAND iiSharedCanvasNativeDocumentGrowingStrokeAllocationTest
                    --file-only --operation=${_operation_arg} --allocation-group=${_group_arg})
            set_tests_properties(iiSharedCanvas.Native.DocumentGrowingStrokeAllocationFile${_operation}${_group}
                PROPERTIES TIMEOUT 900)
        endforeach()
    endforeach()
    add_test(NAME iiSharedCanvas.Native.DocumentGrowingStrokeAllocationArguments
        COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:iiSharedCanvasNativeDocumentGrowingStrokeAllocationTest>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/DocumentGrowingStrokeAllocationArguments.cmake")
    foreach(_operation Timing TimingUndo TimingRedo Remove RemoveUndo RemoveRedo)
        string(TOLOWER "${_operation}" _operation_arg)
        add_test(NAME iiSharedCanvas.Native.DocumentCompoundHistoryAllocationFile${_operation}
            COMMAND iiSharedCanvasNativeDocumentCompoundHistoryAllocationTest --file-only --operation=${_operation_arg})
        set_tests_properties(iiSharedCanvas.Native.DocumentCompoundHistoryAllocationFile${_operation} PROPERTIES TIMEOUT 900)
    endforeach()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME iiSharedCanvas.Native.TimelineOracle COMMAND ${CMAKE_COMMAND}
            "-DDRIVER=$<TARGET_FILE:iiSharedCanvasNativeNativeTimelineInterchangeTest>"
            "-DPYTHON=${Python3_EXECUTABLE}"
            "-DORACLE=${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_timeline_interchange.py"
            "-DWORK=${_iisc_test_work}/native-timeline-oracle-test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativeTimelineOracle.cmake")
        set_tests_properties(iiSharedCanvas.Native.TimelineOracle PROPERTIES TIMEOUT 300)
    endif()
    find_program(IISHAREDCANVAS_PDFINFO_EXECUTABLE pdfinfo)
    find_program(IISHAREDCANVAS_PDFTOPPM_EXECUTABLE pdftoppm)
    if(Python3_Interpreter_FOUND AND IISHAREDCANVAS_PDFINFO_EXECUTABLE AND IISHAREDCANVAS_PDFTOPPM_EXECUTABLE)
        add_test(NAME iiSharedCanvas.Native.PdfExternalCheck COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativePdfExternalCheck.py"
            "$<TARGET_FILE:iiSharedCanvasNativeNativePdfExportTest>"
            "${IISHAREDCANVAS_PDFINFO_EXECUTABLE}" "${IISHAREDCANVAS_PDFTOPPM_EXECUTABLE}"
            "${_iisc_test_work}/native-test-output")
        set_tests_properties(iiSharedCanvas.Native.PdfExternalCheck PROPERTIES TIMEOUT 300)
    endif()
    if(MINGW)
        target_link_options(iiSharedCanvasNativeNativeIiscUpgradeTest PRIVATE -municode)
        target_link_options(iiSharedCanvasNativeNativeTimelineInterchangeTest PRIVATE -municode)
    endif()
    # Durable-file tests perform synchronous writes on the selected volume;
    # their deadline is a hang guard, not a storage latency requirement.
    foreach(_file_case AudioPreviewNative NativeIiscUpgrade BitmapInput BitmapHistory BitmapPixelPreview WorkingSnapshotAllocation)
        set_tests_properties(iiSharedCanvas.Native.${_file_case} PROPERTIES TIMEOUT 300)
    endforeach()
    set_tests_properties(iiSharedCanvas.Native.BitmapWetBrush PROPERTIES TIMEOUT 180)
    set_tests_properties(iiSharedCanvas.Native.BitmapClock PROPERTIES TIMEOUT 180)
    set_tests_properties(iiSharedCanvas.Native.WorkingFileTiles PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.BitmapFileRegion PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.LayerFileEdit PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentHistory PROPERTIES TIMEOUT 180)
    set_tests_properties(iiSharedCanvas.Native.DocumentHistoryAllocation PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentHistoryPreview PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentHistoryPreviewAllocation PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentStagedPixelPreview PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentStagedPixelPreviewAllocation PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentStagedPixelPreview8K PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.FrameOverlayRender8K PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.FrameOverlayFile PROPERTIES TIMEOUT 300)
    add_test(NAME iiSharedCanvas.Native.DocumentHistoryPreview8K COMMAND iiSharedCanvasNativeDocumentHistoryPreviewTest --large)
    set_tests_properties(iiSharedCanvas.Native.DocumentHistoryPreview8K PROPERTIES TIMEOUT 180)
    add_test(NAME iiSharedCanvas.Native.MaskRegion8K COMMAND iiSharedCanvasNativeMaskRegionTest --large)
    set_tests_properties(iiSharedCanvas.Native.MaskRegion8K PROPERTIES TIMEOUT 300)
    add_test(NAME iiSharedCanvas.Native.MaskRegionCost COMMAND iiSharedCanvasNativeMaskRegionTest --cost)
    set_tests_properties(iiSharedCanvas.Native.MaskRegionCost PROPERTIES TIMEOUT 180)
    set_tests_properties(iiSharedCanvas.Native.MaskRegionFile8K PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.LayerStructureFile PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentLayerHistory PROPERTIES TIMEOUT 300)
    set_tests_properties(iiSharedCanvas.Native.DocumentAssetLayerHistory PROPERTIES TIMEOUT 300)
    set_tests_properties(iiSharedCanvas.Native.DocumentAssetLayerHistoryAllocation PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentStrokeHistory PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentGrowingStroke PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentGrowingStrokeAllocation PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentGrowingStroke8K PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentStrokeHistoryAllocation PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentCompoundHistory PROPERTIES TIMEOUT 900)
    set_tests_properties(iiSharedCanvas.Native.DocumentCompoundHistoryAllocation PROPERTIES TIMEOUT 900)
    add_test(NAME iiSharedCanvas.Native.DocumentCompoundHistory8K COMMAND iiSharedCanvasNativeDocumentCompoundHistoryTest --large)
    set_tests_properties(iiSharedCanvas.Native.DocumentCompoundHistory8K PROPERTIES TIMEOUT 900)
    add_test(NAME iiSharedCanvas.Native.DocumentStrokeHistory8K COMMAND iiSharedCanvasNativeDocumentStrokeHistoryTest --large)
    set_tests_properties(iiSharedCanvas.Native.DocumentStrokeHistory8K PROPERTIES TIMEOUT 900)
    add_test(NAME iiSharedCanvas.Native.DocumentAssetLayerHistory8K COMMAND iiSharedCanvasNativeDocumentAssetLayerHistoryTest --large)
    set_tests_properties(iiSharedCanvas.Native.DocumentAssetLayerHistory8K PROPERTIES TIMEOUT 900)
    add_test(NAME iiSharedCanvas.Native.DocumentLayerHistory8K COMMAND iiSharedCanvasNativeDocumentLayerHistoryTest --large)
    set_tests_properties(iiSharedCanvas.Native.DocumentLayerHistory8K PROPERTIES TIMEOUT 900)
    add_test(NAME iiSharedCanvas.Native.LayerStructureFileCost COMMAND iiSharedCanvasNativeLayerStructureFileTest --cost)
    set_tests_properties(iiSharedCanvas.Native.LayerStructureFileCost PROPERTIES TIMEOUT 300)
    add_test(NAME iiSharedCanvas.Native.LayerStructureFile8K COMMAND iiSharedCanvasNativeLayerStructureFileTest --large)
    set_tests_properties(iiSharedCanvas.Native.LayerStructureFile8K PROPERTIES TIMEOUT 900)
    add_test(NAME iiSharedCanvas.Native.DocumentHistory8K COMMAND iiSharedCanvasNativeDocumentHistoryTest --large)
    set_tests_properties(iiSharedCanvas.Native.DocumentHistory8K PROPERTIES TIMEOUT 180)
    add_test(NAME iiSharedCanvas.Native.BitmapHistory8K COMMAND iiSharedCanvasNativeBitmapHistoryTest --large)
    set_tests_properties(iiSharedCanvas.Native.BitmapHistory8K PROPERTIES TIMEOUT 180)
    add_test(NAME iiSharedCanvas.Native.BitmapPixelPreview8K COMMAND iiSharedCanvasNativeBitmapPixelPreviewTest --large)
    set_tests_properties(iiSharedCanvas.Native.BitmapPixelPreview8K PROPERTIES TIMEOUT 180)
    if(IISHAREDCANVAS_BUILD_NATIVE_CLI)
        add_test(NAME iiSharedCanvas.Native.TimelineCLI COMMAND ${CMAKE_COMMAND}
            "-DCLI=$<TARGET_FILE:iiSharedCanvasNativeTimelineExportCli>"
            "-DDRIVER=$<TARGET_FILE:iiSharedCanvasNativeNativeIiscUpgradeTest>"
            "-DWORK=${_iisc_test_work}/native-timeline-cli-test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativeTimelineCli.cmake")
        set_tests_properties(iiSharedCanvas.Native.TimelineCLI PROPERTIES TIMEOUT 300)
        if(Python3_Interpreter_FOUND)
            add_test(NAME iiSharedCanvas.Native.TimelineWAL COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativeTimelineWalTest.py"
                "$<TARGET_FILE:iiSharedCanvasNativeTimelineExportCli>"
                "$<TARGET_FILE:iiSharedCanvasNativeNativeIiscUpgradeTest>"
                "${_iisc_test_work}/native-timeline-wal-test")
            set_tests_properties(iiSharedCanvas.Native.TimelineWAL PROPERTIES TIMEOUT 900)
        endif()
        add_test(NAME iiSharedCanvas.Native.LayeredCLI COMMAND ${CMAKE_COMMAND}
            "-DIMPORTER=$<TARGET_FILE:iiSharedCanvasNativeImportCli>"
            "-DEXPORTER=$<TARGET_FILE:iiSharedCanvasNativePsdExportCli>"
            "-DDRIVER=$<TARGET_FILE:iiSharedCanvasNativeNativeIiscUpgradeTest>"
            "-DWORK=${_iisc_test_work}/native-layered-cli-test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativeLayeredCli.cmake")
        set_tests_properties(iiSharedCanvas.Native.LayeredCLI PROPERTIES TIMEOUT 300)
        add_test(NAME iiSharedCanvas.Native.UpgradeCLI COMMAND ${CMAKE_COMMAND}
            "-DCLI=$<TARGET_FILE:iiSharedCanvasNativeUpgradeCli>"
            "-DDRIVER=$<TARGET_FILE:iiSharedCanvasNativeNativeIiscUpgradeTest>"
            "-DWORK=${_iisc_test_work}/native-upgrade-cli-test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativeUpgradeCli.cmake")
        set_tests_properties(iiSharedCanvas.Native.UpgradeCLI PROPERTIES TIMEOUT 180)
        add_test(NAME iiSharedCanvas.Native.WorkingFileCLI COMMAND ${CMAKE_COMMAND}
            "-DCLI=$<TARGET_FILE:iiSharedCanvasWorkingCli>"
            "-DDRIVER=$<TARGET_FILE:iiSharedCanvasNativeWorkingFileTilesTest>"
            "-DWORK=${_iisc_test_work}/native-cli-test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativeWorkingFileCli.cmake")
        set_tests_properties(iiSharedCanvas.Native.WorkingFileCLI PROPERTIES TIMEOUT 180)
    endif()
    file(MAKE_DIRECTORY "${_iisc_test_work}/native-tmp")
    get_property(_native_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${_native_tests} PROPERTIES ENVIRONMENT "TMPDIR=${_iisc_test_work}/native-tmp;TMP=${_iisc_test_work}/native-tmp;TEMP=${_iisc_test_work}/native-tmp")
    set_tests_properties(${_native_tests} PROPERTIES ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
endif()
install(TARGETS iiSharedCanvasNative EXPORT iiSharedCanvasNativeTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES src/iiSharedCanvasNative.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas)
install(FILES src/iiSharedCanvasNative.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas RENAME iiSharedCanvasCore.h)
install(FILES src/iiSharedCanvasNative.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas RENAME iiSharedCanvas.h)
set(_native_headers Audio/AudioCodec.h Camera/CameraRaw.h Document/Document.h Document/DocumentEditor.h Document/DocumentHistory.h Document/Attribution.h
    Document/CanvasSampling.h Document/LayerAppearance.h
    Bitmap/BitmapEdit.h Bitmap/BitmapEditor.h Bitmap/ChunkedBitmapEditor.h Bitmap/BitmapProcessing.h Bitmap/RasterProcessingTypes.h
    Vector/VectorEditor.h Vector/SvgExport.h Vector/PdfExport.h Render/FrameRenderer.h Render/AsyncFrameRenderQueue.h Render/DocumentPreview.h Serialization/IiscCodec.h
    Timeline/TimelineEditor.h Timeline/TimelineProject.h Timeline/TimelineInterchange.h Video/VideoFrameDecoder.h Video/VideoFrameEncoder.h Validation/Validation.h
    Metadata/StableDiffusionMetadata.h Metadata/StableDiffusionGenerationParameters.h Metadata/PublicationMetadata.h
    File/DocumentFile.h File/IiscUpgrade.h Media/MediaIo.h Bitmap/BitmapCodec.h Layered/LayeredDocumentCodec.h)
file(GLOB _native_control_headers RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/src/ControlNet/*.h")
list(APPEND _native_headers ${_native_control_headers})
foreach(_header IN LISTS _native_headers)
    get_filename_component(_directory "${_header}" DIRECTORY)
    install(FILES "src/${_header}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas/${_directory}")
endforeach()
if(BUILD_TESTING)
    set(_header_sources)
    foreach(_header IN LISTS _native_headers)
        string(MAKE_C_IDENTIFIER "${_header}" _name)
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-header-checks/${_name}.cpp" CONTENT "#include <${_header}>\n")
        list(APPEND _header_sources "${CMAKE_CURRENT_BINARY_DIR}/native-header-checks/${_name}.cpp")
    endforeach()
    add_library(iiSharedCanvasNativeHeaders OBJECT ${_header_sources})
    target_link_libraries(iiSharedCanvasNativeHeaders PRIVATE iiSharedCanvas::Native)
endif()
install(FILES docs/NATIVE.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES
    docs/TIMELINE_INTERCHANGE.md docs/TIMELINE_INTERCHANGE.agent.json
    docs/VIDEO_FRAME_DECODER.md docs/VIDEO_FRAME_DECODER.agent.json
    docs/VIDEO_FRAME_ENCODER.md docs/VIDEO_FRAME_ENCODER.agent.json
    docs/IISC_UPGRADE_CLI.md docs/IISC_UPGRADE_CLI.agent.json
    docs/LAYERED_IMPORT_CLI.md docs/LAYERED_IMPORT_CLI.agent.json
    docs/PSD_EXPORT_CLI.md docs/PSD_EXPORT_CLI.agent.json
    docs/FRAME_OVERLAY.md docs/FRAME_OVERLAY.agent.json
    docs/LAYER_FILE_EDITS.md docs/LAYER_FILE_EDITS.agent.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES docs/BITMAP_INPUT.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES docs/BITMAP_WET_BRUSH.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES docs/BITMAP_CLOCK.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES docs/BITMAP_HISTORY.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES docs/DOCUMENT_HISTORY.md docs/DOCUMENT_HISTORY.agent.json DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES docs/MASK_REGIONS.md docs/MASK_REGIONS.agent.json DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES docs/PIXEL_PREVIEW.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES docs/BITMAP_FILE_REGIONS.md docs/BITMAP_FILE_REGIONS.agent.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(FILES docs/WORKING_FILE_TILES.md docs/WORKING_FILE_TILES.agent.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative/docs
    FILES_MATCHING PATTERN "*.md" PATTERN "*.json")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/iiSharedCanvas/Export.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas/iiSharedCanvas)
install(EXPORT iiSharedCanvasNativeTargets NAMESPACE iiSharedCanvas:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvasNative)
configure_package_config_file(cmake/iiSharedCanvasNativeConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasNativeConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvasNative
    PATH_VARS CMAKE_INSTALL_DATADIR)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasNativeConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasNativeConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasNativeConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvasNative)
