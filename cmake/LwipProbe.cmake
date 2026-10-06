add_executable(lwip_probe tools/lwip_probe.cpp)
target_link_libraries(lwip_probe PRIVATE scrctl_core scrctl_lwip)
target_compile_options(lwip_probe PRIVATE -Wall -Wextra)
