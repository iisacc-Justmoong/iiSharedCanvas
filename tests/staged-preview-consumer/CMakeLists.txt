cmake_minimum_required(VERSION 3.31)
project(StagedPreviewConsumer LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/iiSharedCanvasTestWorkDir.cmake")
iisc_configure_test_work_dir(_iisc_test_work "${CMAKE_CURRENT_SOURCE_DIR}/../../resources")
set(CMAKE_DISABLE_FIND_PACKAGE_Qt5 ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Qt6 ON)
find_package(iiSharedCanvasNative 0.35 CONFIG REQUIRED)
if(TARGET Qt6::Core OR TARGET Qt6::Gui OR TARGET Qt5::Core)
    message(FATAL_ERROR "The staged preview consumer imported Qt")
endif()
add_executable(staged-preview ../DocumentStagedPixelPreviewTest.cpp)
target_compile_features(staged-preview PRIVATE cxx_std_23)
target_link_libraries(staged-preview PRIVATE iiSharedCanvas::Native)
target_compile_definitions(staged-preview PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/test-output")
enable_testing()
file(MAKE_DIRECTORY "${_iisc_test_work}/native-tmp")
add_test(NAME NativeCanvas.StagedPixelPreview COMMAND staged-preview)
set_tests_properties(NativeCanvas.StagedPixelPreview PROPERTIES TIMEOUT 900
    ENVIRONMENT "TMPDIR=${_iisc_test_work}/native-tmp;TMP=${_iisc_test_work}/native-tmp;TEMP=${_iisc_test_work}/native-tmp"
    ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")

# Keep process temporary files in the same private resource workspace.
file(MAKE_DIRECTORY "${_iisc_test_work}/consumer-tmp")
get_property(_iisc_consumer_tests DIRECTORY PROPERTY TESTS)
if(_iisc_consumer_tests)
    set_property(TEST ${_iisc_consumer_tests} APPEND PROPERTY ENVIRONMENT
        "TMPDIR=${_iisc_test_work}/consumer-tmp" "TMP=${_iisc_test_work}/consumer-tmp" "TEMP=${_iisc_test_work}/consumer-tmp")
endif()
