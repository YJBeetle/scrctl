include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
option(SCRCTL_FETCH_DEPENDENCIES "下载缺失的 JSON 依赖（固定版本及 SHA256）" ON)

find_package(nlohmann_json 3.12.0 QUIET CONFIG)
if(NOT TARGET nlohmann_json::nlohmann_json)
    if(NOT SCRCTL_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "需要 nlohmann_json >= 3.12.0；请安装或启用 SCRCTL_FETCH_DEPENDENCIES")
    endif()
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
        URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187)
    FetchContent_MakeAvailable(nlohmann_json)
endif()
