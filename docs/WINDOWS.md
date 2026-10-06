# Windows 构建与验证

目前已验证 Windows 11 ARM64、MSYS2 CLANGARM64 工具链。x64 和 MSVC 尚未验证。
应用使用普通 `main`，视频使用 FFmpeg 软件解码，窗口使用 SDL2；当前没有 Windows 音频后端。

## 构建

安装 [MSYS2](https://www.msys2.org/)，打开 **CLANGARM64** 终端。不要在 MINGW64、UCRT64
或普通 MSYS 终端混用下面的依赖；CLANGARM64 生成原生 ARM64 程序。

```bash
pacman -S --needed \
  mingw-w64-clang-aarch64-clang mingw-w64-clang-aarch64-cmake \
  mingw-w64-clang-aarch64-ninja mingw-w64-clang-aarch64-openssl \
  mingw-w64-clang-aarch64-gettext-tools mingw-w64-clang-aarch64-SDL2 \
  mingw-w64-clang-aarch64-python mingw-w64-clang-aarch64-pkgconf \
  mingw-w64-clang-aarch64-ffmpeg mingw-w64-clang-aarch64-nlohmann-json \
  mingw-w64-clang-aarch64-pugixml

cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-win --parallel 4
ctest --test-dir build-win --output-on-failure
cmake --install build-win --prefix dist-win
```

CMake 缺少的固定源码依赖仍会从网络获取，离线配置方法见 README。
Windows 安装需要 CMake 3.21 以上；依赖扫描器会递归收集实际导入的 DLL，不复制
Windows 系统 DLL。其他工具链可通过 `SCRCTL_RUNTIME_DLL_DIRS` 补充 DLL 搜索目录，
这不意味着这些工具链已经通过验证。

保留安装目录中的 `bin` 与 `share`，可以整体移动目录。用 PowerShell 检查安装：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/test_windows_install.ps1 `
  -InstallDirectory dist-win -BuildDirectory build-win
```

该检查临时复制安装目录、隐藏开发构建的翻译目录，并将 PATH 限定为 Windows 系统目录。
它验证 exe 与 DLL 能独立启动，默认 auto 能按 `LC_ALL` 选择英文或中文，结束后恢复临时修改。
不会建立设备连接；上面的执行策略只适用于本次 PowerShell 进程。

## 设备连接

Wi-Fi 路径使用已有 RemotePairing 记录，设备和电脑之间需要能访问其监听端口：

```powershell
$env:LANG = 'zh_CN.UTF-8'
./dist-win/bin/scrctl.exe --wifi 192.168.1.50 --no-audio --stats
```

默认记录目录为 `%LOCALAPPDATA%\scrctl`；`XDG_DATA_HOME` 设置后使用其下的 `scrctl`
目录。新建记录文件从创建时就限制为当前用户访问，写完后替换目标文件；新建的叶子目录
也设置当前用户权限，不修改共享父目录或已有目录的权限。配对记录包含密钥，不要提交到仓库。
当前产品没有新建远程配对的命令，见路线图。

USB 适配连接 Apple Mobile Device Service 提供的 `127.0.0.1:27015` usbmux 服务。
仅在设备管理器看到 iPhone，不能证明该服务及开发者通道可用；还需要 Apple 的对应服务、
设备信任、开发者模式及匹配的 DDI。本轮 VM 没有该服务，因此原生 USB 路径尚未实测。

## 本轮结果

环境为 Windows 11 IoT Enterprise LTSC ARM64（26100）、Parallels、Clang 22.1.8，
设备为 iPhone14,4 / iOS 27.0。测试使用已有配对记录，未重新执行 SRP pair-setup。

- 原生 ARM64 完整构建及离线测试 24/24 通过；同一批改动的 macOS 回归 24/24 通过。
- Windows 本地 socket / PSK TLS 测试连续 10 次通过，包括中断阻塞接收。
  修复前 shutdown 单独调用有接收未退出的样本，补充 CancelIoEx 后通过。
- Wi-Fi 无窗口运行 20 秒，输出 315 帧，完成强制截图降级后恢复视频，正常退出。
- 移动后的安装产物在系统 PATH 下运行 SDL 窗口 25 秒，输出 229 帧；
  强制降级期间取得 18 张截图，随后恢复视频，截图失败为 0，正常退出。
- 安装启动检查覆盖 DLL、随包中文目录和默认 auto 选择。

这些是有限时长回归，不代表真实断网、物理拔插、多设备或长期运行已验证。
Windows 原生 USB、音频、输入控制和新建配对仍需分别验证。

## CI 产物

GitHub Actions 的 `windows-arm64` 作业使用 `windows-11-arm` 和 MSYS2 CLANGARM64，
执行构建、24 项离线测试、安装及独立启动检查。产物名为 `scrctl-windows-arm64`，
测试日志另外保存。当前配置在 VM 上完成了对应本地验证，尚未推送取得远端运行结果。

CI 包包含 DLL、翻译目录、已安装包版本、FFmpeg 构建配置、依赖许可文件和本项目源码快照。
MSYS2 当前 FFmpeg 包启用了 GPL 和 version3，属于 GPL-3.0-or-later 构建，不能将其标为
纯 LGPL 包。本项目的 Apache-2.0 声明不会覆盖随包依赖的许可；对外发布前还需准备与实际
二进制对应的第三方源码及构建材料，审核整个依赖组合的分发条件。当前 CI 产物用于开发验证。
