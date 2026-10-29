cmake_minimum_required(VERSION 3.31)
project(NativeTestOutputIsolation LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/iiSharedCanvasTestWorkDir.cmake")
iisc_configure_test_work_dir(_iisc_test_work "${CMAKE_CURRENT_SOURCE_DIR}/../../resources")
set(CMAKE_DISABLE_FIND_PACKAGE_Qt5 ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Qt6 ON)
find_package(iiSharedCanvasNative CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(TARGET Qt6::Core OR TARGET Qt5::Core)
    message(FATAL_ERROR "The native output isolation consumer imported Qt")
endif()

enable_testing()
file(MAKE_DIRECTORY "${_iisc_test_work}/tmp")
foreach(_case RasterSampling LayerCompositing MaskStack BitmapHistory BitmapPixelPreview)
    set(_output "${_iisc_test_work}/test-output/${_case}")
    add_executable(isolation-${_case} "../${_case}Test.cpp")
    target_link_libraries(isolation-${_case} PRIVATE iiSharedCanvas::Native)
    target_compile_definitions(isolation-${_case} PRIVATE
        IISHAREDCANVAS_TEST_OUTPUT_DIR="${_output}"
        IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
    add_test(NAME NativeOutputIsolation.${_case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/../NativeTestOutputIsolation.py"
        "$<TARGET_FILE:isolation-${_case}>" "${_output}" "${_case}")
    set_tests_properties(NativeOutputIsolation.${_case} PROPERTIES TIMEOUT 900
        ENVIRONMENT "TMPDIR=${_iisc_test_work}/tmp;TMP=${_iisc_test_work}/tmp;TEMP=${_iisc_test_work}/tmp"
        ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
endforeach()

# Exercise the same failure positions against the installed package without
# sharing the main build's running test executable or its output directory.
set(_growing_output "${_iisc_test_work}/test-output/GrowingStrokeAllocation")
add_executable(isolation-GrowingStrokeAllocation ../DocumentGrowingStrokeAllocationTest.cpp)
target_link_libraries(isolation-GrowingStrokeAllocation PRIVATE iiSharedCanvas::Native)
target_compile_definitions(isolation-GrowingStrokeAllocation PRIVATE
    IISHAREDCANVAS_TEST_OUTPUT_DIR="${_growing_output}"
    IISHAREDCANVAS_TEST_RESOURCE_DIR="${iiSharedCanvasNative_RESOURCE_DIR}")
foreach(_operation Undo Redo)
    string(TOLOWER "${_operation}" _operation_arg)
    foreach(_group Control Lower Upper)
        string(TOLOWER "${_group}" _group_arg)
        add_test(NAME NativeOutputIsolation.GrowingStroke${_operation}${_group}
            COMMAND isolation-GrowingStrokeAllocation --file-only
                --operation=${_operation_arg} --allocation-group=${_group_arg})
        set_tests_properties(NativeOutputIsolation.GrowingStroke${_operation}${_group} PROPERTIES
            TIMEOUT 900
            ENVIRONMENT "TMPDIR=${_iisc_test_work}/tmp;TMP=${_iisc_test_work}/tmp;TEMP=${_iisc_test_work}/tmp"
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
    endforeach()
endforeach()
add_test(NAME NativeOutputIsolation.GrowingStrokeArguments
    COMMAND ${CMAKE_COMMAND} "-DPROGRAM=$<TARGET_FILE:isolation-GrowingStrokeAllocation>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../DocumentGrowingStrokeAllocationArguments.cmake")

# Keep process temporary files in the same private resource workspace.
file(MAKE_DIRECTORY "${_iisc_test_work}/consumer-tmp")
get_property(_iisc_consumer_tests DIRECTORY PROPERTY TESTS)
if(_iisc_consumer_tests)
    set_property(TEST ${_iisc_consumer_tests} APPEND PROPERTY ENVIRONMENT
        "TMPDIR=${_iisc_test_work}/consumer-tmp" "TMP=${_iisc_test_work}/consumer-tmp" "TEMP=${_iisc_test_work}/consumer-tmp")
endif()
