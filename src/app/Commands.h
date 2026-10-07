#pragma once

#include <optional>
#include <string>

namespace scrctl::remote {
class Device;
}

namespace scrctl::app {

struct Options;

/// 执行版本、设备列表、应用列表或剪贴板命令，返回退出码。
/// 没有选择独立命令时返回 nullopt，由调用方继续视频流程。
std::optional<int> run_standalone_command(const Options &options);

/// 解析 --start-app 的前缀和名称，在已有设备会话中启动应用。
int launch_app(remote::Device &device, const std::string &app_spec);

} // namespace scrctl::app
