cmake_minimum_required(VERSION 3.31)
project(NativeCanvasVideoConsumer LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/iiSharedCanvasTestWorkDir.cmake")
iisc_configure_test_work_dir(_iisc_test_work "${CMAKE_CURRENT_SOURCE_DIR}/../../resources")

set(CMAKE_DISABLE_FIND_PACKAGE_Qt5 ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Qt6 ON)
find_package(iiSharedCanvasNative CONFIG REQUIRED)
find_package(iiVisualCodec 0.4.2 CONFIG REQUIRED COMPONENTS Video)
if(TARGET Qt6::Core OR TARGET Qt6::Gui OR TARGET Qt5::Core)
    message(FATAL_ERROR "The installed video consumer imported Qt")
endif()
get_filename_component(_native_share "${iiSharedCanvasNative_RESOURCE_DIR}/../.." ABSOLUTE)
set(_native_docs "${_native_share}/iiSharedCanvasNative")
file(READ "${_native_docs}/NATIVE.md" _native_guide)
string(REGEX MATCHALL "\\([^)]*\\.md\\)" _guide_links "${_native_guide}")
foreach(_link IN LISTS _guide_links)
    string(REGEX REPLACE "^\\((.*)\\)$" "\\1" _relative_guide "${_link}")
    if(NOT EXISTS "${_native_docs}/${_relative_guide}")
        message(FATAL_ERROR "The installed native guide links to missing ${_relative_guide}")
    endif()
endforeach()

add_executable(frame-decoder ../NativeVideoFrameDecoderTest.cpp)
target_link_libraries(frame-decoder PRIVATE iiSharedCanvas::Native)
add_executable(visual-video-decoder ../NativeVisualVideoDecoderTest.cpp
    ../../example/VisualCodecVideoDecoder.cpp)
target_include_directories(visual-video-decoder PRIVATE ../../example)
target_link_libraries(visual-video-decoder PRIVATE iiSharedCanvas::Native iiVisualCodec::Video)
target_compile_definitions(visual-video-decoder PRIVATE
    IISHAREDCANVAS_VISUAL_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../iiVisualCodec/resources/video")
add_executable(frame-encoder ../NativeVideoFrameEncoderTest.cpp)
target_link_libraries(frame-encoder PRIVATE iiSharedCanvas::Native)
add_executable(visual-video-encoder ../NativeVisualVideoEncoderTest.cpp
    ../../example/VisualCodecVideoEncoder.cpp ../../example/VisualCodecVideoDecoder.cpp)
target_include_directories(visual-video-encoder PRIVATE ../../example)
target_link_libraries(visual-video-encoder PRIVATE iiSharedCanvas::Native iiVisualCodec::Video)

enable_testing()
add_test(NAME NativeVideoConsumer.FrameDecoder COMMAND frame-decoder)
add_test(NAME NativeVideoConsumer.VisualCodec COMMAND visual-video-decoder)
add_test(NAME NativeVideoConsumer.FrameEncoder COMMAND frame-encoder)
add_test(NAME NativeVideoConsumer.VisualCodecEncoder COMMAND visual-video-encoder)
file(MAKE_DIRECTORY "${_iisc_test_work}/tmp")
set_tests_properties(NativeVideoConsumer.FrameDecoder NativeVideoConsumer.VisualCodec NativeVideoConsumer.FrameEncoder NativeVideoConsumer.VisualCodecEncoder PROPERTIES
    ENVIRONMENT "TMPDIR=${_iisc_test_work}/tmp;TMP=${_iisc_test_work}/tmp;TEMP=${_iisc_test_work}/tmp"
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

# Keep process temporary files in the same private resource workspace.
file(MAKE_DIRECTORY "${_iisc_test_work}/consumer-tmp")
get_property(_iisc_consumer_tests DIRECTORY PROPERTY TESTS)
if(_iisc_consumer_tests)
    set_property(TEST ${_iisc_consumer_tests} APPEND PROPERTY ENVIRONMENT
        "TMPDIR=${_iisc_test_work}/consumer-tmp" "TMP=${_iisc_test_work}/consumer-tmp" "TEMP=${_iisc_test_work}/consumer-tmp")
endif()
