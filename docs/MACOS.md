# macOS 构建、安装与 CI 包

使用 Xcode Command Line Tools、CMake、Python 3 和 Homebrew 依赖：

```bash
brew install sdl2 openssl ffmpeg pkg-config gettext
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl)"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix dist-macos
python3 tools/record_macos_package.py dist-macos --source-commit "$(git rev-parse HEAD)"
ffmpeg -version > dist-macos/share/doc/scrctl/ffmpeg-build.txt
python3 tools/test_macos_install.py dist-macos build
```

安装目录包含 `bin/scrctl`、`bin/lib` 的非系统 dylib、`share/locale` 的翻译与
`share/doc/scrctl` 的许可和构建记录。整体保留目录结构即可移动，运行
`./dist-macos/bin/scrctl --help`。默认语言为 `auto`，根据 `LC_ALL`、`LC_MESSAGES`、
`LANG` 的优先级选择中文或英文，也可用 `--lang en` 或 `--lang zh-CN` 指定。

CMake 的 `BundleUtilities` 在安装阶段递归收集链接依赖、改写 install name、移除
构建和 Homebrew rpath，并验证依赖闭包。安装前缀需要使用单独目录；`bin` 中含有
其他程序时会拒绝打包。Homebrew 的 `sdl2-compat` 通过 `dlopen`
加载 SDL3，因此额外安装这个运行时。所有 Mach-O 副本随后进行 ad hoc 签名；系统库
和 Homebrew 原件不会被修改。自定义、非 Homebrew 的 SDL 动态插件需要另行确认。

检查脚本将安装目录复制到带空格的新路径，临时隐藏构建目录的翻译，清除 dyld 与
语言覆盖并限制 PATH。它检查包内签名、所有 install name、实际 dyld 加载路径、
SDL3 动态加载、英文/中文/auto 与版本输出，退出时恢复构建目录。检查还使用 Apple
`otool -l` 读取主程序和每个 dylib 的 `LC_BUILD_VERSION` 或旧版
`LC_VERSION_MIN_MACOSX`；任一文件的最低系统版本高于当前 macOS 时直接失败，
即使该文件碰巧能够运行到 `--help`。该检查不连接设备；
设备媒体、音频、输入与长期运行仍需要各自的运行验证。包的架构以
`package-metadata.json` 为准；CI 固定使用 `macos-15` 的标准 ARM64 runner，
不生成 universal 包。GitHub 的 Intel runner 标签是 `macos-15-intel`，见
[官方 runner 列表](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)。

GitHub Actions 的 macOS 作业在离线自检后执行同样的安装、记录和搬移检查。
`scrctl-macos` artifact 中的 `scrctl-macos-<架构>.tar.gz` 保留可执行权限和符号链接；
解压后直接运行 `bin/scrctl`。测试日志单独上传。下载包仍受 macOS 正常的安全检查，
当前开发产物没有 Developer ID 签名或公证。

`package-metadata.json` 的 `executable_minimum_macos` 记录主程序最低版本，
每个 `runtime_dependencies` 条目记录该 dylib 的 `minimum_macos`，顶层
`minimum_macos` 取整个包的最高要求。支持范围必须满足这份真实最低版本记录，
不能只根据 runner 名称、配置参数或旧系统上的启动成功来判断。macOS 15.8 是当前
本机验证目标；旧 macOS 26 runner 产物的主程序及库均声明最低 26.0，应按这个要求
处理，不能用此前的 `--help` / `--version` 成功当成 macOS 15 分发验证。

2026-10-07 下载的 [03c2f41 CI 包](https://github.com/YJBeetle/scrctl/actions/runs/37585863387/artifacts/11467220946)
在本机 macOS 15.8 完成搬移检查：程序及包内所有库的最低要求均为 15.0，实际加载
来自搬移目录，签名、权限、符号链接、中英文和自动语言选择通过。此结果不覆盖
macOS 14 或更旧系统，也没有额外连接设备。

包内还记录实际 dylib 来源与 SHA256、架构、源码提交、Homebrew keg 版本、原始安装
receipt 与 formula，以及 keg 提供的许可正文。本项目静态依赖的许可见
`THIRD_PARTY_NOTICES.md`；`ffmpeg-build.txt` 记录 FFmpeg 的完整构建选项。Homebrew
FFmpeg 可能启用 GPL/version3，必须按本次构建配置与随包依赖的实际许可处理，不能
笼统声明为 LGPL 包。上述记录与本项目源码快照用于开发验证，并不替代对应第三方
源码、构建材料或分发条件审核。
