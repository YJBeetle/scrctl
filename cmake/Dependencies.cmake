include(FetchContent)
option(SCRCTL_FETCH_DEPENDENCIES "下载缺失的依赖（固定版本及 SHA256）" ON)

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

find_package(CLI11 2.5.0 QUIET CONFIG)
if(NOT TARGET CLI11::CLI11)
    if(NOT SCRCTL_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "需要 CLI11 >= 2.5.0；请安装或启用 SCRCTL_FETCH_DEPENDENCIES")
    endif()
    set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(cli11
        URL https://codeload.github.com/CLIUtils/CLI11/tar.gz/refs/tags/v2.5.0
        URL_HASH SHA256=17e02b4cddc2fa348e5dbdbb582c59a3486fa2b2433e70a0c3bacb871334fd55)
    FetchContent_MakeAvailable(cli11)
endif()

find_package(pugixml 1.16 QUIET CONFIG)
if(NOT TARGET pugixml::pugixml)
    if(NOT SCRCTL_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "需要 pugixml >= 1.16；请安装或启用 SCRCTL_FETCH_DEPENDENCIES")
    endif()
    set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(pugixml
        URL https://codeload.github.com/zeux/pugixml/tar.gz/refs/tags/v1.16
        URL_HASH SHA256=357bcab8877dc9943f355d3a72daba1b053238ba955f50fa81586afb65090219)
    FetchContent_MakeAvailable(pugixml)
endif()
