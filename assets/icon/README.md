# 图标工程

scrctl 的图标用 [VectorCraft](https://getartcraft.com/apps/vectorcraft) 维护。
构图:iMac(24 英寸)前视剪影,屏幕上是带灵动岛的 iPhone,寓意"在电脑上操作 iPhone";
透明底 glyph,无背景色块,适配命令行程序与跨平台场景。

## 文件

- `scrctl-icon.vectorcraft` — 主工程,选定方案 **c1(石墨机身 + 青绿渐变手机)**,在此细改。
- `scrctl-icon.svg` — 当前导出的 SVG 母版。
- `scrctl-icon-1024.png` — 当前导出的 1024 px 预览。

PNG 由 CMake 直接嵌入可执行程序，并作为 SDL 窗口图标（macOS 上为 Dock 图标）。
更新 PNG 后重新构建即可生效；安装和运行时不需要外部图标文件。

`.vectorcraft` 是 VectorCraft 的原生 JSON 文档,可直接用 VectorCraft 打开编辑,
也适合入库做版本对比。

## 配色(c1)

| 部位 | 颜色 |
| --- | --- |
| 机身 / 脚架 | `#34364A` 石墨 |
| 屏幕 / 灵动岛 | `#0F1020` |
| 手机 | 线性渐变 `#64D2FF` → `#30D158`(自上而下) |
| 下巴圆点 | `#21222E` |

画布 1024×1024:机身 152..872 × 96..676(圆角 36),屏幕内缩 22(圆角 16),
下巴高 108,脚架为梯形加圆角底条,整体下缘至 772。

## 再导出

编辑后在 VectorCraft 中导出,或用 CLI 批量转换:

```bash
vectorcraft-cli convert assets/icon/scrctl-icon.vectorcraft assets/icon/scrctl-icon.svg
vectorcraft-cli convert assets/icon/scrctl-icon.vectorcraft assets/icon/scrctl-icon-1024.png
```

小尺寸位图从 1024 px 导出后缩放即可。工程由 vectorcraft-cli 0.10.0 生成。
