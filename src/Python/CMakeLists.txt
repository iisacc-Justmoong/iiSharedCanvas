# Optional host-side Python bridge. Native applications, including iPad apps,
# do not link this shared library or a Python interpreter.
add_library(iiSharedCanvasPython SHARED ProjectedStrokeBridge.cpp ProjectedStrokeBridge.h)
target_compile_features(iiSharedCanvasPython PRIVATE cxx_std_23)
target_compile_definitions(iiSharedCanvasPython PRIVATE IISC_PYTHON_BUILD=1)
target_link_libraries(iiSharedCanvasPython PRIVATE iiSharedCanvas::Native)
set_target_properties(iiSharedCanvasPython PROPERTIES CXX_EXTENSIONS OFF)
if(APPLE)
    set_target_properties(iiSharedCanvasPython PROPERTIES INSTALL_RPATH
        "@loader_path;@loader_path/../../iiPaintEngine/lib;@loader_path/../../iiFileProvider/lib;@loader_path/../../iiSDKCore/lib;@loader_path/../../iiImageCodec/lib")
elseif(UNIX)
    set_target_properties(iiSharedCanvasPython PROPERTIES INSTALL_RPATH
        "$ORIGIN;$ORIGIN/../../iiPaintEngine/lib;$ORIGIN/../../iiFileProvider/lib;$ORIGIN/../../iiSDKCore/lib;$ORIGIN/../../iiImageCodec/lib")
endif()
install(TARGETS iiSharedCanvasPython
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES iisharedcanvas.py DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSharedCanvasNative/python)
install(FILES ProjectedStrokeBridge.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas/Python)

if(BUILD_TESTING)
    find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME iiSharedCanvas.Native.ProjectedStrokePython
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/ProjectedStrokePythonTest.py"
                    "$<TARGET_FILE:iiSharedCanvasPython>" "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(iiSharedCanvas.Native.ProjectedStrokePython PROPERTIES TIMEOUT 120
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
    endif()
endif()
