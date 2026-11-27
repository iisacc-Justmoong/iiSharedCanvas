# Optional host-side Python bridge. Native applications, including iPad apps,
# do not link this shared library or a Python interpreter.
add_library(iiSharedCanvasPython SHARED ProjectedStrokeBridge.cpp ProjectedStrokeBridge.h CanvasControlBridge.cpp CanvasControlBridge.h SequenceBridge_p.hpp SequenceModelBridge_p.hpp SequenceModelTypes_p.hpp)
target_compile_features(iiSharedCanvasPython PRIVATE cxx_std_23)
target_compile_definitions(iiSharedCanvasPython PRIVATE IISC_PYTHON_BUILD=1)
target_link_libraries(iiSharedCanvasPython PRIVATE iiSharedCanvas::Native)
if(TARGET iiSharedCanvasGui)
    # Keep GUI/Vulkan dependencies out of the existing headless Python bridge.
    add_library(iiSharedCanvasGuiPython SHARED CanvasGuiBridge.cpp CanvasGuiBridge.h)
    target_compile_features(iiSharedCanvasGuiPython PRIVATE cxx_std_23)
    target_compile_definitions(iiSharedCanvasGuiPython PRIVATE IISC_PYTHON_BUILD=1)
    target_link_libraries(iiSharedCanvasGuiPython PRIVATE iiSharedCanvas::Gui)
    set_target_properties(iiSharedCanvasGuiPython PROPERTIES CXX_EXTENSIONS OFF)
    if(APPLE)
        set_target_properties(iiSharedCanvasGuiPython PROPERTIES INSTALL_RPATH
            "@loader_path;@loader_path/../../iiPaintEngine/lib;@loader_path/../../iiFileProvider/lib;@loader_path/../../iiSDKCore/lib;@loader_path/../../iiImageCodec/lib;@loader_path/../../Vulkan/lib")
    elseif(UNIX)
        set_target_properties(iiSharedCanvasGuiPython PROPERTIES INSTALL_RPATH
            "$ORIGIN;$ORIGIN/../../iiPaintEngine/lib;$ORIGIN/../../iiFileProvider/lib;$ORIGIN/../../iiSDKCore/lib;$ORIGIN/../../iiImageCodec/lib;$ORIGIN/../../Vulkan/lib")
    endif()
    install(TARGETS iiSharedCanvasGuiPython LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES CanvasGuiBridge.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas/Python)
endif()
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
install(FILES ProjectedStrokeBridge.h CanvasControlBridge.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSharedCanvas/Python)

if(BUILD_TESTING)
    find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        if(TARGET iiSharedCanvasGui)
            add_test(NAME iiSharedCanvas.Native.CanvasGuiPython
                COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/CanvasGuiPythonTest.py"
                        "$<TARGET_FILE:iiSharedCanvasGuiPython>" "${CMAKE_CURRENT_SOURCE_DIR}")
            set_tests_properties(iiSharedCanvas.Native.CanvasGuiPython PROPERTIES TIMEOUT 120
                ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
        endif()
        add_test(NAME iiSharedCanvas.Native.SequenceOperationsPython
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/SequenceOperationsPythonTest.py"
                    "$<TARGET_FILE:iiSharedCanvasPython>" "${CMAKE_CURRENT_SOURCE_DIR}"
                    "$<TARGET_FILE:iiSharedCanvasNativeSequenceOperationsTest>" "${_iisc_test_work}/sequence-python"
                    "$<TARGET_FILE:iiSharedCanvasNativeSequencePersistenceRichTest>")
        set_tests_properties(iiSharedCanvas.Native.SequenceOperationsPython PROPERTIES TIMEOUT 300
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
        add_test(NAME iiSharedCanvas.Native.ChannelExchangePython
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/ChannelExchangePythonTest.py"
                    "$<TARGET_FILE:iiSharedCanvasPython>" "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(iiSharedCanvas.Native.ChannelExchangePython PROPERTIES TIMEOUT 120
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
        add_test(NAME iiSharedCanvas.Native.CanvasOperationsPython
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/CanvasOperationsPythonTest.py"
                    "$<TARGET_FILE:iiSharedCanvasPython>" "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(iiSharedCanvas.Native.CanvasOperationsPython PROPERTIES TIMEOUT 120
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
        add_test(NAME iiSharedCanvas.Native.LayerEditingPython
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/LayerEditingPythonTest.py"
                    "$<TARGET_FILE:iiSharedCanvasPython>" "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(iiSharedCanvas.Native.LayerEditingPython PROPERTIES TIMEOUT 120
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
        add_test(NAME iiSharedCanvas.Native.LayerContentOperationsPython
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/LayerContentOperationsPythonTest.py"
                    "$<TARGET_FILE:iiSharedCanvasPython>" "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(iiSharedCanvas.Native.LayerContentOperationsPython PROPERTIES TIMEOUT 120
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
        add_test(NAME iiSharedCanvas.Native.SelectionLayerRelationsPython
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/SelectionLayerRelationsPythonTest.py"
                    "$<TARGET_FILE:iiSharedCanvasPython>" "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(iiSharedCanvas.Native.SelectionLayerRelationsPython PROPERTIES TIMEOUT 120
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
        add_test(NAME iiSharedCanvas.Native.DocumentAssetControlPython
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/DocumentAssetControlPythonTest.py"
                    "$<TARGET_FILE:iiSharedCanvasPython>" "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(iiSharedCanvas.Native.DocumentAssetControlPython PROPERTIES TIMEOUT 120
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
        add_test(NAME iiSharedCanvas.Native.ProjectedStrokePython
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/ProjectedStrokePythonTest.py"
                    "$<TARGET_FILE:iiSharedCanvasPython>" "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(iiSharedCanvas.Native.ProjectedStrokePython PROPERTIES TIMEOUT 120
            ENVIRONMENT_MODIFICATION "DYLD_LIBRARY_PATH=unset:;DYLD_FALLBACK_LIBRARY_PATH=unset:;DYLD_FRAMEWORK_PATH=unset:;LD_LIBRARY_PATH=unset:")
    endif()
endif()
