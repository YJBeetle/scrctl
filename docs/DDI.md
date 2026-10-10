# 开发镜像（DDI）下载与安装

DDI（Developer Disk Image）提供设备端的开发服务。scrctl 的屏幕、截图、HID 和
剪贴板功能需要镜像包含相应的 CoreDevice 服务。设备能被枚举、配对或报告镜像
“兼容”，均不能保证这些服务已经可用。

## 用 scrctl 下载

```sh
scrctl --download-ddi
scrctl --download-ddi --ddi-directory ./ddi-cache --ddi-download-timeout 300
```

此命令直接使用 libcurl 下载，不需要 Python、Xcode 或连接手机。完成后打印完整
镜像目录和构建号，再次运行会校验并复用已有缓存。`--ddi-directory` 指定缓存根
目录，镜像存入版本子目录；相对路径以当前工作目录为准。下载总预算默认 300 秒，
可设为 1–3600 秒，Ctrl+C 可取消。此命令仅接受上述选项及 `--lang`，不与镜像、
配对或设备控制混用。

默认缓存根目录：

| 平台 | 目录 |
| --- | --- |
| 所有平台，设置了绝对路径 `XDG_CACHE_HOME` | `$XDG_CACHE_HOME/scrctl/ddi` |
| macOS | `$HOME/Library/Caches/scrctl/ddi` |
| Linux | `$HOME/.cache/scrctl/ddi` |
| Windows | `%LOCALAPPDATA%/scrctl/cache/ddi` |

首版下载固定的 **Cryptex DDI / 27A5228h**，来自第三方
[DeveloperDiskImage 仓库的固定提交](https://github.com/doronz88/DeveloperDiskImage/tree/6eae353ae694bda1c421d4a3eee5459ae59c99a1/PersonalizedImages/Xcode_iOS_DDI_Cryptex)。
这是我们已用于 iPhone14,4 / iOS 27.0.1 恢复屏幕服务的版本，不自动选择未知镜像，
不承诺适用于所有 iOS / iPadOS 版本。Manifest 的 `ProductVersion=1.0` 是镜像自身
版本，不能用来匹配手机系统版本。

下载内容为 `BuildManifest.plist`、`Image.dmg`、`Image.dmg.trustcache`、
`Image.dmg.cryptex_info` 和 `Image.dmg.root_hash`。固定文件尺寸及 SHA-256 检查通过、
manifest 可解析且 Cryptex 资产完整后，才将临时目录发布为完整缓存。缓存复用也
重新检查所有文件。HTTPS 校验证书，下载失败不会把半份镜像标为完成。

## 安装到设备

**下载命令尚不负责设备安装。** Cryptex 安装还需要设备服务及个性化签名票据；
下载文件完整不代表设备允许安装。scrctl 运行时不会自动卸载已有镜像。

当前已验证的安装方式使用独立的 pymobiledevice3 11.26.0 研究工具。它不随 scrctl
分发，也不是下载命令的运行依赖。先解锁并信任电脑、启用开发者模式；检查当前
挂载类型及 Cryptex 状态，显式选择设备：

```sh
pymobiledevice3 mounter list --udid <UDID>
PYMOBILEDEVICE3_UDID=<UDID> pymobiledevice3 cryptex list --userspace
```

如果存在旧 Personalized 镜像且缺少需要的服务，先退出 Xcode，避免它立即重挂旧
镜像；确认完整下载目录已经准备好，再卸载对应旧镜像并安装：

```sh
pymobiledevice3 mounter umount-personalized --udid <UDID>
PYMOBILEDEVICE3_UDID=<UDID> pymobiledevice3 cryptex auto-install \
    --userspace --restore-dir <scrctl打印的完整镜像目录>
PYMOBILEDEVICE3_UDID=<UDID> pymobiledevice3 cryptex list --userspace
```

若已安装 Cryptex，按实际 identifier / version 处理，不套用 Personalized 卸载命令。
Windows PowerShell 通过 `$env:PYMOBILEDEVICE3_UDID='<UDID>'` 设置同一进程环境变量，
再运行 `pymobiledevice3 cryptex ...`。安装完成后重新连接 scrctl，检查屏幕服务是否
恢复。曾观察到 `auto-mount` 在旧镜像已挂载时直接成功返回，因此退出码 0 不能
单独证明镜像已更新。

实测记录见 [协议记录 §34](coredevice.md#34-ios-2701-更新后的开发镜像准备)。
