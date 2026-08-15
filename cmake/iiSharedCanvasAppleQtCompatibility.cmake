# Qt 6.8.3 can expose the removed AGL framework through WrapOpenGL on newer
# macOS SDKs. Sanitize only imported Qt link interfaces; no source API depends
# on Qt directly.
if(APPLE)
    foreach(_qt_target Qt6::Gui WrapOpenGL::WrapOpenGL)
        if(TARGET ${_qt_target})
            get_target_property(_qt_link_libraries ${_qt_target} INTERFACE_LINK_LIBRARIES)
            if(_qt_link_libraries)
                list(REMOVE_ITEM _qt_link_libraries "-framework" "AGL" "-framework AGL")
                list(FILTER _qt_link_libraries EXCLUDE REGEX "AGL")
                set_target_properties(${_qt_target} PROPERTIES
                        INTERFACE_LINK_LIBRARIES "${_qt_link_libraries}")
            endif()
            get_target_property(_qt_link_options ${_qt_target} INTERFACE_LINK_OPTIONS)
            if(_qt_link_options)
                list(FILTER _qt_link_options EXCLUDE REGEX "AGL")
                set_target_properties(${_qt_target} PROPERTIES
                        INTERFACE_LINK_OPTIONS "${_qt_link_options}")
            endif()
        endif()
    endforeach()
endif()
