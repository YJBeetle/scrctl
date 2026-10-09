# 最低版本保持 3.20；较新 CMake 的行为通过可用性检查显式启用。
cmake_minimum_required(VERSION 3.20)
# 下载源码解包后使用当前时间，确保依赖归档更新能触发重新构建。
# 在顶层设置，作用于全部 FetchContent 声明；include() 的策略作用域不会外传。
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(scrctl
        VERSION 0.1.0
        DESCRIPTION "scrcpy for iOS: native screen mirroring and control over CoreDevice"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX SDL_MAIN_HANDLED)
endif()

include(cmake/Dependencies.cmake)
include(cmake/Lwip.cmake)
include(cmake/Translations.cmake)
include(cmake/Threading.cmake)

option(SCRCTL_BUILD_TESTS "构建无头自检" ON)
option(SCRCTL_FETCH_SDL2 "用 FetchContent 拉取 SDL2，而不是找系统安装" OFF)

# ---------------------------------------------------------------- SDL2 ------
# SDL_RenderWindowToLogical 的浮点坐标转换需要 2.0.18。
# 默认查找系统 SDL2 包；指定 SCRCTL_FETCH_SDL2 时改为下载固定版本源码。
# 未找到系统包且未启用下载时，配置失败并给出安装方式。
add_library(Scrctl_SDL INTERFACE)
if(SCRCTL_FETCH_SDL2)
    include(FetchContent)
    FetchContent_Declare(sdl2
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-2.32.10
            GIT_SHALLOW TRUE)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl2)
    target_link_libraries(Scrctl_SDL INTERFACE SDL2::SDL2)
else()
    find_package(SDL2 2.0.18 QUIET CONFIG)
    if(NOT TARGET SDL2::SDL2)
        find_package(SDL2 2.0.18 QUIET MODULE)
        if(SDL2_FOUND)
            target_include_directories(Scrctl_SDL INTERFACE "${SDL2_INCLUDE_DIRS}")
            target_link_libraries(Scrctl_SDL INTERFACE "${SDL2_LIBRARIES}")
        else()
            message(FATAL_ERROR
                    "找不到 SDL2 2.0.18 或以上版本。请安装开发包（macOS: brew install sdl2；"
                    "Debian/Ubuntu: apt install libsdl2-dev），"
                    "或配置时加 -DSCRCTL_FETCH_SDL2=ON 从源码拉取。")
        endif()
    else()
        target_link_libraries(Scrctl_SDL INTERFACE SDL2::SDL2)
    endif()
endif()
# 上游源码的 SDL2::SDL2 本身是 ALIAS；接口目标也兼容安装包和 MODULE 查找，
# 避免再为 ALIAS 创建 ALIAS，同时保留上游传递的编译选项与链接依赖。
add_library(Scrctl::SDL2 ALIAS Scrctl_SDL)

# ------------------------------------------------------- Apple frameworks ---
# Apple 媒体框架通过 C API 调用，解码器使用 .cpp，无需 Objective-C++ 或 ARC。
# 非 Apple 平台使用下方按依赖选择的解码后端。
add_library(ScrctlAppleFrameworks INTERFACE)
if(APPLE)
    target_link_libraries(ScrctlAppleFrameworks INTERFACE
            "-framework VideoToolbox"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework CoreFoundation"
            "-framework AudioToolbox"
            "-framework CoreAudio")
endif()

# ------------------------------------------------------------- OpenSSL ------
# lockdown 的 StartSession 可能要求双向 TLS，客户端证书来自配对记录。
# 本项目通过 usbmuxd 获取配对材料，避免要求读取受保护的 /var/db/lockdown。
# OpenSSL 是 Apache-2.0，与本项目许可兼容。
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------- 解码后端 ---
# 当前 VideoToolbox 适配器使用 2 字节 NAL 长度前缀，单个 NAL 最大为 65535 字节；
# 已有设备样本包含 70101 字节的 NAL。此前 4 字节配置失败的范围见解码器注释，
# 不能据此推断所有系统和码流都有相同限制。
# 找到 libavcodec 时编入软件 HEVC 解码，供 macOS 处理超限 NAL，并作为
# Linux / Windows 的视频后端。缺少开发包时仍可构建：macOS 只保留平台后端，
# 非 Apple 平台则没有可用的视频解码后端。
option(SCRCTL_LIBAV "使用 FFmpeg 软件视频与 AAC-ELD 音频解码" ON)
set(SCRCTL_LIBAV_FOUND OFF)
if(SCRCTL_LIBAV)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBAV IMPORTED_TARGET libavcodec libavutil libswscale libswresample)
    endif()
    if(LIBAV_FOUND)
        set(SCRCTL_LIBAV_FOUND ON)
    elseif(SCRCTL_LIBAV STREQUAL "ON")
        message(WARNING
                "没找到 FFmpeg 开发库（libavcodec/libavutil/libswscale/libswresample）。软件解码后端不编，"
                "关键帧超过 65535 字节的码流会出现无法解码的丢帧。"
                "装 ffmpeg 开发件后重新配置即可。")
    endif()
endif()

# libavformat 只用于容器录制；缺少它不影响已有解码器或裸 HEVC 文件。
option(SCRCTL_LIBAVFORMAT "使用 FFmpeg 封装 MP4 / MKV 录制" ON)
set(SCRCTL_LIBAVFORMAT_FOUND OFF)
if(SCRCTL_LIBAVFORMAT AND SCRCTL_LIBAV_FOUND)
    pkg_check_modules(RECORDING_AVFORMAT IMPORTED_TARGET libavformat)
    if(RECORDING_AVFORMAT_FOUND)
        set(SCRCTL_LIBAVFORMAT_FOUND ON)
    endif()
endif()

# ------------------------------------------------------------- core lib -----
add_library(scrctl_core STATIC
        src/i18n/Translation.cpp
        src/json/Json.cpp
        src/plist/Plist.cpp
        src/plist/PlistValue.cpp
        src/util/Base64.cpp
        src/xpc/XpcValue.cpp
        src/http2/Framing.cpp
        src/util/Deflate.cpp
        src/media/MediaOffer.cpp
        src/media/StreamSession.cpp
        src/media/FramePump.cpp
        src/media/ScreenshotSource.cpp
        src/media/AudioPump.cpp
        src/media/AudioRegulator.cpp
        src/media/RecordingClock.cpp
        src/media/RecordingVideoConfig.cpp
        src/media/RecordingMuxer.cpp
        src/media/Recorder.cpp
        src/rt/RtpHevc.cpp
        src/rt/RtpSeq.cpp
        src/rt/RtpTimestamp.cpp
        src/rt/Rtcp.cpp
        src/transport/Socket.cpp
        src/transport/SocketPlatform.cpp
        src/transport/Usbmux.cpp
        src/transport/TcpConnect.cpp
        src/transport/TlsChannel.cpp
        src/transport/Lockdown.cpp
        src/transport/Tunnel.cpp
        src/net/LwipRuntime.cpp
        src/net/Stack.cpp
        src/net/TcpStream.cpp
        src/net/UdpSocket.cpp
        src/remote/RemoteXpc.cpp
        src/remote/Rsd.cpp
        src/remote/Device.cpp
        src/remote/Discovery.cpp
        src/remote/Pasteboard.cpp
        src/remote/DisplayInfo.cpp
        src/remote/App.cpp
        src/remote/PairingChannel.cpp
        src/bitstream/AnnexB.cpp
        src/hid/Hid.cpp
        src/wifi/Crypto.cpp
        src/wifi/Srp.cpp
        src/wifi/Opack.cpp
        src/wifi/Tlv.cpp
        src/wifi/Rppairing.cpp
        src/wifi/PairRecord.cpp
        src/wifi/Mdns.cpp
        src/wifi/DiscoveryIdentity.cpp
        src/wifi/PairingIdentity.cpp
        src/wifi/PairVerify.cpp
        src/wifi/PairSetup.cpp
        src/remote/Pairing.cpp
        src/wifi/RemotePairing.cpp)
target_include_directories(scrctl_core PUBLIC src)
target_link_libraries(scrctl_core PUBLIC ScrctlAppleFrameworks OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
target_link_libraries(scrctl_core PRIVATE pugixml::pugixml scrctl_lwip Scrctl::Mdns)
target_link_libraries(scrctl_core PRIVATE Intl::Intl)
target_link_libraries(scrctl_core PUBLIC Scrctl::Threading)
if(WIN32)
    target_sources(scrctl_core PRIVATE src/wifi/PairRecordWindows.cpp)
    target_link_libraries(scrctl_core PUBLIC ws2_32 PRIVATE advapi32 iphlpapi)
endif()
target_compile_definitions(scrctl_core PRIVATE
        SCRCTL_BUILD_LOCALEDIR="${CMAKE_BINARY_DIR}/locale"
        SCRCTL_RELATIVE_LOCALEDIR="${SCRCTL_RELATIVE_LOCALEDIR}"
        SCRCTL_INSTALL_LOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}")
add_dependencies(scrctl_core scrctl_translations)
target_compile_options(scrctl_core PRIVATE -Wall -Wextra)

if(SCRCTL_LIBAVFORMAT_FOUND)
    target_link_libraries(scrctl_core PUBLIC PkgConfig::RECORDING_AVFORMAT)
    target_compile_definitions(scrctl_core PUBLIC SCRCTL_HAVE_LIBAVFORMAT=1)
endif()

# Apple 平台启用 VideoToolbox 和 AudioToolbox；其他平台没有对应的系统后端。
# 软件视频解码另按 libavcodec 的可用性选择实现或报告不可用的占位实现。
if(APPLE)
    target_sources(scrctl_core PRIVATE src/decode/VideoToolboxDecoder.cpp
            src/decode/AudioToolboxEldDecoder.cpp)
else()
    target_sources(scrctl_core PRIVATE src/decode/PlatformDecoderFallback.cpp
            src/decode/AudioDecoderNone.cpp)
endif()
if(SCRCTL_LIBAV_FOUND)
    target_sources(scrctl_core PRIVATE src/decode/FFmpegDecoder.cpp src/decode/FFmpegEldDecoder.cpp)
    # 静态库的 libav 符号由最终可执行文件链接，需要向使用方传递链接依赖。
    target_link_libraries(scrctl_core PUBLIC PkgConfig::LIBAV)
    # Decoder.h 的 kHaveDecoder 依赖这个宏；库与调用方必须看到相同定义，
    # 才能一致判断后端是否可用，并保持头文件中的内联定义一致。
    target_compile_definitions(scrctl_core PUBLIC SCRCTL_HAVE_LIBAV=1)
else()
    target_sources(scrctl_core PRIVATE src/decode/SoftwareDecoderStub.cpp)
endif()

# ----------------------------------------------------------------- app ------
add_library(scrctl_app STATIC
    src/app/Application.cpp
    src/app/Commands.cpp
        src/app/SdlRuntime.cpp
        src/app/Presenter.cpp
        src/app/KeyboardState.cpp
        src/app/ClipboardPasteJob.cpp
        src/app/AudioOut.cpp
        src/app/FileSource.cpp
        src/app/Cli.cpp
        src/app/DeviceConnection.cpp
        src/app/LiveSource.cpp
        src/app/LiveStats.cpp)
target_link_libraries(scrctl_app PUBLIC scrctl_core Scrctl::SDL2 PRIVATE CLI11::CLI11)
target_compile_options(scrctl_app PRIVATE -Wall -Wextra)
target_compile_definitions(scrctl_app PRIVATE SCRCTL_VERSION_STRING="${PROJECT_VERSION}")
add_executable(scrctl src/app/main.cpp)
target_link_libraries(scrctl PRIVATE scrctl_app)
target_compile_options(scrctl PRIVATE -Wall -Wextra)

# 开发诊断和历史实验默认关闭；目标、依赖和输出目录集中在此维护。
include(cmake/Tools.cmake)

# ---------------------------------------------------------------- tests -----
if(SCRCTL_BUILD_TESTS)
    enable_testing()
    if(SCRCTL_LWIP_PROBE)
        add_test(NAME lwip_compat COMMAND lwip_probe)
        set_tests_properties(lwip_compat PROPERTIES TIMEOUT 15)
    endif()
    if(SCRCTL_NGHTTP2_PROBE)
        add_test(NAME nghttp2_compat COMMAND nghttp2_probe)
        set_tests_properties(nghttp2_compat PROPERTIES TIMEOUT 10)
    endif()

    find_package(Python3 3.7 REQUIRED COMPONENTS Interpreter)
    function(scrctl_add_test_executable target)
        add_executable(${target} ${ARGN})
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
    endfunction()
    scrctl_add_test_executable(probe_wait_test tests/probe_wait_test.cpp)
    target_include_directories(probe_wait_test PRIVATE tools)
    target_link_libraries(probe_wait_test PRIVATE CLI11::CLI11)
    add_test(NAME probe_wait COMMAND probe_wait_test)
    if(SCRCTL_BUILD_EXPERIMENTS)
        add_test(NAME pli_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/pli_probe_cli_test.py" "$<TARGET_FILE:pli_probe>")
        set_tests_properties(pli_probe_cli PROPERTIES TIMEOUT 30)
        add_test(NAME lifecycle_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/lifecycle_probe_cli_test.py"
            "$<TARGET_FILE:wake_latency_probe>" "$<TARGET_FILE:hid_gate_probe>"
            "$<TARGET_FILE:restart_gap_probe>" "$<TARGET_FILE:lease_renew_probe>"
            "$<TARGET_FILE:lifetime_probe>" "$<TARGET_FILE:two_session_probe>")
        # 六个真实可执行文件的参数边界共用一组回归；sanitizer 启动每个进程较慢。
        set_tests_properties(lifecycle_probe_cli PROPERTIES TIMEOUT 90)
    endif()
    if(SCRCTL_BUILD_PROBES)
        add_test(NAME hid_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/hid_probe_cli_test.py" "$<TARGET_FILE:hid_probe>")
        # 参数和语言回归会串行启动 141 个进程；sanitizer 下的启动开销可超过 30 秒。
        # 单次调用仍由脚本的 3 秒超时保护，这里给整组检查保留有限的总预算。
        set_tests_properties(hid_probe_cli PROPERTIES TIMEOUT 90)
        add_test(NAME rr_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/rr_probe_cli_test.py" "$<TARGET_FILE:rr_keepalive_probe>")
        set_tests_properties(rr_probe_cli PROPERTIES TIMEOUT 30)
        add_test(NAME screenshot_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/screenshot_probe_cli_test.py" "$<TARGET_FILE:screenshot_probe>")
        set_tests_properties(screenshot_probe_cli PROPERTIES TIMEOUT 30)
        add_test(NAME wifi_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/wifi_probe_cli_test.py" "$<TARGET_FILE:wifi_probe>")
        set_tests_properties(wifi_probe_cli PROPERTIES TIMEOUT 30)
        add_test(NAME stall_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/stall_probe_cli_test.py" "$<TARGET_FILE:stall_probe>")
        set_tests_properties(stall_probe_cli PROPERTIES TIMEOUT 30)
        add_test(NAME audio_pump_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/audio_pump_probe_cli_test.py" "$<TARGET_FILE:audio_pump_probe>")
        set_tests_properties(audio_pump_probe_cli PROPERTIES TIMEOUT 30)
        add_test(NAME audio_decode_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/audio_decode_probe_cli_test.py" "$<TARGET_FILE:audio_decode_probe>")
        set_tests_properties(audio_decode_probe_cli PROPERTIES TIMEOUT 30)
        add_test(NAME feature_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/feature_probe_cli_test.py" "$<TARGET_FILE:feature_probe>")
        set_tests_properties(feature_probe_cli PROPERTIES TIMEOUT 30)
        add_test(NAME applist_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/applist_probe_cli_test.py" "$<TARGET_FILE:applist_probe>")
        set_tests_properties(applist_probe_cli PROPERTIES TIMEOUT 30)
        add_test(NAME app_launch_probe_cli COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/app_launch_probe_cli_test.py" "$<TARGET_FILE:app_launch_probe>")
        set_tests_properties(app_launch_probe_cli PROPERTIES TIMEOUT 30)
    endif()
    add_test(NAME i18n COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/i18n_test.py" "$<TARGET_FILE:scrctl>")
    set_tests_properties(i18n PROPERTIES TIMEOUT 30)
    add_test(NAME translation_catalogs COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/update_translations.py" --check)
    set_tests_properties(translation_catalogs PROPERTIES TIMEOUT 30)

    scrctl_add_test_executable(cli_test tests/cli_test.cpp)
    target_link_libraries(cli_test PRIVATE scrctl_app)
    add_test(NAME cli COMMAND cli_test)

    scrctl_add_test_executable(keyboard_state_test tests/keyboard_state_test.cpp
        src/app/KeyboardState.cpp)
    target_include_directories(keyboard_state_test PRIVATE src)
    target_link_libraries(keyboard_state_test PRIVATE Scrctl::SDL2)
    add_test(NAME keyboard_state COMMAND keyboard_state_test)

    scrctl_add_test_executable(clipboard_job_test tests/clipboard_job_test.cpp
        src/app/ClipboardPasteJob.cpp)
    target_include_directories(clipboard_job_test PRIVATE src)
    target_link_libraries(clipboard_job_test PRIVATE Scrctl::Threading)
    add_test(NAME clipboard_job COMMAND clipboard_job_test)
    set_tests_properties(clipboard_job PROPERTIES TIMEOUT 15)

    scrctl_add_test_executable(app_lifecycle_test tests/app_lifecycle_test.cpp)
    target_link_libraries(app_lifecycle_test PRIVATE scrctl_app)
    add_test(NAME app_lifecycle COMMAND app_lifecycle_test)

    # 直接编译真实文件源，测试只在两种解码器工厂边界提供受控后端。
    scrctl_add_test_executable(filesource_test tests/filesource_test.cpp src/app/FileSource.cpp)
    target_link_libraries(filesource_test PRIVATE scrctl_core)
    add_test(NAME filesource COMMAND filesource_test)
    set_tests_properties(filesource PROPERTIES TIMEOUT 30)

    scrctl_add_test_executable(annexb_test tests/annexb_test.cpp)
    target_link_libraries(annexb_test PRIVATE scrctl_core)

    scrctl_add_test_executable(plist_test tests/plist_test.cpp)
    target_link_libraries(plist_test PRIVATE scrctl_core)
    add_test(NAME plist COMMAND plist_test)

    scrctl_add_test_executable(rtp_test tests/rtp_test.cpp)
    target_link_libraries(rtp_test PRIVATE scrctl_core)
    add_test(NAME rtp COMMAND rtp_test)

    scrctl_add_test_executable(rtp_timestamp_test tests/rtp_timestamp_test.cpp)
    target_link_libraries(rtp_timestamp_test PRIVATE scrctl_core)
    add_test(NAME rtp_timestamp COMMAND rtp_timestamp_test)

    scrctl_add_test_executable(recording_clock_test tests/recording_clock_test.cpp)
    target_link_libraries(recording_clock_test PRIVATE scrctl_core)
    add_test(NAME recording_clock COMMAND recording_clock_test)

    scrctl_add_test_executable(recording_video_config_test tests/recording_video_config_test.cpp)
    target_link_libraries(recording_video_config_test PRIVATE scrctl_core)
    add_test(NAME recording_video_config COMMAND recording_video_config_test)

    scrctl_add_test_executable(recording_muxer_test tests/recording_muxer_test.cpp)
    target_link_libraries(recording_muxer_test PRIVATE scrctl_core)
    add_test(NAME recording_muxer COMMAND recording_muxer_test)

    scrctl_add_test_executable(recorder_test tests/recorder_test.cpp)
    target_link_libraries(recorder_test PRIVATE scrctl_core)
    add_test(NAME recorder COMMAND recorder_test)

    scrctl_add_test_executable(pasteboard_test tests/pasteboard_test.cpp)
    target_link_libraries(pasteboard_test PRIVATE scrctl_core)
    add_test(NAME pasteboard COMMAND pasteboard_test)

    scrctl_add_test_executable(displayinfo_test tests/displayinfo_test.cpp)
    target_link_libraries(displayinfo_test PRIVATE scrctl_core)
    add_test(NAME displayinfo COMMAND displayinfo_test)

    scrctl_add_test_executable(app_test tests/app_test.cpp)
    target_link_libraries(app_test PRIVATE scrctl_core)
    add_test(NAME app COMMAND app_test)

    scrctl_add_test_executable(hid_test tests/hid_test.cpp)
    target_link_libraries(hid_test PRIVATE scrctl_core)
    add_test(NAME hid COMMAND hid_test)

    scrctl_add_test_executable(live_hid_test tests/live_hid_test.cpp)
    target_link_libraries(live_hid_test PRIVATE scrctl_app)
    add_test(NAME live_hid COMMAND live_hid_test)
    set_tests_properties(live_hid PROPERTIES TIMEOUT 10)

    scrctl_add_test_executable(viewgeom_test tests/viewgeom_test.cpp)
    target_link_libraries(viewgeom_test PRIVATE scrctl_core)
    add_test(NAME viewgeom COMMAND viewgeom_test)

    # 转屏渲染要真的画一遍才知道 SDL 的角度方向对不对，所以这个测试要链 SDL。
    # 它自己把视频驱动设成 dummy，因此无显示器（CI、SSH）也能跑。
    scrctl_add_test_executable(render_test tests/render_test.cpp)
    target_link_libraries(render_test PRIVATE scrctl_app)
    add_test(NAME render COMMAND render_test)

    scrctl_add_test_executable(net_runtime_test tests/net_runtime_test.cpp)
    target_link_libraries(net_runtime_test PRIVATE scrctl_core scrctl_lwip)
    add_test(NAME net_runtime COMMAND net_runtime_test)
    set_tests_properties(net_runtime PROPERTIES TIMEOUT 40)

    scrctl_add_test_executable(service_cancellation_test tests/service_cancellation_test.cpp)
    target_link_libraries(service_cancellation_test PRIVATE scrctl_core scrctl_lwip)
    add_test(NAME service_cancellation COMMAND service_cancellation_test)
    set_tests_properties(service_cancellation PROPERTIES TIMEOUT 30)

    scrctl_add_test_executable(net_test tests/net_test.cpp)
    target_link_libraries(net_test PRIVATE scrctl_core)
    add_test(NAME net COMMAND net_test)

    # POSIX 验证 SIGPIPE 防护；Windows 验证原生句柄、I/O 取消和 PSK TLS。
    if(WIN32)
        scrctl_add_test_executable(transport_test tests/transport_windows_test.cpp)
    else()
        scrctl_add_test_executable(transport_test tests/transport_test.cpp)
    endif()
    target_link_libraries(transport_test PRIVATE scrctl_core)
    add_test(NAME transport COMMAND transport_test)
    set_tests_properties(transport PROPERTIES TIMEOUT 15)

    scrctl_add_test_executable(tcp_connect_test tests/tcp_connect_test.cpp)
    target_link_libraries(tcp_connect_test PRIVATE scrctl_core)
    add_test(NAME tcp_connect COMMAND tcp_connect_test)
    set_tests_properties(tcp_connect PROPERTIES TIMEOUT 10)

    scrctl_add_test_executable(rsd_test tests/rsd_test.cpp)
    target_link_libraries(rsd_test PRIVATE scrctl_core)
    add_test(NAME rsd COMMAND rsd_test)
    set_tests_properties(rsd PROPERTIES TIMEOUT 15)

    scrctl_add_test_executable(media_test tests/media_test.cpp)
    target_link_libraries(media_test PRIVATE scrctl_core)
    add_test(NAME media COMMAND media_test)

    scrctl_add_test_executable(json_test tests/json_test.cpp)
    target_link_libraries(json_test PRIVATE scrctl_core)
    add_test(NAME json COMMAND json_test)

    # 无线那一套（密码学 / TLV / 记录 / 帧）离线就能判，不需要设备。
    scrctl_add_test_executable(wifi_test tests/wifi_test.cpp tests/pair_setup_reply_test.cpp)
    target_link_libraries(wifi_test PRIVATE scrctl_core)
    add_test(NAME wifi COMMAND wifi_test)

    scrctl_add_test_executable(pairing_identity_test tests/pairing_identity_test.cpp)
    target_link_libraries(pairing_identity_test PRIVATE scrctl_core)
    add_test(NAME pairing_identity COMMAND pairing_identity_test)

    scrctl_add_test_executable(pairing_workflow_test tests/pairing_workflow_test.cpp)
    target_link_libraries(pairing_workflow_test PRIVATE scrctl_core)
    add_test(NAME pairing_workflow COMMAND pairing_workflow_test)

    scrctl_add_test_executable(mdns_test tests/mdns_test.cpp)
    target_link_libraries(mdns_test PRIVATE scrctl_core Scrctl::Mdns)
    add_test(NAME mdns COMMAND mdns_test)
    set_tests_properties(mdns PROPERTIES TIMEOUT 15)

    scrctl_add_test_executable(discovery_identity_test tests/discovery_identity_test.cpp)
    target_link_libraries(discovery_identity_test PRIVATE scrctl_core)
    add_test(NAME discovery_identity COMMAND discovery_identity_test)
    set_tests_properties(discovery_identity PROPERTIES TIMEOUT 15)

    scrctl_add_test_executable(discovery_test tests/discovery_test.cpp)
    target_link_libraries(discovery_test PRIVATE scrctl_core)
    add_test(NAME discovery COMMAND discovery_test)
    set_tests_properties(discovery PROPERTIES TIMEOUT 15)

    scrctl_add_test_executable(device_selection_test tests/device_selection_test.cpp)
    target_link_libraries(device_selection_test PRIVATE scrctl_app)
    add_test(NAME device_selection COMMAND device_selection_test)

    scrctl_add_test_executable(xpc_test tests/xpc_test.cpp)
    target_link_libraries(xpc_test PRIVATE scrctl_core)
    add_test(NAME xpc COMMAND xpc_test)

    scrctl_add_test_executable(http2_test tests/http2_test.cpp)
    target_link_libraries(http2_test PRIVATE scrctl_core)
    add_test(NAME http2 COMMAND http2_test)

    scrctl_add_test_executable(remote_channel_test tests/remote_channel_test.cpp)
    target_link_libraries(remote_channel_test PRIVATE scrctl_core)
    add_test(NAME remote_channel COMMAND remote_channel_test)
    # 32/64 MiB 输入上限测试需要大量缓冲复制；Debug + ASan/UBSan 显著慢于 Release。
    # 保留整个边界用例，给较慢的 CI runner 留出时间，仍用有限时限检测卡死。
    set_tests_properties(remote_channel PROPERTIES TIMEOUT 60)

    scrctl_add_test_executable(audio_buffer_test tests/audio_buffer_test.cpp)
    target_link_libraries(audio_buffer_test PRIVATE scrctl_core)
    add_test(NAME audio_buffer COMMAND audio_buffer_test)

    # 真实接收循环与录制器，替换已有网络/解码/声卡边界；不访问真机或音频设备。
    scrctl_add_test_executable(audio_capture_test tests/audio_capture_test.cpp
        src/media/AudioPump.cpp src/app/AudioOut.cpp)
    target_link_libraries(audio_capture_test PRIVATE scrctl_core Scrctl::SDL2)
    add_test(NAME audio_capture COMMAND audio_capture_test)
    scrctl_add_test_executable(frame_capture_test tests/frame_capture_test.cpp
        src/media/FramePump.cpp)
    target_link_libraries(frame_capture_test PRIVATE scrctl_core)
    add_test(NAME frame_capture COMMAND frame_capture_test)
    set_tests_properties(frame_capture PROPERTIES TIMEOUT 30)
    set_tests_properties(audio_capture PROPERTIES TIMEOUT 30)

    scrctl_add_test_executable(decode_test tests/decode_test.cpp)
    target_link_libraries(decode_test PRIVATE scrctl_core)

    if(SCRCTL_LIBAV_FOUND)
        scrctl_add_test_executable(audio_regulator_test tests/audio_regulator_test.cpp)
        target_link_libraries(audio_regulator_test PRIVATE scrctl_core)
        add_test(NAME audio_regulator COMMAND audio_regulator_test)

        # 真实 PCM 缓冲/消费与 libswresample；只替换设备会话、解码和声卡边界。
        scrctl_add_test_executable(audio_pause_recovery_test tests/audio_pause_recovery_test.cpp
            src/media/AudioPump.cpp src/app/AudioOut.cpp)
        target_link_libraries(audio_pause_recovery_test PRIVATE scrctl_core Scrctl::SDL2)
        add_test(NAME audio_pause_recovery COMMAND audio_pause_recovery_test)
        set_tests_properties(audio_pause_recovery PROPERTIES TIMEOUT 15)

        scrctl_add_test_executable(audio_decoder_test tests/audio_decoder_test.cpp)
        target_link_libraries(audio_decoder_test PRIVATE scrctl_core)
        add_test(NAME audio_decoder COMMAND audio_decoder_test)
    endif()

    # 合成码流那部分自检不需要输入文件，任何时候都跑。
    add_test(NAME annexb_synth COMMAND annexb_test)

    # 真机录制相关的两个测试需要一个 .hevc 输入；没有就跳过而不是失败，
    # 免得干净 checkout 上 ctest 全红。
    #
    # 但"注册时存在"不等于"跑的时候还在"：这个输入是个 CACHE 路径，而 macOS 的
    # 每日清理会把 /tmp 里放了几天的一切都收走（真发生过：同一棵树上午 17/17，
    # 半夜变成 15/17 两项红，红的原因是"打不开输入文件"，与被测代码无关）。所以
    # 除了 configure 期这一道，运行期也要能跳——用 SKIP_REGULAR_EXPRESSION 认那句
    # "打不开"，缺文件就是 Skipped，不是 Failed。
    set(SCRCTL_TEST_STREAM "" CACHE FILEPATH "用于测试的真机录制 .hevc（可留空跳过）")
    if(SCRCTL_TEST_STREAM)
        if(NOT EXISTS "${SCRCTL_TEST_STREAM}")
            message(WARNING "SCRCTL_TEST_STREAM=${SCRCTL_TEST_STREAM} 现在不存在，"
                            "这两项测试会记为 Skipped（不是通过）")
        endif()
        add_test(NAME annexb COMMAND annexb_test ${SCRCTL_TEST_STREAM})
        add_test(NAME decode COMMAND decode_test ${SCRCTL_TEST_STREAM} ${CMAKE_BINARY_DIR}/f.raw 5)
        set_tests_properties(annexb decode PROPERTIES SKIP_REGULAR_EXPRESSION "打不开")
    endif()
endif()

if(WIN32)
    include(cmake/WindowsRuntime.cmake)
elseif(APPLE)
    include(cmake/MacOSRuntime.cmake)
else()
    install(TARGETS scrctl RUNTIME DESTINATION bin)
endif()
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION share/doc/scrctl)
if(WIN32)
    install(FILES docs/WINDOWS.md DESTINATION share/doc/scrctl)
elseif(APPLE)
    install(FILES docs/MACOS.md DESTINATION share/doc/scrctl)
endif()
