foreach(_arguments IN ITEMS
        "--file-only;--operation=undo;--allocation-group=unknown"
        "--file-only;--operation=undo;--allocation-group=control;--allocation-group=lower"
        "--file-only;--allocation-group=control"
        "--memory-only;--operation=undo;--allocation-group=control"
        "--file-only;--operation=begin;--allocation-group=control")
    execute_process(COMMAND "${PROGRAM}" ${_arguments}
        RESULT_VARIABLE _result OUTPUT_QUIET ERROR_QUIET)
    if(NOT "${_result}" STREQUAL "2")
        message(FATAL_ERROR "Invalid allocation group selection returned ${_result}, expected 2: ${_arguments}")
    endif()
endforeach()
