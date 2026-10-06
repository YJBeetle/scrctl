# lwIP 的配置决定库 ABI，探针从固定源码单独构建，不使用配置未知的系统二进制。
enable_language(C)
include(FetchContent)
FetchContent_Declare(lwip
    URL https://codeload.github.com/lwip-tcpip/lwip/tar.gz/refs/tags/STABLE-2_2_1_RELEASE
    URL_HASH SHA256=ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539
    SOURCE_SUBDIR scrctl-probe-only)
# lwIP 提供的 Filelists.cmake 同时创建额外库和文档目标。这里只有 raw IPv6 核心。
FetchContent_MakeAvailable(lwip)
file(GLOB lwip_probe_core CONFIGURE_DEPENDS "${lwip_SOURCE_DIR}/src/core/*.c")
file(GLOB lwip_probe_ipv6 CONFIGURE_DEPENDS "${lwip_SOURCE_DIR}/src/core/ipv6/*.c")
add_library(scrctl_lwip_probe_core STATIC ${lwip_probe_core} ${lwip_probe_ipv6})
target_include_directories(scrctl_lwip_probe_core PUBLIC
    "${PROJECT_SOURCE_DIR}/tools/lwip" "${lwip_SOURCE_DIR}/src/include")
set_target_properties(scrctl_lwip_probe_core PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
add_executable(lwip_probe tools/lwip_probe.cpp)
target_link_libraries(lwip_probe PRIVATE scrctl_core scrctl_lwip_probe_core)
target_compile_options(lwip_probe PRIVATE -Wall -Wextra)
