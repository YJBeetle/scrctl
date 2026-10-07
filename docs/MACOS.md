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
SDL3 动态加载、英文/中文/auto 与版本输出，退出时恢复构建目录。该检查不连接设备；
设备媒体、音频、输入与长期运行仍需要各自的运行验证。包的架构以
`package-metadata.json` 为准；CI 使用当前 `macos-latest` 架构，不生成 universal 包。

GitHub Actions 的 macOS 作业在离线自检后执行同样的安装、记录和搬移检查。
`scrctl-macos` artifact 中的 `scrctl-macos-<架构>.tar.gz` 保留可执行权限和符号链接；
解压后直接运行 `bin/scrctl`。测试日志单独上传。下载包仍受 macOS 正常的安全检查，
当前开发产物没有 Developer ID 签名或公证。

包内记录实际 dylib 来源与 SHA256、架构、源码提交、Homebrew keg 版本、原始安装
receipt 与 formula，以及 keg 提供的许可正文。本项目静态依赖的许可见
`THIRD_PARTY_NOTICES.md`；`ffmpeg-build.txt` 记录 FFmpeg 的完整构建选项。Homebrew
FFmpeg 可能启用 GPL/version3，必须按本次构建配置与随包依赖的实际许可处理，不能
笼统声明为 LGPL 包。上述记录与本项目源码快照用于开发验证，并不替代对应第三方
源码、构建材料或分发条件审核。
