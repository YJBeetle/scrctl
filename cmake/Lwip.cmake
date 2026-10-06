# 配置决定 lwIP 的 ABI；使用固定上游源码，不链接配置未知的系统二进制。
enable_language(C)
include(FetchContent)
if(NOT SCRCTL_FETCH_DEPENDENCIES AND NOT FETCHCONTENT_SOURCE_DIR_LWIP)
    message(FATAL_ERROR "离线构建需要 FETCHCONTENT_SOURCE_DIR_LWIP 指向 lwIP 2.2.1 源码")
endif()
FetchContent_Declare(lwip
    URL https://codeload.github.com/lwip-tcpip/lwip/tar.gz/refs/tags/STABLE-2_2_1_RELEASE
    URL_HASH SHA256=ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539
    SOURCE_SUBDIR scrctl-core-only)
FetchContent_MakeAvailable(lwip)
file(GLOB lwip_core CONFIGURE_DEPENDS "${lwip_SOURCE_DIR}/src/core/*.c")
file(GLOB lwip_ipv6 CONFIGURE_DEPENDS "${lwip_SOURCE_DIR}/src/core/ipv6/*.c")
add_library(scrctl_lwip STATIC ${lwip_core} ${lwip_ipv6} src/net/LwipPort.cpp)
target_include_directories(scrctl_lwip PUBLIC
    "${PROJECT_SOURCE_DIR}/src/net/lwip" "${lwip_SOURCE_DIR}/src/include")
set_target_properties(scrctl_lwip PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
