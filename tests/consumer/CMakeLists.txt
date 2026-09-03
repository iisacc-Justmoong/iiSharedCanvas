cmake_minimum_required(VERSION 3.31)
project(iiSharedCanvasConsumer LANGUAGES CXX)

find_package(iiSharedCanvas 0.5.0 CONFIG REQUIRED)

add_executable(iiSharedCanvasConsumer main.cpp)
target_compile_definitions(iiSharedCanvasConsumer PRIVATE
        IISHAREDCANVAS_CONSUMER_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(iiSharedCanvasConsumer PRIVATE
        iiSharedCanvas::iiSharedCanvas)
