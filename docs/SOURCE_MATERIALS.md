# 安装包对应源码与构建材料

macOS 和 Windows 的 CI 安装包分别配套同一轮运行的源码 artifact：

| 二进制 artifact | 对应源码 artifact | 源码归档 |
| --- | --- | --- |
| `scrctl-macos` | `scrctl-sources-macos` | `scrctl-sources-macos.tar` |
| `scrctl-windows-arm64` | `scrctl-sources-windows-arm64` | `scrctl-sources-windows.tar` |

请从**同一轮 GitHub Actions** 下载两个附件。安装目录的
`share/doc/scrctl/source-materials.json` 保存源码提交、源码归档 SHA256 和总清单
SHA256；源码清单逐文件记录路径、大小及 SHA256。源码收集失败时不会上传新的
二进制 artifact。发布到 GitHub Release 或其他下载站时，必须同时上传对应源码归档，
保留这些记录，不能仅提供上游首页或另一版本的源码。

这些材料包含源码和实际构建配方，不表示逐字节可复现的二进制构建已验证，也不代替
依赖许可组合的分发条件审核。当前 Homebrew/MSYS2 的 FFmpeg 配置可能启用 GPL
和 version3；应查阅该包的 `ffmpeg-build.txt` 与许可证，不能标为纯 LGPL 构建。
FFmpeg 官方要求源码准确对应分发的库，并提供编译方式与修改资料，见
[FFmpeg 分发说明](https://ffmpeg.org/legal.html)。

## 内容与版本对应

- `project/scrctl-source.tar`：安装包记录的同一 Git 提交，含 CMake、平台适配、
  lwIP 配置、源码收集工具和许可证。
- `build/`：实际依赖版本、库来源与哈希、FFmpeg 构建选项、CMakeCache、
  compile_commands 和 CI 工作流。缓存和命令中的绝对路径来自构建机，重建时应替换。
- `embedded/`：CMake 本次实际选择的 JSON、CLI11、pugixml、mDNS、lwIP 源码树，
  包括许可证、上游构建文件、每个源码归档及内部文件的哈希。安装包 CI 强制使用固定
  FetchContent 源，避免将系统包版本和固定下载版本混淆。本地源码覆盖导出的是实际
  覆盖目录，清单中的默认 URL 不作为覆盖源码身份的证明。
- `homebrew/`（macOS）：每个实际捆绑 dylib 所属安装 keg 的原始 formula、receipt、
  上游源码、resources 和外部/内嵌补丁。使用 Homebrew 自身的配方解析与下载接口，
  核对配方版本、已记录原库字节及下载 SHA256；Git 源必须固定提交，保存子模块和
  Git 元数据。配方内的 configure 参数和 `inreplace` 修改也完整保留。
- `msys2/`（Windows）：每个实际捆绑 DLL 所属包的精确 pkgbase/版本 source package，
  包含 PKGBUILD、`.SRCINFO`、源码和补丁，并保留签名和校验记录。使用安装二进制时
  相同的 MSYS2 信任 keyring 验证 detached signature，再核对源码包版本及声明的
  非 SKIP 校验值。一个 pkgbase 的不同拆分包共享同一份源码归档。

Windows 若原工具链缓存仍有对应二进制包，会进一步比对包中 DLL 字节并记录
`.BUILDINFO` 的 PKGBUILD 哈希；`recipe_binding` 会区分这一检查与仅根据精确
pkgbase/版本匹配。声明 SKIP 的 VCS 输入依赖签名源码包的快照，清单保留这一边界，
不把它描述成通过上游内容校验。

系统提供且没有捆绑的 macOS frameworks、Windows 系统 DLL 不收集源码。
通用编译器和全部构建工具的源码也不在此附件内；其版本、依赖声明和平台配方保留。
Ubuntu CI 当前没有发布独立 Linux 安装包，因此不生成 Linux 分发源码附件。

## 重建

1. GitHub Actions 下载的是 artifact 外层 ZIP，先解开它，再核对内部 `.tar`
   与安装包 `source-materials.json` 中的归档 SHA256，最后解开源码归档。
2. 解开 `project/scrctl-source.tar` 和 `embedded/*-source.tar`。
3. 根据平台目录的 README、原始 receipt/PKGBUILD 和 build 记录准备编译器、依赖。
   Homebrew 目录记录配方执行所用 Homebrew 版本和提交，提供缓存恢复方法；MSYS2
   source package 可解开后用 `makepkg-mingw` 按原 PKGBUILD 构建。不要将下载的
   未核验配方当作管理员脚本运行。
4. 配置 scrctl 时设置 `-DSCRCTL_FETCH_DEPENDENCIES=OFF`，按
   `embedded/rebuild-overrides.json` 指向解开的源码目录，同时保留原 CMake 选项。
   为使用这些源码，设置 `CMAKE_DISABLE_FIND_PACKAGE_nlohmann_json`、
   `CMAKE_DISABLE_FIND_PACKAGE_CLI11`、`CMAKE_DISABLE_FIND_PACKAGE_pugixml` 为 `ON`。
5. 执行 `cmake --build`；安装时保留翻译和运行库闭包。macOS install name 改写与
   ad hoc 签名步骤、Windows DLL 复制步骤在交付的 CMake 脚本和 CI 工作流内。

## 本地生成

先配置、构建并安装到独立目录，记录实际依赖及 FFmpeg 构建选项。
`CMAKE_EXPORT_COMPILE_COMMANDS=ON` 必须在配置时启用。

macOS：

```sh
HOMEBREW_DEVELOPER=1 HOMEBREW_NO_AUTO_UPDATE=1 \
  brew ruby tools/collect_macos_sources.rb dist-macos sources-macos/homebrew
python3 tools/collect_embedded_sources.py build sources-macos/embedded
python3 tools/package_source_materials.py dist-macos build sources-macos --platform macos
```

Windows（MSYS2 CLANGARM64，已初始化可信 pacman keyring）：

```sh
python tools/collect_windows_sources.py \
  dist-win/share/doc/scrctl/package-metadata.json sources-windows/msys2
python tools/collect_embedded_sources.py build-win sources-windows/embedded
python tools/package_source_materials.py dist-win build-win sources-windows --platform windows
```

先前的 CI 包没有这些源码附件，不能用新附件追认旧包。新工具需要构建时的原始
Homebrew keg 或 MSYS2 元数据；不能从另一台机器的当前包版本推断旧二进制来源。
