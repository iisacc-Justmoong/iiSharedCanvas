cmake_minimum_required(VERSION 3.31)
project(iiSharedCanvasConsumer LANGUAGES CXX)

find_package(iiSharedCanvas 0.3.0 CONFIG REQUIRED)

add_executable(iiSharedCanvasConsumer main.cpp)
target_link_libraries(iiSharedCanvasConsumer PRIVATE
        iiSharedCanvas::iiSharedCanvas)
