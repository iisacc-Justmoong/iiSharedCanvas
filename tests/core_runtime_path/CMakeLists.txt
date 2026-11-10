cmake_minimum_required(VERSION 3.31)
project(CanvasCoreRuntimePath LANGUAGES CXX)

if(NOT CANVAS_PREFIX)
    message(FATAL_ERROR "CANVAS_PREFIX is required")
endif()

# Reproduce the case where the toolchain considers the SDK's library
# directory implicit and therefore does not generate an automatic rpath.
list(APPEND CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES "${CANVAS_PREFIX}/lib")
find_package(iiSharedCanvas CONFIG REQUIRED COMPONENTS Core)
get_target_property(_options iiSharedCanvas::Core INTERFACE_LINK_OPTIONS)
list(FIND _options "LINKER:-rpath,${CANVAS_PREFIX}/lib" _index)
if(_index EQUAL -1)
    message(FATAL_ERROR "The imported Core target does not publish its implicit runtime directory")
endif()
