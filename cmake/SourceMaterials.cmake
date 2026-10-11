# Capture the dependency selection made by this configure, rather than guessing
# versions later from the download declarations or from an installed executable.
function(scrctl_source_json_string output value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\t" "\\t" value "${value}")
    set(${output} "\"${value}\"" PARENT_SCOPE)
endfunction()

function(scrctl_source_dependency name target detected_version declared_url declared_ref)
    FetchContent_GetProperties(${name} SOURCE_DIR selected_source)
    set(item "{}")
    foreach(key IN ITEMS name target detected_version declared_url declared_ref)
        scrctl_source_json_string(value "${${key}}")
        string(JSON item SET "${item}" ${key} "${value}")
    endforeach()
    if(selected_source)
        set(selection "source-tree")
        scrctl_source_json_string(value "${selected_source}")
        string(JSON item SET "${item}" source_directory "${value}")
        string(TOUPPER "${name}" upper_name)
        if(FETCHCONTENT_SOURCE_DIR_${upper_name})
            string(JSON item SET "${item}" source_override true)
        else()
            string(JSON item SET "${item}" source_override false)
        endif()
    else()
        set(selection "system-package")
    endif()
    scrctl_source_json_string(value "${selection}")
    string(JSON item SET "${item}" selection "${value}")
    foreach(property IN ITEMS INTERFACE_INCLUDE_DIRECTORIES IMPORTED_LOCATION IMPORTED_IMPLIB
            IMPORTED_LOCATION_RELEASE IMPORTED_IMPLIB_RELEASE)
        get_target_property(paths "${target}" "${property}")
        set(values "[]")
        if(paths AND NOT paths MATCHES "-NOTFOUND$")
            foreach(path IN LISTS paths)
                scrctl_source_json_string(value "${path}")
                string(JSON length LENGTH "${values}")
                string(JSON values SET "${values}" ${length} "${value}")
            endforeach()
        endif()
        string(JSON item SET "${item}" ${property} "${values}")
    endforeach()
    get_property(dependencies GLOBAL PROPERTY SCRCTL_SOURCE_MATERIAL_DEPENDENCIES)
    if(NOT dependencies)
        set(dependencies "[]")
    endif()
    string(JSON length LENGTH "${dependencies}")
    string(JSON dependencies SET "${dependencies}" ${length} "${item}")
    set_property(GLOBAL PROPERTY SCRCTL_SOURCE_MATERIAL_DEPENDENCIES "${dependencies}")
endfunction()

scrctl_source_dependency(nlohmann_json nlohmann_json::nlohmann_json "${nlohmann_json_VERSION}"
    "https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0"
    "SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187")
scrctl_source_dependency(cli11 CLI11::CLI11 "${CLI11_VERSION}"
    "https://codeload.github.com/CLIUtils/CLI11/tar.gz/refs/tags/v2.5.0"
    "SHA256=17e02b4cddc2fa348e5dbdbb582c59a3486fa2b2433e70a0c3bacb871334fd55")
scrctl_source_dependency(pugixml pugixml::pugixml "${pugixml_VERSION}"
    "https://codeload.github.com/zeux/pugixml/tar.gz/refs/tags/v1.16"
    "SHA256=357bcab8877dc9943f355d3a72daba1b053238ba955f50fa81586afb65090219")
scrctl_source_dependency(mdns ScrctlMdns ""
    "https://codeload.github.com/mjansson/mdns/tar.gz/refs/tags/1.4.3"
    "SHA256=be1fd8e35599cb7de179decbd0633c121d11a2dcb9cc193ff5c590bd0d480483")
scrctl_source_dependency(lwip scrctl_lwip ""
    "https://codeload.github.com/lwip-tcpip/lwip/tar.gz/refs/tags/STABLE-2_2_1_RELEASE"
    "SHA256=ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539")
if(SCRCTL_FETCH_SDL2)
    scrctl_source_dependency(sdl2 SDL2::SDL2 "${SDL_VERSION}"
        "https://github.com/libsdl-org/SDL.git" "release-2.32.10")
endif()

get_property(dependencies GLOBAL PROPERTY SCRCTL_SOURCE_MATERIAL_DEPENDENCIES)
set(manifest "{\"schema_version\":1}")
string(JSON manifest SET "${manifest}" dependencies "${dependencies}")
string(JSON manifest SET "${manifest}" build "{}")
foreach(key IN ITEMS CMAKE_GENERATOR CMAKE_BUILD_TYPE CMAKE_C_COMPILER CMAKE_C_COMPILER_ID
        CMAKE_C_COMPILER_VERSION CMAKE_CXX_COMPILER CMAKE_CXX_COMPILER_ID
        CMAKE_CXX_COMPILER_VERSION CMAKE_SYSTEM_NAME CMAKE_SYSTEM_PROCESSOR
        CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES)
    scrctl_source_json_string(value "${${key}}")
    string(JSON manifest SET "${manifest}" build ${key} "${value}")
endforeach()
scrctl_source_json_string(value "${PROJECT_SOURCE_DIR}")
string(JSON manifest SET "${manifest}" project_source_directory "${value}")
file(WRITE "${CMAKE_BINARY_DIR}/source-materials-manifest.json" "${manifest}\n")
