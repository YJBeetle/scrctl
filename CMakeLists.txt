# 最低版本保持 3.20；较新 CMake 的行为通过可用性检查显式启用。
cmake_minimum_required(VERSION 3.20)
# 下载源码解包后使用当前时间，确保依赖归档更新能触发重新构建。
# 在顶层设置，作用于全部 FetchContent 声明；include() 的策略作用域不会外传。
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

project(scrctl
        VERSION 1.0.0
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

# Device protocols and media capture live in a separately running xdb Server.
set(XDB_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(XDB_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(XDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XDB_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(xdb)
if(WIN32)
    set_target_properties(xdb_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()

option(SCRCTL_LIBAV "Use FFmpeg for local HEVC file playback" ON)
find_package(PkgConfig QUIET)
if(SCRCTL_LIBAV AND PkgConfig_FOUND)
    pkg_check_modules(LIBAV QUIET IMPORTED_TARGET libavcodec libavutil libswscale)
endif()
add_library(scrctl_core STATIC src/bitstream/AnnexB.cpp src/i18n/Translation.cpp)
target_include_directories(scrctl_core PUBLIC src)
target_link_libraries(scrctl_core PRIVATE Intl::Intl PUBLIC Scrctl::Threading)
target_compile_definitions(scrctl_core PRIVATE SCRCTL_BUILD_LOCALEDIR="${CMAKE_BINARY_DIR}/locale"
    SCRCTL_RELATIVE_LOCALEDIR="${SCRCTL_RELATIVE_LOCALEDIR}"
    SCRCTL_INSTALL_LOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}")
add_dependencies(scrctl_core scrctl_translations)
if(APPLE)
    target_sources(scrctl_core PRIVATE src/decode/VideoToolboxDecoder.cpp)
    target_link_libraries(scrctl_core PRIVATE "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo" "-framework CoreFoundation")
else()
    target_sources(scrctl_core PRIVATE src/decode/PlatformDecoderFallback.cpp)
endif()
if(LIBAV_FOUND)
    target_sources(scrctl_core PRIVATE src/decode/FFmpegDecoder.cpp)
    target_link_libraries(scrctl_core PUBLIC PkgConfig::LIBAV)
    target_compile_definitions(scrctl_core PUBLIC SCRCTL_HAVE_LIBAV=1)
else()
    target_sources(scrctl_core PRIVATE src/decode/SoftwareDecoderStub.cpp)
endif()
add_library(scrctl_app STATIC src/app/Application.cpp src/app/Commands.cpp src/app/SdlRuntime.cpp
    src/app/Presenter.cpp src/app/KeyboardState.cpp src/app/ClipboardPasteJob.cpp
    src/app/DeviceRotationJob.cpp src/app/AudioOut.cpp src/app/FileSource.cpp
    src/app/Cli.cpp src/app/LiveSource.cpp)
target_link_libraries(scrctl_app PUBLIC scrctl_core Scrctl::SDL2 xdb_client nlohmann_json::nlohmann_json PRIVATE CLI11::CLI11)
target_compile_definitions(scrctl_app PRIVATE SCRCTL_VERSION_STRING="${PROJECT_VERSION}")
add_executable(scrctl src/app/main.cpp)
target_link_libraries(scrctl PRIVATE scrctl_app CLI11::CLI11)
if(WIN32)
    target_link_libraries(scrctl PRIVATE shell32)
endif()
if(SCRCTL_BUILD_TESTS)
    enable_testing()
    foreach(name cli keyboard_state clipboard_job device_rotation viewgeom render)
        add_executable(${name}_test tests/${name}_test.cpp)
        target_link_libraries(${name}_test PRIVATE scrctl_app)
        add_test(NAME ${name} COMMAND ${name}_test)
        set_tests_properties(${name} PROPERTIES TIMEOUT 30)
    endforeach()
    add_executable(filesource_test tests/filesource_test.cpp src/app/FileSource.cpp)
    target_link_libraries(filesource_test PRIVATE scrctl_core)
    add_test(NAME filesource COMMAND filesource_test)
    set_tests_properties(filesource PROPERTIES TIMEOUT 30)
    add_executable(annexb_test tests/annexb_test.cpp)
    target_link_libraries(annexb_test PRIVATE scrctl_core)
    add_test(NAME annexb COMMAND annexb_test)
endif()
if(WIN32)
    include(cmake/WindowsRuntime.cmake)
elseif(APPLE)
    include(cmake/MacOSRuntime.cmake)
else()
    set_target_properties(scrctl PROPERTIES INSTALL_RPATH "$ORIGIN/lib")
    install(TARGETS scrctl RUNTIME DESTINATION bin)
    install(TARGETS xdb_client LIBRARY DESTINATION bin/lib)
endif()
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION share/doc/scrctl)
install(FILES docs/DDI.md docs/SOURCE_MATERIALS.md DESTINATION share/doc/scrctl)
if(WIN32)
    install(FILES docs/WINDOWS.md DESTINATION share/doc/scrctl)
elseif(APPLE)
    install(FILES docs/MACOS.md DESTINATION share/doc/scrctl)
endif()


if(SCRCTL_BUILD_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME i18n COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/i18n_test.py $<TARGET_FILE:scrctl>)
    add_test(NAME xdb_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/xdb_integration_test.py $<TARGET_FILE:scrctl>)
    set_tests_properties(xdb_integration PROPERTIES TIMEOUT 45)
endif()

include(cmake/SourceMaterials.cmake)

install(FILES xdb/LICENSE xdb/COPYING.GPLv3 DESTINATION share/licenses/XDeviceBridge)
