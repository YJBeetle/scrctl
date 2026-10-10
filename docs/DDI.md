# 开发镜像（DDI）下载与安装

DDI（Developer Disk Image）提供设备端的开发服务。scrctl 的屏幕、截图、HID 和
剪贴板功能需要镜像包含相应的 CoreDevice 服务。设备能被枚举、配对或报告镜像
“兼容”，均不能保证这些服务已经可用。

## 用 scrctl 下载

```sh
scrctl --download-ddi -s <UDID>
scrctl --download-ddi --wifi auto -s <UDID>
scrctl --download-ddi --ddi-system-version 18.7.8 --ddi-product-type iPad11,2
scrctl --download-ddi --ddi-directory ./ddi-cache --ddi-download-timeout 300
```

此命令直接使用 libcurl 下载，不需要 Python 或 Xcode，也不会把 DDI 捆绑进安装包。
指定 `-s <UDID>` 时，通过 USB lockdown 只读查询设备的系统版本和机型，再选择
镜像类别并检查镜像的机型身份；这一步不要求设备已经安装 DDI。使用 `--wifi auto`
或手动 Wi-Fi 地址时，经已有配对记录连接并只读获取 RSD 中的设备属性；无线发现和
连接的通常前提仍适用。无设备时可以用
`--ddi-system-version` 和 `--ddi-product-type` 明确指定目标。裸 `--download-ddi`
保留原来的行为，下载固定的 iOS 27 Cryptex 镜像，不连接设备。
显式指定版本 / 机型与设备检测参数互斥，避免声明的目标与实际连接设备不一致。

完成后打印完整镜像目录和构建号，再次运行会校验并复用已有缓存。
`--ddi-directory` 指定缓存根目录，镜像存入版本子目录；相对路径以当前工作目录
为准。下载总预算默认 300 秒，
可设为 1–3600 秒，Ctrl+C 可取消。此命令不与屏幕镜像、配对或设备控制混用。

默认缓存根目录：

| 平台 | 目录 |
| --- | --- |
| 所有平台，设置了绝对路径 `XDG_CACHE_HOME` | `$XDG_CACHE_HOME/scrctl/ddi` |
| macOS | `$HOME/Library/Caches/scrctl/ddi` |
| Linux | `$HOME/.cache/scrctl/ddi` |
| Windows | `%LOCALAPPDATA%/scrctl/cache/ddi` |

## 系统版本与机型选择

| 目标系统 | 镜像类别 | 下载选择与限制 |
| --- | --- | --- |
| iOS / iPadOS 16 及以下 | Classic DDI | 按系统的主版本、次版本查找固定目录，例如 `16.6`；补丁号不单独选版。只有目录中存在的版本可下载，没有时明确报错，不回退到其他版本；没有机型 manifest，不据此保证目标机型可安装 |
| iOS / iPadOS 17–26 | Personalized DDI | 使用固定的 Personalized 镜像；目标机型及已取得的 `HardwareModel`、板号、芯片身份必须匹配同一个安装身份，再核对该身份引用的两个 payload。安装仍需个性化签名票据 |
| iOS / iPadOS 27 | Cryptex DDI | 使用固定的 Cryptex 镜像 `27A5228h`；校验通用 Cryptex 身份的四个 payload，指定机型时另查 manifest 声明的型号名单。此镜像已用于 `iPhone14,4` / iOS 27.0.1 恢复屏幕服务 |

当前 Classic 固定目录收录 40 个版本：`11.4`、`12.0`–`12.4`、`13.0`–`13.7`、
`14.0`–`14.8`、`15.0`–`15.8`、`16.0`–`16.7`。例如 `16.7.10` 选择 `16.7`，
但 `16.8` 不会回退到 `16.7`。

镜像来自第三方 DeveloperDiskImage 仓库，固定提交
[`6eae353ae694bda1c421d4a3eee5459ae59c99a1`](https://github.com/doronz88/DeveloperDiskImage/tree/6eae353ae694bda1c421d4a3eee5459ae59c99a1)。
Personalized 来源为 [Xcode_iOS_DDI_Personalized](https://github.com/doronz88/DeveloperDiskImage/tree/6eae353ae694bda1c421d4a3eee5459ae59c99a1/PersonalizedImages/Xcode_iOS_DDI_Personalized)，
Cryptex 来源为 [Xcode_iOS_DDI_Cryptex](https://github.com/doronz88/DeveloperDiskImage/tree/6eae353ae694bda1c421d4a3eee5459ae59c99a1/PersonalizedImages/Xcode_iOS_DDI_Cryptex)，
两者构建号均为 `27A5228h`。下载器固定各文件的尺寸和摘要，避免同一次下载混入
不同版本的文件。Classic 使用固定目录的
Git blob SHA-1 与尺寸检查，Personalized / Cryptex 使用固定 SHA-256；manifest 资产
再与其内部摘要和路径核对。Manifest 的 `ProductVersion=1.0` 是镜像自身版本，
不能用来匹配手机系统版本。尚未提供来源的系统版本不会猜测或自动下载其他类别。

Personalized 不能只依赖 `SupportedProductTypes` 名单：不同身份可能引用不同
镜像内容。未找到符合目标信息的身份，或其 payload 与实际下载文件不匹配时，
会明确拒绝。Cryptex 使用通用身份校验资产，机型检查按声明的型号名单执行，
不套用 Personalized 的板号 / 芯片身份筛选。未收录的目标型号保守拒绝；已校验
完整的缓存仍然保留，型号不兼容无需删除缓存。Classic 没有机型身份检查。
未指定机型的裸下载也不构成任何机型的兼容性结论。

**镜像选择、安装接受与产品功能是三个不同结果。** manifest 包含机型不代表它的
每个系统版本都能安装，也不代表该系统提供实时媒体等功能。Classic 下载用于准备
开发镜像，尚无证据证明 iOS 16 及以下具备 scrctl 依赖的 CoreDevice 服务，不能据此
宣称旧系统已支持镜像和控制。完整的真机支持范围见 [README](../README.md#ios--ipados-与机型)。

之前测试的 iPad mini 5（`iPad11,2` / iPadOS 18.7.8）应选择 Personalized，而不是
Cryptex；其目标身份与当前固定 payload 匹配。本轮没有重新安装或测试这台 iPad。
历史记录已经验证截图轮询、触摸、硬件按键、键盘和剪贴板；实时视频与
音频由设备明确拒绝并要求 iOS 27.0，下载或更换 DDI 不保证改变该能力门槛。

Classic 下载镜像及签名文件；Personalized 下载 `BuildManifest.plist`、`Image.dmg`
和 `Image.dmg.trustcache`；Cryptex 还包括 `Image.dmg.cryptex_info` 和
`Image.dmg.root_hash`。固定文件尺寸及摘要检查通过，Personalized / Cryptex 的
manifest 可解析且资产完整后，才将临时目录发布为完整缓存。缓存复用也重新检查
所有文件。HTTPS 校验证书，下载失败不会把半份镜像标为完成。

## 安装到设备

**下载命令尚不负责设备安装。** Personalized / Cryptex 安装还需要设备服务及个性化签名票据；
下载文件完整不代表设备允许安装。scrctl 运行时不会自动卸载已有镜像。

当前的安装方式使用独立的 pymobiledevice3 研究工具。它不随 scrctl
分发，也不是下载命令的运行依赖。先解锁并信任电脑、启用开发者模式；检查当前
挂载类型及 Cryptex 状态，显式选择设备：

```sh
pymobiledevice3 mounter list --udid <UDID>
PYMOBILEDEVICE3_UDID=<UDID> pymobiledevice3 cryptex list --userspace
```

### Personalized（包括已测试的 iPadOS 18）

先用 scrctl 下载目标系统与机型对应的镜像。然后将打印出的完整目录内三个文件
显式交给研究工具，位置参数依次是镜像、trustcache、manifest：

```sh
pymobiledevice3 mounter mount-personalized --udid <UDID> \
    <完整镜像目录>/Image.dmg \
    <完整镜像目录>/Image.dmg.trustcache \
    <完整镜像目录>/BuildManifest.plist
pymobiledevice3 mounter list --udid <UDID>
```

上述参数依据 [pymobiledevice3 11.26.0 挂载 CLI](https://github.com/doronz88/pymobiledevice3/blob/v11.26.0/pymobiledevice3/cli/mounter.py)，
本轮只核对下载内容与机型身份，没有用新缓存重做 iPad 的安装验收。安装之后重新
连接 scrctl，确认服务目录与所需功能。iPad mini 5 的历史实际结果见
[协议记录 §23、§24](coredevice.md)。也可以使用 Xcode 的设备管理界面或
`pymobiledevice3 mounter auto-mount --udid <UDID>`，但后者使用自己的镜像下载流程，
没有消费上面 scrctl 打印的目录。

### Cryptex（iOS 27）

以下替换流程已使用 pymobiledevice3 11.26.0 验证。

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
