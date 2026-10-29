cmake_minimum_required(VERSION 3.31)
project(InstalledBitmapRegionContract LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/iiSharedCanvasTestWorkDir.cmake")
iisc_configure_test_work_dir(_iisc_test_work "${CMAKE_CURRENT_SOURCE_DIR}/../../resources")
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DISABLE_FIND_PACKAGE_Qt5 ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Qt6 ON)
find_package(iiSharedCanvasNative 0.35 CONFIG REQUIRED)
add_executable(file-regions ../BitmapFileRegionTest.cpp)
target_link_libraries(file-regions PRIVATE iiSharedCanvas::Native)
target_compile_definitions(file-regions PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_iisc_test_work}/output"
    IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
enable_testing()
add_test(NAME NativeCanvas.InstalledFileRegions COMMAND file-regions)
set_tests_properties(NativeCanvas.InstalledFileRegions PROPERTIES TIMEOUT 900)

# Keep process temporary files in the same private resource workspace.
file(MAKE_DIRECTORY "${_iisc_test_work}/consumer-tmp")
get_property(_iisc_consumer_tests DIRECTORY PROPERTY TESTS)
if(_iisc_consumer_tests)
    set_property(TEST ${_iisc_consumer_tests} APPEND PROPERTY ENVIRONMENT
        "TMPDIR=${_iisc_test_work}/consumer-tmp" "TMP=${_iisc_test_work}/consumer-tmp" "TEMP=${_iisc_test_work}/consumer-tmp")
endif()
