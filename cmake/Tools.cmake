# 诊断工具用于开发和设备验证，不属于安装包。历史实验另行选择，便于复现
# 原有协议对照；归档并不表示这些工具已具备产品命令的稳定接口。
option(SCRCTL_BUILD_PROBES "构建常用开发诊断工具" OFF)
option(SCRCTL_BUILD_EXPERIMENTS "构建归档的协议实验工具" OFF)
option(SCRCTL_LWIP_PROBE "构建 lwIP 隧道适配验证探针" OFF)
option(SCRCTL_NGHTTP2_PROBE "构建 nghttp2 适配验证探针（需要系统 libnghttp2）" OFF)

function(scrctl_add_tool target directory)
    add_executable(${target} "${PROJECT_SOURCE_DIR}/tools/${directory}/${target}.cpp")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tools")
    if(ARGN)
        target_link_libraries(${target} PRIVATE ${ARGN})
    else()
        target_link_libraries(${target} PRIVATE scrctl_core)
    endif()
    if(directory STREQUAL ".")
        set(output_directory tools)
    else()
        set(output_directory experiments)
    endif()
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${output_directory}")
endfunction()

if(SCRCTL_BUILD_PROBES)
    foreach(target IN ITEMS
            feature_probe screenshot_probe rr_keepalive_probe wifi_probe
            audio_pump_probe audio_decode_probe applist_probe app_launch_probe hid_probe)
        scrctl_add_tool(${target} . scrctl_core CLI11::CLI11)
    endforeach()
    scrctl_add_tool(stall_probe . scrctl_app CLI11::CLI11)
endif()

if(SCRCTL_BUILD_EXPERIMENTS)
    foreach(target IN ITEMS
            usbmux_probe lockdown_probe pasteboard_probe pli_probe fir_probe rtcp_probe
            lifetime_probe two_session_probe lease_renew_probe restart_gap_probe
            wake_latency_probe stream_probe bitrate_probe frame_probe hid_gate_probe
            feature_schema_probe display_info_probe)
        scrctl_add_tool(${target} experiments)
    endforeach()
    foreach(target IN ITEMS
            pli_probe lifetime_probe two_session_probe lease_renew_probe
            restart_gap_probe wake_latency_probe hid_gate_probe)
        target_link_libraries(${target} PRIVATE CLI11::CLI11)
    endforeach()
endif()

# 两项库适配实验有独立开关；启用历史实验不会自动要求安装 nghttp2。
# lwIP 的生产适配始终构建，SCRCTL_LWIP_PROBE 仅控制独立验证程序。
if(SCRCTL_LWIP_PROBE)
    scrctl_add_tool(lwip_probe experiments scrctl_core scrctl_lwip)
    target_compile_options(lwip_probe PRIVATE -Wall -Wextra)
endif()
if(SCRCTL_NGHTTP2_PROBE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2>=1.50)
    scrctl_add_tool(nghttp2_probe experiments scrctl_app PkgConfig::NGHTTP2)
    target_compile_options(nghttp2_probe PRIVATE -Wall -Wextra)
endif()
