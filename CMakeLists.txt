cmake_minimum_required(VERSION 3.28)

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

option(SCRCTL_BUILD_TESTS "构建无头自检" ON)
option(SCRCTL_FETCH_SDL2 "用 FetchContent 拉取 SDL2，而不是找系统安装" OFF)

# ---------------------------------------------------------------- SDL2 ------
# 优先用系统的 SDL2（macOS: brew，Linux: apt/libsdl2-dev），找不到再按源码拉。
# 关掉拉取又找不到时给出明确指引，而不是抛一个看不懂的 CMake 错误。
if(SCRCTL_FETCH_SDL2)
    include(FetchContent)
    FetchContent_Declare(sdl2
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG v2.32.72
            GIT_SHALLOW TRUE)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sdl2)
    add_library(Scrctl::SDL2 ALIAS SDL2::SDL2)
else()
    find_package(SDL2 QUIET CONFIG)
    if(NOT TARGET SDL2::SDL2)
        find_package(SDL2 QUIET MODULE)
        if(SDL2_FOUND)
            add_library(Scrctl_SDL INTERFACE)
            target_include_directories(Scrctl_SDL INTERFACE "${SDL2_INCLUDE_DIRS}")
            target_link_libraries(Scrctl_SDL INTERFACE "${SDL2_LIBRARIES}")
            add_library(Scrctl::SDL2 ALIAS Scrctl_SDL)
        else()
            message(FATAL_ERROR
                    "找不到 SDL2。装一个（macOS: brew install sdl2；"
                    "Debian/Ubuntu: apt install libsdl2-dev），"
                    "或配置时加 -DSCRCTL_FETCH_SDL2=ON 从源码拉取。")
        endif()
    else()
        add_library(Scrctl::SDL2 ALIAS SDL2::SDL2)
    endif()
endif()

# ------------------------------------------------------- Apple frameworks ---
# VideoToolbox / CoreMedia / CoreVideo 是纯 C API，所以解码器写成 .cpp 而非
# .mm，避开 ARC 与 CF 桥接。非 Apple 平台后续接 libav 后端。
add_library(ScrctlAppleFrameworks INTERFACE)
if(APPLE)
    target_link_libraries(ScrctlAppleFrameworks INTERFACE
            "-framework VideoToolbox"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework CoreFoundation")
endif()

# ------------------------------------------------------------- OpenSSL ------
# lockdown 的 StartSession 之后必须双向 TLS（客户端证书来自配对记录），
# 而 /var/db/lockdown 是 root-only，所以配对材料只能经 usbmuxd 取。
# OpenSSL 是 Apache-2.0，与本项目许可兼容。
find_package(OpenSSL REQUIRED)

# ---------------------------------------------------------------- 解码后端 ---
# VideoToolbox 的长度前缀只有 2 字节可用（4 字节的会话见一个样本拒一个），所以
# 单帧超过 65535 字节的码流它吃不下——真机主屏的 IDR 实测 70101 字节。那种码流
# 只有软解能救，而 Linux/Windows 上软解又是唯一后端，所以 libavcodec 找得到就编。
#
# 找不到时降级：只留平台后端，超大 NAL 仍然只能丢帧。这样没有 ffmpeg 开发件的
# 机器仍然能构建，只是失去对高分辨率细节画面的兜底。
option(SCRCTL_LIBAV "用 libavcodec 编软件 HEVC 解码后端（超大 NAL 的兜底）" ON)
set(SCRCTL_LIBAV_FOUND OFF)
if(SCRCTL_LIBAV)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBAV IMPORTED_TARGET libavcodec libavutil libswscale)
    endif()
    if(LIBAV_FOUND)
        set(SCRCTL_LIBAV_FOUND ON)
    elseif(SCRCTL_LIBAV STREQUAL "ON")
        message(WARNING
                "没找到 libavcodec/libswscale（pkg-config）。软件解码后端不编，"
                "关键帧超过 65535 字节的码流会出现无法解码的丢帧。"
                "装 ffmpeg 开发件后重新配置即可。")
    endif()
endif()

# ------------------------------------------------------------- core lib -----
add_library(scrctl_core STATIC
        src/jsonlite/Jsonlite.cpp
        src/plist/Plist.cpp
        src/plist/Bplist.cpp
        src/xpc/XpcValue.cpp
        src/http2/Framing.cpp
        src/util/Deflate.cpp
        src/media/MediaOffer.cpp
        src/media/StreamSession.cpp
        src/media/FramePump.cpp
        src/rt/RtpHevc.cpp
        src/rt/Rtcp.cpp
        src/transport/Usbmux.cpp
        src/transport/TlsChannel.cpp
        src/transport/Lockdown.cpp
        src/transport/Tunnel.cpp
        src/net/Stack.cpp
        src/net/TcpStream.cpp
        src/net/UdpSocket.cpp
        src/remote/RemoteXpc.cpp
        src/remote/Rsd.cpp
        src/remote/Device.cpp
        src/remote/Pasteboard.cpp
        src/remote/DisplayInfo.cpp
        src/remote/App.cpp
        src/bitstream/AnnexB.cpp
        src/hid/Hid.cpp)
target_include_directories(scrctl_core PUBLIC src)
target_link_libraries(scrctl_core PUBLIC ScrctlAppleFrameworks OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(scrctl_core PRIVATE -Wall -Wextra)

# 解码后端按平台与依赖挑：平台后端在 Apple 上是 VideoToolbox，别处只能借用软解；
# 软解后端有 libavcodec 就用，没有就换回返回 nullptr 的占位实现。
if(APPLE)
    target_sources(scrctl_core PRIVATE src/decode/VideoToolboxDecoder.cpp)
else()
    target_sources(scrctl_core PRIVATE src/decode/PlatformDecoderFallback.cpp)
endif()
if(SCRCTL_LIBAV_FOUND)
    target_sources(scrctl_core PRIVATE src/decode/FFmpegDecoder.cpp)
    # 必须是 PUBLIC：解码后端编在静态库里，最终链接由各个可执行文件做，
    # 搜索路径不传下去就是"库找到了、链接找不到"。
    target_link_libraries(scrctl_core PUBLIC PkgConfig::LIBAV)
    # 同样是 PUBLIC：`decode/Decoder.h` 里的 `kHaveDecoder` 要靠它判断"这个构建有没有
    # 后端"。只在库里定义、头文件在调用点看不到，就成了"库能解、主程序以为自己不能解"，
    # 而且两边取值不一样还是 ODR 违规。
    target_compile_definitions(scrctl_core PUBLIC SCRCTL_HAVE_LIBAV=1)
else()
    target_sources(scrctl_core PRIVATE src/decode/SoftwareDecoderStub.cpp)
endif()

# ----------------------------------------------------------------- app ------
add_executable(scrctl src/app/main.cpp)
target_link_libraries(scrctl PRIVATE scrctl_core Scrctl::SDL2)
target_compile_options(scrctl PRIVATE -Wall -Wextra)

# 需要真机在线的手工探针，不进 ctest。
add_executable(usbmux_probe tools/usbmux_probe.cpp)
target_link_libraries(usbmux_probe PRIVATE scrctl_core)

add_executable(lockdown_probe tools/lockdown_probe.cpp)
target_link_libraries(lockdown_probe PRIVATE scrctl_core)

add_executable(feature_probe tools/feature_probe.cpp)
target_link_libraries(feature_probe PRIVATE scrctl_core)

add_executable(screenshot_probe tools/screenshot_probe.cpp)
target_link_libraries(screenshot_probe PRIVATE scrctl_core)

add_executable(rr_keepalive_probe tools/rr_keepalive_probe.cpp)
target_link_libraries(rr_keepalive_probe PRIVATE scrctl_core)

add_executable(two_session_probe tools/two_session_probe.cpp)
target_link_libraries(two_session_probe PRIVATE scrctl_core)

add_executable(lease_renew_probe tools/lease_renew_probe.cpp)
target_link_libraries(lease_renew_probe PRIVATE scrctl_core)

add_executable(stream_probe tools/stream_probe.cpp)
target_link_libraries(stream_probe PRIVATE scrctl_core)

add_executable(applist_probe tools/applist_probe.cpp)
target_link_libraries(applist_probe PRIVATE scrctl_core)

add_executable(app_launch_probe tools/app_launch_probe.cpp)
target_link_libraries(app_launch_probe PRIVATE scrctl_core)

add_executable(hid_probe tools/hid_probe.cpp)
target_link_libraries(hid_probe PRIVATE scrctl_core)

add_executable(hid_gate_probe tools/hid_gate_probe.cpp)
target_link_libraries(hid_gate_probe PRIVATE scrctl_core)

add_executable(wake_latency_probe tools/wake_latency_probe.cpp)
target_link_libraries(wake_latency_probe PRIVATE scrctl_core)

add_executable(pli_probe tools/pli_probe.cpp)
target_link_libraries(pli_probe PRIVATE scrctl_core)

add_executable(rtcp_probe tools/rtcp_probe.cpp)
target_link_libraries(rtcp_probe PRIVATE scrctl_core)

add_executable(lifetime_probe tools/lifetime_probe.cpp)
target_link_libraries(lifetime_probe PRIVATE scrctl_core)

add_executable(fir_probe tools/fir_probe.cpp)
target_link_libraries(fir_probe PRIVATE scrctl_core)

add_executable(restart_gap_probe tools/restart_gap_probe.cpp)
target_link_libraries(restart_gap_probe PRIVATE scrctl_core)

add_executable(bitrate_probe tools/bitrate_probe.cpp)
target_link_libraries(bitrate_probe PRIVATE scrctl_core)

# ---------------------------------------------------------------- tests -----
if(SCRCTL_BUILD_TESTS)
    enable_testing()

    add_executable(annexb_test tests/annexb_test.cpp)
    target_link_libraries(annexb_test PRIVATE scrctl_core)

    add_executable(plist_test tests/plist_test.cpp)
    target_link_libraries(plist_test PRIVATE scrctl_core)
    add_test(NAME plist COMMAND plist_test)

    add_executable(rtp_test tests/rtp_test.cpp)
    target_link_libraries(rtp_test PRIVATE scrctl_core)
    add_test(NAME rtp COMMAND rtp_test)

    add_executable(pasteboard_test tests/pasteboard_test.cpp)
    target_link_libraries(pasteboard_test PRIVATE scrctl_core)
    add_test(NAME pasteboard COMMAND pasteboard_test)

    add_executable(displayinfo_test tests/displayinfo_test.cpp)
    target_link_libraries(displayinfo_test PRIVATE scrctl_core)
    add_test(NAME displayinfo COMMAND displayinfo_test)

    add_executable(app_test tests/app_test.cpp)
    target_link_libraries(app_test PRIVATE scrctl_core)
    add_test(NAME app COMMAND app_test)

    add_executable(hid_test tests/hid_test.cpp)
    target_link_libraries(hid_test PRIVATE scrctl_core)
    add_test(NAME hid COMMAND hid_test)

    add_executable(viewgeom_test tests/viewgeom_test.cpp)
    target_link_libraries(viewgeom_test PRIVATE scrctl_core)
    add_test(NAME viewgeom COMMAND viewgeom_test)

    add_executable(net_test tests/net_test.cpp)
    target_link_libraries(net_test PRIVATE scrctl_core)
    add_test(NAME net COMMAND net_test)

    add_executable(media_test tests/media_test.cpp)
    target_link_libraries(media_test PRIVATE scrctl_core)
    add_test(NAME media COMMAND media_test)

    add_executable(bplist_test tests/bplist_test.cpp)
    target_link_libraries(bplist_test PRIVATE scrctl_core)
    add_test(NAME bplist COMMAND bplist_test)

    add_executable(jsonlite_test tests/jsonlite_test.cpp)
    target_link_libraries(jsonlite_test PRIVATE scrctl_core)
    add_test(NAME jsonlite COMMAND jsonlite_test)

    add_executable(xpc_test tests/xpc_test.cpp)
    target_link_libraries(xpc_test PRIVATE scrctl_core)
    add_test(NAME xpc COMMAND xpc_test)

    add_executable(http2_test tests/http2_test.cpp)
    target_link_libraries(http2_test PRIVATE scrctl_core)
    add_test(NAME http2 COMMAND http2_test)

    add_executable(decode_test tests/decode_test.cpp)
    target_link_libraries(decode_test PRIVATE scrctl_core)

    # 合成码流那部分自检不需要输入文件，任何时候都跑。
    add_test(NAME annexb_synth COMMAND annexb_test)

    # 真机录制相关的两个测试需要一个 .hevc 输入；没有就跳过而不是失败，
    # 免得干净 checkout 上 ctest 全红。
    set(SCRCTL_TEST_STREAM "" CACHE FILEPATH "用于测试的真机录制 .hevc（可留空跳过）")
    if(SCRCTL_TEST_STREAM)
        add_test(NAME annexb COMMAND annexb_test ${SCRCTL_TEST_STREAM})
        add_test(NAME decode COMMAND decode_test ${SCRCTL_TEST_STREAM} ${CMAKE_BINARY_DIR}/f.raw 5)
    endif()
endif()

install(TARGETS scrctl RUNTIME DESTINATION bin)

add_executable(feature_schema_probe tools/feature_schema_probe.cpp)
target_link_libraries(feature_schema_probe PRIVATE scrctl_core)

add_executable(frame_probe tools/frame_probe.cpp)
target_link_libraries(frame_probe PRIVATE scrctl_core)

add_executable(pasteboard_probe tools/pasteboard_probe.cpp)
target_link_libraries(pasteboard_probe PRIVATE scrctl_core)

# 只显示几何订阅，不起媒体会话（理由见文件头）。
add_executable(display_info_probe tools/display_info_probe.cpp)
target_link_libraries(display_info_probe PRIVATE scrctl_core)
