# BundleUtilities 在安装阶段扫描、复制并修复 Mach-O 依赖；独立 CLI 的 bundle
# 根就是 bin，所以库放在 bin/lib，避免它的安全检查拒绝修改根目录外的文件。
target_link_options(scrctl PRIVATE "-Wl,-headerpad_max_install_names")
install(CODE [[
    # 独立 CLI 会让 BundleUtilities 扫描整个 bin；提前拒绝共享前缀，避免
    # 用户误把 /usr/local/bin 或其他已有程序的目录当成可移动包来修复。
    set(scrctl_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
    file(GLOB scrctl_existing_items "${scrctl_root}/bin/*")
    foreach(scrctl_item IN LISTS scrctl_existing_items)
        get_filename_component(scrctl_name "${scrctl_item}" NAME)
        if(NOT scrctl_name STREQUAL "scrctl" AND NOT scrctl_name STREQUAL "lib")
            message(FATAL_ERROR "macOS packaging requires a standalone install prefix; unrelated item: ${scrctl_item}")
        endif()
    endforeach()
    file(GLOB scrctl_existing_libs "${scrctl_root}/bin/lib/*")
    if(scrctl_existing_libs AND NOT EXISTS "${scrctl_root}/share/doc/scrctl/runtime-origins.tsv")
        message(FATAL_ERROR "Refusing to fix existing libraries without scrctl package provenance: ${scrctl_root}/bin/lib")
    endif()
]])
install(TARGETS scrctl RUNTIME DESTINATION bin)

# Homebrew 的 sdl2 可能指向 sdl2-compat；它通过 dlopen 而非链接加载 SDL3。
# 这种依赖不会出现在 otool -L 中，必须按上游的 @loader_path/libSDL3.dylib
# 搜索规则显式安装，再让 BundleUtilities 递归处理 SDL3 本身的依赖。
set(scrctl_sdl3_runtime "")
if(SDL2_DIR)
    get_filename_component(scrctl_sdl2_config "${SDL2_DIR}" REALPATH)
    if(scrctl_sdl2_config MATCHES "/sdl2-compat/")
        find_package(SDL3 REQUIRED CONFIG)
        set(scrctl_sdl3_runtime "$<TARGET_FILE:SDL3::SDL3>")
    endif()
endif()

# 这里只写安装脚本，不在 configure 阶段加载 BundleUtilities。保留原始库路径，
# 供产物元数据记录实际使用的 Homebrew keg、版本与许可文件。
install(CODE "set(scrctl_sdl3_origin \"${scrctl_sdl3_runtime}\")")
install(CODE [[
    include(BundleUtilities)
    set(BU_CHMOD_BUNDLE_ITEMS ON)
    function(gp_item_default_embedded_path_override item default_embedded_path_var)
        set(${default_embedded_path_var} "@executable_path/lib" PARENT_SCOPE)
    endfunction()

    set(scrctl_root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
    set(scrctl_exe "${scrctl_root}/bin/scrctl")
    set(scrctl_plugins "")
    file(MAKE_DIRECTORY "${scrctl_root}/bin/lib" "${scrctl_root}/share/doc/scrctl")
    if(scrctl_sdl3_origin)
        # Imported target 的路径也可能是 Homebrew 前缀中的符号链接，必须复制
        # realpath 的内容，避免安装一个指回包外 Cellar 的失效链接。
        get_filename_component(scrctl_sdl3_origin "${scrctl_sdl3_origin}" REALPATH)
        file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin/lib" TYPE FILE
            FILES "${scrctl_sdl3_origin}" RENAME libSDL3.dylib)
        list(APPEND scrctl_plugins "${scrctl_root}/bin/lib/libSDL3.dylib")
    endif()
    get_bundle_keys("${scrctl_exe}" "${scrctl_plugins}" "" scrctl_keys)
    set(scrctl_origins "library\toriginal_path\n")
    foreach(scrctl_key IN LISTS scrctl_keys)
        if(NOT scrctl_key STREQUAL "scrctl")
            get_filename_component(scrctl_name "${${scrctl_key}_RESOLVED_EMBEDDED_ITEM}" NAME)
            set(scrctl_origin "${${scrctl_key}_RESOLVED_ITEM}")
            # BundleUtilities 为保留 symlink 链，会把 COPYFLAG=2 的来源改为
            # 包内目标。追溯目标 key 的原始来源，避免元数据误记成安装目录。
            if("${${scrctl_key}_COPYFLAG}" STREQUAL "2")
                foreach(scrctl_target_key IN LISTS scrctl_keys)
                    if("${${scrctl_target_key}_RESOLVED_EMBEDDED_ITEM}" STREQUAL scrctl_origin)
                        set(scrctl_origin "${${scrctl_target_key}_RESOLVED_ITEM}")
                        break()
                    endif()
                endforeach()
            endif()
            get_filename_component(scrctl_origin "${scrctl_origin}" REALPATH)
            if(scrctl_name STREQUAL "libSDL3.dylib" AND scrctl_sdl3_origin)
                get_filename_component(scrctl_origin "${scrctl_sdl3_origin}" REALPATH)
            endif()
            string(APPEND scrctl_origins "${scrctl_name}\t${scrctl_origin}\n")
        endif()
    endforeach()
    file(WRITE "${scrctl_root}/share/doc/scrctl/runtime-origins.tsv" "${scrctl_origins}")
    clear_bundle_keys(scrctl_keys)
    # 此成熟工具统一递归拷贝库、改写 install name、删除旧 rpath，并验证依赖。
    fixup_bundle("${scrctl_exe}" "${scrctl_plugins}" "")

    # install_name_tool 会使原有签名失效；Apple Silicon 要求有效的本地签名。
    # 仅重新签署包内副本，不改变 Homebrew、系统安全策略或任何系统库。
    find_program(scrctl_codesign codesign REQUIRED)
    file(GLOB_RECURSE scrctl_dylibs "${scrctl_root}/bin/lib/*.dylib")
    foreach(scrctl_binary IN LISTS scrctl_dylibs scrctl_exe)
        if(NOT IS_SYMLINK "${scrctl_binary}")
            execute_process(COMMAND "${scrctl_codesign}" --force --sign - "${scrctl_binary}"
                RESULT_VARIABLE scrctl_sign_result ERROR_VARIABLE scrctl_sign_error)
            if(NOT scrctl_sign_result EQUAL 0)
                message(FATAL_ERROR "codesign failed for ${scrctl_binary}: ${scrctl_sign_error}")
            endif()
        endif()
    endforeach()
]])
install(DIRECTORY "${PROJECT_SOURCE_DIR}/cmake/licenses/"
    DESTINATION share/doc/scrctl/dependency-licenses/gnu)
