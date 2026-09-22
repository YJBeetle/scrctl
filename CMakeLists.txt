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

# ------------------------------------------------------------- core lib -----
add_library(scrctl_core STATIC
        src/json/Json.cpp
        src/plist/Plist.cpp
        src/xpc/XpcValue.cpp
        src/http2/Framing.cpp
        src/transport/Usbmux.cpp
        src/transport/TlsChannel.cpp
        src/transport/Lockdown.cpp
        src/transport/Tunnel.cpp
        src/net/TcpStream.cpp
        src/remote/RemoteXpc.cpp
        src/remote/Rsd.cpp
        src/remote/Device.cpp
        src/bitstream/AnnexB.cpp
        src/decode/VideoToolboxDecoder.cpp)
target_include_directories(scrctl_core PUBLIC src)
target_link_libraries(scrctl_core PUBLIC ScrctlAppleFrameworks OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(scrctl_core PRIVATE -Wall -Wextra)

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

# ---------------------------------------------------------------- tests -----
if(SCRCTL_BUILD_TESTS)
    enable_testing()

    add_executable(annexb_test tests/annexb_test.cpp)
    target_link_libraries(annexb_test PRIVATE scrctl_core)

    add_executable(plist_test tests/plist_test.cpp)
    target_link_libraries(plist_test PRIVATE scrctl_core)
    add_test(NAME plist COMMAND plist_test)

    add_executable(json_test tests/json_test.cpp)
    target_link_libraries(json_test PRIVATE scrctl_core)
    add_test(NAME json COMMAND json_test)

    add_executable(xpc_test tests/xpc_test.cpp)
    target_link_libraries(xpc_test PRIVATE scrctl_core)
    add_test(NAME xpc COMMAND xpc_test)

    add_executable(http2_test tests/http2_test.cpp)
    target_link_libraries(http2_test PRIVATE scrctl_core)
    add_test(NAME http2 COMMAND http2_test)

    add_executable(decode_test tests/decode_test.cpp)
    target_link_libraries(decode_test PRIVATE scrctl_core)

    # 这两个测试需要一个 .hevc 输入；没有就跳过而不是失败，
    # 免得干净 checkout 上 ctest 全红。
    set(SCRCTL_TEST_STREAM "" CACHE FILEPATH "用于测试的真机录制 .hevc（可留空跳过）")
    if(SCRCTL_TEST_STREAM)
        add_test(NAME annexb COMMAND annexb_test ${SCRCTL_TEST_STREAM})
        add_test(NAME decode COMMAND decode_test ${SCRCTL_TEST_STREAM} ${CMAKE_BINARY_DIR}/f.raw 5)
    else()
        add_test(NAME annexb_usage COMMAND annexb_test)
        set_tests_properties(annexb_usage PROPERTIES WILL_FAIL TRUE)
    endif()
endif()

install(TARGETS scrctl RUNTIME DESTINATION bin)
