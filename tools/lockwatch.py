# 看住设备的锁屏/灭屏事件，带时间戳。
#
# 为什么要它：媒体流在画面静止几秒后会被设备结束掉。一个没排除的解释是"那其实是
# 手机自己锁屏/灭屏了"。这两件事在设备侧各有自己的通知，把通知流和拆流时刻摆到
# 同一条时间轴上，谁是谁的原因一眼就能看出来；不看通知、只凭"静默了 3 秒"推理，
# 就永远是在猜。
#
# 观察的三个名字（SpringBoard 自己发的 Darwin 通知）：
#   com.apple.springboard.lockstate         锁状态变化，带 0/1
#   com.apple.springboard.lockcomplete      锁动画走完
#   com.apple.springboard.hasBlankedScreen  屏幕真的灭了，带 0/1
#
# 心跳每 5 秒打一行，用来把这条时间轴和另一路探针的时间轴对齐。
#
# 用法：python3 tools/lockwatch.py            # 一直跑到被 kill
import asyncio
import sys
import time

from pymobiledevice3.lockdown import create_using_usbmux
from pymobiledevice3.services.notification_proxy import NotificationProxyService

NAMES = [
    "com.apple.springboard.lockstate",
    "com.apple.springboard.lockcomplete",
    "com.apple.springboard.hasBlankedScreen",
]

T0 = time.monotonic()


def stamp() -> str:
    return "%8.2fs" % (time.monotonic() - T0)


def say(msg: str) -> None:
    print(f"{stamp()} {msg}", flush=True)


async def heartbeat() -> None:
    while True:
        await asyncio.sleep(5)
        say("心跳")


async def main() -> None:
    lockdown = await create_using_usbmux()
    say(f"已连接 {lockdown.product_type} / iOS {lockdown.product_version}")
    async with NotificationProxyService(lockdown) as proxy:
        for n in NAMES:
            await proxy.notify_register_dispatch(n)
        say(f"已登记 {len(NAMES)} 个通知，开始观察")
        beat = asyncio.create_task(heartbeat())
        try:
            async for note in proxy.receive_notification():
                say(f"通知 {note}")
        finally:
            beat.cancel()


try:
    asyncio.run(main())
except KeyboardInterrupt:
    sys.exit(0)
