find_package(Threads REQUIRED)
include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

# 部分旧 libc++ 以实验开关提供已经标准化的 C++20 停止令牌。实际检查编译和
# 链接，再向使用公开 stop_token 接口的目标传递同一选项；不提高 macOS 下限。
set(scrctl_stop_token_check [[
    #include <stop_token>
    #include <thread>
    int main() {
        std::stop_source source;
        std::stop_callback callback(source.get_token(), [] {});
        source.request_stop();
        std::jthread worker([](std::stop_token) {});
        worker.request_stop();
        worker.join();
    }
]])
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES Threads::Threads)
check_cxx_source_compiles("${scrctl_stop_token_check}" SCRCTL_STANDARD_STOP_TOKEN)
if(NOT SCRCTL_STANDARD_STOP_TOKEN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_REQUIRED_FLAGS "-fexperimental-library")
    check_cxx_source_compiles("${scrctl_stop_token_check}" SCRCTL_EXPERIMENTAL_STOP_TOKEN)
endif()
cmake_pop_check_state()

if(NOT SCRCTL_STANDARD_STOP_TOKEN AND NOT SCRCTL_EXPERIMENTAL_STOP_TOKEN)
    message(FATAL_ERROR "The C++ standard library must support std::stop_token and std::jthread. Use a newer compiler and standard library.")
endif()

add_library(scrctl_threading INTERFACE)
add_library(Scrctl::Threading ALIAS scrctl_threading)
target_link_libraries(scrctl_threading INTERFACE Threads::Threads)
if(NOT SCRCTL_STANDARD_STOP_TOKEN)
    target_compile_options(scrctl_threading INTERFACE -fexperimental-library)
    target_link_options(scrctl_threading INTERFACE -fexperimental-library)
endif()
unset(scrctl_stop_token_check)
