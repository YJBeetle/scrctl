"""设备发现：不硬编码任何设备标识。

优先级：显式参数 > SCRCTL_UDID 环境变量 > 唯一 USB 设备自动发现。
"""
from __future__ import annotations

import os
import sys

from pymobiledevice3 import usbmux


async def resolve_udid(arg: str | None = None) -> str:
    if arg:
        return arg
    env = os.environ.get("SCRCTL_UDID")
    if env:
        return env

    devices = await usbmux.list_devices()
    usb = [d for d in devices if d.is_usb]
    if not usb:
        sys.exit("没有 USB 连接的设备。插上手机，或用 --udid / SCRCTL_UDID 指定。")
    if len(usb) > 1:
        print("检测到多台 USB 设备，请用 --udid 指定其一：", file=sys.stderr)
        for d in usb:
            print(f"  {d.serial}", file=sys.stderr)
        sys.exit(1)
    return usb[0].serial
