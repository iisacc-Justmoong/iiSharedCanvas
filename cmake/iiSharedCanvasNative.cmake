find_package(iiFileProviderNative 0.9 CONFIG REQUIRED)
find_package(iiFileProviderCompatibility 0.9 CONFIG REQUIRED)
find_package(iiImageCodec 0.1 CONFIG REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(iiPaintEngine 0.5.1 CONFIG REQUIRED)
find_package(iiSDKCore 0.18 CONFIG REQUIRED COMPONENTS Contracts)
file(GLOB _native_control_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ControlNet/*.cpp")
add_library(iiSharedCanvasNative STATIC
    src/Document/Document.cpp src/Document/DocumentEditor.cpp src/Document/ArtboardEditor.cpp
    src/Document/LayerAppearance.cpp src/Document/CanvasSampling.cpp
    src/Bitmap/BitmapEditor.cpp src/Bitmap/ChunkedBitmapEditor.cpp
    src/Bitmap/BitmapProcessing.cpp src/Bitmap/RasterProcessingTypes.cpp
    src/Vector/VectorEditor.cpp src/Render/FrameRenderer.cpp
    src/Serialization/IiscCodec.cpp src/Validation/Validation.cpp
    src/Timeline/TimelineEditor.cpp src/Timeline/TimelineProject.cpp
    src/Metadata/StableDiffusionMetadata.cpp src/Metadata/StableDiffusionGenerationParameters.cpp
    src/Native/DocumentFile.cpp src/Native/IiscInput.cpp
    src/Native/Media.cpp src/Native/BitmapCodec.cpp src/Native/VectorPdf.cpp src/Native/PsdParser.cpp src/Native/PsdWriter.cpp src/Native/LayeredDocumentCodec.cpp src/Native/OpenRasterParser.cpp ${_native_control_sources})
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
if(BUILD_TESTING)
    add_executable(iiSharedCanvasNativeDocumentCommitTest tests/DocumentCommitTest.cpp)
    target_link_libraries(iiSharedCanvasNativeDocumentCommitTest PRIVATE iiSharedCanvas::Native)
    add_test(NAME iiSharedCanvas.Native.DocumentCommit COMMAND iiSharedCanvasNativeDocumentCommitTest)
    add_executable(iiSharedCanvasNativeMedia tests/NativeMedia.cpp)
    target_link_libraries(iiSharedCanvasNativeMedia PRIVATE iiSharedCanvas::Native)
    add_test(NAME iiSharedCanvas.Native.Media COMMAND iiSharedCanvasNativeMedia "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(iiSharedCanvasNativeContracts tests/NativeContracts.cpp)
    target_link_libraries(iiSharedCanvasNativeContracts PRIVATE iiSharedCanvas::Native)
    add_test(NAME iiSharedCanvas.Native.Contracts COMMAND iiSharedCanvasNativeContracts "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(iiSharedCanvas.Native.Contracts PROPERTIES TIMEOUT 60)
    foreach(_case ArtboardRegion BitmapEditor BitmapProcessing CanvasMedia DocumentAudio DocumentEditor
            DocumentModel FrameRenderer IiscCodec LayerCompositing MaskStack RasterSampling
            StableDiffusionGenerationParameters StableDiffusionMetadata TimelineEditor TimelineProject Validation VectorEditor)
        add_executable(iiSharedCanvasNative${_case}Test "tests/${_case}Test.cpp")
        target_link_libraries(iiSharedCanvasNative${_case}Test PRIVATE iiSharedCanvas::Native)
        target_compile_definitions(iiSharedCanvasNative${_case}Test PRIVATE IISHAREDCANVAS_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/native-test-output")
        add_test(NAME iiSharedCanvas.Native.${_case} COMMAND iiSharedCanvasNative${_case}Test)
        set_tests_properties(iiSharedCanvas.Native.${_case} PROPERTIES TIMEOUT 60)
    endforeach()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/native-tmp")
    get_property(_native_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${_native_tests} PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}/native-tmp;TMP=${CMAKE_CURRENT_BINARY_DIR}/native-tmp;TEMP=${CMAKE_CURRENT_BINARY_DIR}/native-tmp")
    set_tests_properties(${_native_tests} PROPERTIES ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
endif()
install(TARGETS iiSharedCanvasNative EXPORT iiSharedCanvasNativeTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES src/iiSharedCanvasNative.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas)
install(FILES src/iiSharedCanvasNative.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas RENAME iiSharedCanvasCore.h)
install(FILES src/iiSharedCanvasNative.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas RENAME iiSharedCanvas.h)
set(_native_headers Document/Document.h Document/DocumentEditor.h Document/Attribution.h
    Document/CanvasSampling.h Document/LayerAppearance.h
    Bitmap/BitmapEditor.h Bitmap/ChunkedBitmapEditor.h Bitmap/BitmapProcessing.h Bitmap/RasterProcessingTypes.h
    Vector/VectorEditor.h Render/FrameRenderer.h Serialization/IiscCodec.h
    Timeline/TimelineEditor.h Timeline/TimelineProject.h Validation/Validation.h
    Metadata/StableDiffusionMetadata.h Metadata/StableDiffusionGenerationParameters.h Metadata/PublicationMetadata.h
    File/DocumentFile.h Media/MediaIo.h Bitmap/BitmapCodec.h Layered/LayeredDocumentCodec.h)
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
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/iiSharedCanvas/Export.h" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas/iiSharedCanvas)
install(EXPORT iiSharedCanvasNativeTargets NAMESPACE iiSharedCanvas:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvasNative)
configure_package_config_file(cmake/iiSharedCanvasNativeConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasNativeConfig.cmake" INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvasNative)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasNativeConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasNativeConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/iiSharedCanvasNativeConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSharedCanvasNative)
