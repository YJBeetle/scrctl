set(scrctl_icon "${PROJECT_SOURCE_DIR}/assets/icon/scrctl-icon-1024.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${scrctl_icon}")
file(READ "${scrctl_icon}" scrctl_icon_bytes HEX)
string(REGEX REPLACE "(..)" "0x\\1," scrctl_icon_bytes "${scrctl_icon_bytes}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/WindowIconData.h"
    CONTENT "#pragma once\nnamespace scrctl::app {\ninline constexpr unsigned char window_icon_png[] = {${scrctl_icon_bytes}};\n}\n"
    @ONLY)
