#!/usr/bin/env python3
"""CoreDevice DDI 服务探针：验证第三方进程能否截图 + 注入触摸。

单条隧道内做多 operations，避免每次重建隧道（约 60s）导致观察不到瞬时效应。
"""
from __future__ import annotations

import asyncio
import struct
import sys
import time

from pymobiledevice3.remote.core_device.hid_service import (
    TOUCHSCREEN_STATE_CONTACT,
    TOUCHSCREEN_STATE_RELEASE,
    IndigoHIDService,
    touch_session,
)
from pymobiledevice3.remote.core_device.screen_capture_service import ScreenCaptureService
from pymobiledevice3.remote.userspace_tunnel import establish_userspace_rsd

from device import resolve_udid

# iPhone 13 mini 实测 (get-display-info): 1125x2436 @ UIScale 3
SCREEN_W, SCREEN_H = 1125, 2436
NORM_MAX = 65535

BUTTONS = {
    "home": (0x0C, 0x40),
    "lock": (0x0C, 0x30),
    "volup": (0x0C, 0xE9),
    "voldn": (0x0C, 0xEA),
    "mute": (0x0C, 0xE2),
}


def to_norm(x: float, y: float) -> tuple[int, int]:
    """屏幕像素 -> HID 归一化 UInt16 坐标 (0..65535)。"""
    return (
        max(0, min(NORM_MAX, int(x * NORM_MAX / SCREEN_W))),
        max(0, min(NORM_MAX, int(y * NORM_MAX / SCREEN_H))),
    )


def png_size(path: str) -> tuple[int, int]:
    with open(path, "rb") as f:
        head = f.read(24)
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n":
        return (0, 0)
    return struct.unpack(">II", head[16:24])


async def do_shot(tag: str) -> str:
    rsd = STATE["rsd"]
    async with ScreenCaptureService(rsd) as svc:
        t0 = time.monotonic()
        res = await svc.capture_screenshot()
        ms = (time.monotonic() - t0) * 1000
    path = f"{OUTDIR}/shot_{tag}.png"
    with open(path, "wb") as f:
        f.write(res["image"])
    w, h = png_size(path)
    print(f"  [shot] {path}  {w}x{h}  {len(res['image'])} bytes  {ms:.0f}ms")
    return path


async def do_tap(x: float, y: float, hold: float = 0.09) -> None:
    nx, ny = to_norm(x, y)
    svc = STATE["hid"]
    await svc.send_touchscreen(TOUCHSCREEN_STATE_CONTACT, nx, ny)
    await asyncio.sleep(hold)
    await svc.send_touchscreen(TOUCHSCREEN_STATE_RELEASE, nx, ny)
    print(f"  [tap] ({x:g},{y:g})px -> ({nx},{ny})norm  hold={hold*1000:.0f}ms")


async def do_swipe(x1, y1, x2, y2, duration: float = 0.35, steps: int = 24) -> None:
    svc = STATE["hid"]
    ax, ay = to_norm(x1, y1)
    bx, by = to_norm(x2, y2)
    await svc.send_touchscreen(TOUCHSCREEN_STATE_CONTACT, ax, ay)
    await asyncio.sleep(0.03)
    for i in range(1, steps + 1):
        k = i / steps
        await svc.send_touchscreen(
            TOUCHSCREEN_STATE_CONTACT,
            int(ax + (bx - ax) * k),
            int(ay + (by - ay) * k),
        )
        await asyncio.sleep(duration / steps)
    await asyncio.sleep(0.03)
    await svc.send_touchscreen(TOUCHSCREEN_STATE_RELEASE, bx, by)
    print(f"  [swipe] ({x1:g},{y1:g}) -> ({x2:g},{y2:g})  {duration*1000:.0f}ms/{steps}步")


async def do_button(name: str) -> None:
    page, code = BUTTONS[name]
    svc = IndigoHIDService(STATE["rsd"])
    async with svc:
        await svc.send_button(page, code, 1)  # DOWN
        await asyncio.sleep(0.05)
        await svc.send_button(page, code, 0)  # UP
    print(f"  [button] {name} (page=0x{page:02X} code=0x{code:02X})")


STATE: dict = {}
OUTDIR = "."


HELP = """
命令:
  shot [tag]              截图到 probe/shot_<tag>.png
  tap <x> <y> [hold_ms]   点击（像素坐标，屏幕 1125x2436）
  center                  点击屏幕正中
  swipe <x1> <y1> <x2> <y2> [ms]   滑动
  draw <x1> <y1> <x2> <y2>         慢速拖动（画图程序会留下可见线条）
  down / up / left        屏幕方向滑动
  home | lock | volup | voldn | mute   硬件按键
  size <w> <h>            改屏幕分辨率
  quit
"""


async def repl() -> None:
    global SCREEN_W, SCREEN_H
    print(HELP)
    while True:
        try:
            line = await asyncio.to_thread(input, "probe> ")
        except EOFError:
            break
        parts = line.split()
        if not parts:
            continue
        cmd, a = parts[0], parts[1:]
        try:
            if cmd == "quit" or cmd == "q":
                break
            elif cmd == "shot":
                await do_shot(a[0] if a else time.strftime("%H%M%S"))
            elif cmd == "tap":
                hold = float(a[2]) / 1000 if len(a) > 2 else 0.09
                await do_tap(float(a[0]), float(a[1]), hold)
            elif cmd == "center":
                await do_tap(SCREEN_W / 2, SCREEN_H / 2)
            elif cmd in ("swipe", "draw"):
                dur = (float(a[4]) / 1000) if len(a) > 4 and cmd == "swipe" else 1.2
                await do_swipe(*map(float, a[:4]), duration=dur,
                               steps=40 if cmd == "draw" else 24)
            elif cmd == "down":
                await do_swipe(SCREEN_W / 2, SCREEN_H * 0.3, SCREEN_W / 2, SCREEN_H * 0.75)
            elif cmd == "up":
                await do_swipe(SCREEN_W / 2, SCREEN_H * 0.7, SCREEN_W / 2, SCREEN_H * 0.25)
            elif cmd == "left":
                await do_swipe(SCREEN_W * 0.8, SCREEN_H / 2, SCREEN_W * 0.2, SCREEN_H / 2)
            elif cmd in BUTTONS:
                await do_button(cmd)
            elif cmd == "size":
                SCREEN_W, SCREEN_H = int(a[0]), int(a[1])
                print(f"  screen = {SCREEN_W}x{SCREEN_H}")
            else:
                print("  未知命令，输入 ? 看帮助")
        except Exception as exc:
            print(f"  !! {type(exc).__name__}: {exc}")


async def main() -> None:
    global OUTDIR
    OUTDIR = sys.argv[1] if len(sys.argv) > 1 else "."

    print("[1/3] 建立用户态隧道 (无需 root) ...")
    t0 = time.monotonic()
    STATE["rsd"] = await establish_userspace_rsd(await resolve_udid())
    print(f"      完成 {time.monotonic()-t0:.1f}s")

    print("[2/3] 打开 touch_session（启动视频流以解锁 backboardd 注入门）...")
    ctx = touch_session(STATE["rsd"])
    STATE["hid"] = await ctx.__aenter__()
    print("      完成 — 触摸 report 现在可达 UIKit")

    try:
        print("[3/3] 进入交互模式\n")
        await repl()
    finally:
        await ctx.__aexit__(None, None, None)


if __name__ == "__main__":
    import logging
    logging.disable(logging.INFO)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
