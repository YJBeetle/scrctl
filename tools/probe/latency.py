#!/usr/bin/env python3
"""测量 CoreDevice displayservice 视频流的真实帧率与 输入->画面 延迟。

结构刻意与 scrctl 生产版一致：单进程内同时持有视频流与 HID 注入通道
（视频流本身就是 backboardd 的 HID 授权门）。
"""
from __future__ import annotations

import asyncio
import contextlib
import statistics
import struct
import time
from pathlib import Path

import av
import numpy as np
from PIL import Image

from pymobiledevice3.remote.core_device.display_service import DisplayService
from pymobiledevice3.remote.core_device.hid_service import (
    DIGITIZER_SURFACE_MAIN_TOUCHSCREEN,
    TOUCHSCREEN_STATE_CONTACT,
    TOUCHSCREEN_STATE_RELEASE,
    UniversalHIDServiceService,
    build_touchscreen_report,
)
from pymobiledevice3.remote.core_device.screen_stream import (
    _strip_displayservice_trailer,
    depacketize_hevc,
    open_media_receiver,
)
from pymobiledevice3.remote.userspace_tunnel import establish_userspace_rsd

from device import resolve_udid

SCREEN_W, SCREEN_H = 1125, 2436
OUT = Path(__file__).resolve().parent
WARMUP = 3.0
TRACE = 3.0


def parse_rtp(dgram: bytes) -> tuple[int, bool, bytes]:
    """返回 (RTP 时间戳, marker, HEVC payload)。"""
    if len(dgram) < 12 or (dgram[0] >> 6) != 2:
        return 0, False, b""
    b0, b1 = dgram[0], dgram[1]
    ts = struct.unpack(">I", dgram[4:8])[0]
    i = 12 + (b0 & 0x0F) * 4
    if b0 & 0x10:
        if len(dgram) < i + 4:
            return 0, False, b""
        i += 4 + struct.unpack(">H", dgram[i + 2 : i + 4])[0] * 4
    return ts, bool(b1 & 0x80), dgram[i:]


class FrameTap:
    """RTP -> NAL -> Annex-B access unit -> PyAV 解码，为每帧打到达时间戳。

    帧边界靠 RTP 时间戳判定：同一 access unit 的所有包共享时间戳，
    时间戳一变即为一帧结束。比按 NAL type 猜首切片可靠。
    """

    def __init__(self) -> None:
        self.ctx = av.CodecContext.create("hevc", "r")
        self.fu = bytearray()
        self.nals: list[bytes] = []
        self.cur_ts: int | None = None
        self.au: list[bytes] = []
        self.frames: list[tuple[float, np.ndarray]] = []
        self.skipped = 0

    def feed(self, dgram: bytes) -> None:
        ts, _marker, payload = parse_rtp(dgram)
        if not payload:
            return
        if self.cur_ts is None:
            self.cur_ts = ts
        elif ts != self.cur_ts:
            self._flush()
            self.cur_ts = ts
        self.nals.clear()
        depacketize_hevc(payload, self.fu, self.nals)
        for nal in self.nals:
            if nal:
                self.au.append(_strip_displayservice_trailer(nal))

    def _flush(self) -> None:
        if not self.au:
            return
        annexb = b"".join(b"\x00\x00\x00\x01" + n for n in self.au)
        self.au = []
        t = time.monotonic()
        try:
            for pkt in self.ctx.parse(annexb):
                for frame in self.ctx.decode(pkt):
                    self.frames.append((t, frame.to_ndarray(format="gray")))
        except Exception:
            self.skipped += 1


async def draw_stroke(hid: UniversalHIDServiceService) -> float:
    """发一条高对比度笔画（画图 App 上表现为一条亮线），返回发出时刻。"""
    x1, y1 = int(SCREEN_W * 0.18), int(SCREEN_H * 0.30)
    x2, y2 = int(SCREEN_W * 0.82), int(SCREEN_H * 0.62)
    sid = DIGITIZER_SURFACE_MAIN_TOUCHSCREEN
    nx = lambda v: int(v * 65535 / SCREEN_W)  # noqa: E731
    ny = lambda v: int(v * 65535 / SCREEN_H)  # noqa: E731

    t0 = time.monotonic()
    await hid.send_report(sid, build_touchscreen_report(TOUCHSCREEN_STATE_CONTACT, nx(x1), ny(y1)))
    for i in range(1, 30):
        k = i / 29
        await hid.send_report(
            sid,
            build_touchscreen_report(
                TOUCHSCREEN_STATE_CONTACT, nx(x1 + (x2 - x1) * k), ny(y1 + (y2 - y1) * k)
            ),
        )
        await asyncio.sleep(0.012)
    await hid.send_report(sid, build_touchscreen_report(TOUCHSCREEN_STATE_RELEASE, nx(x2), ny(y2)))
    return t0


async def main() -> None:
    print("[1] 建立用户态隧道 ...")
    rsd = await establish_userspace_rsd(await resolve_udid())

    print("[2] 启动视频流（同时解锁 HID 授权门）...")
    async with DisplayService(rsd) as display:
        transport, receiver_ip = open_media_receiver(display, (4 << 20, 1 << 20))
        await display.start_video_stream(
            receiver_ip=receiver_ip,
            receiver_port=transport.port,
            sender_ip=rsd.service.address[0],
            display_id=1,
        )
        await asyncio.sleep(0.3)  # 等 backboardd 把 surface 匹配到这条新授权的流

        tap = FrameTap()
        stop = asyncio.Event()

        async def reader() -> None:
            while not stop.is_set():
                try:
                    data = await asyncio.wait_for(transport.recv(), timeout=0.5)
                except asyncio.TimeoutError:
                    continue
                except OSError:
                    return
                tap.feed(data)

        rt = asyncio.create_task(reader())

        print(f"[3] 预热 {WARMUP:.0f}s 采集基线帧 ...")
        await asyncio.sleep(WARMUP)
        n_before = len(tap.frames)
        baseline = tap.frames[n_before - 1][1] if n_before else None

        async with UniversalHIDServiceService(rsd) as hid:
            print("[4] 发出笔画 ...")
            t0 = await draw_stroke(hid)
            await asyncio.sleep(TRACE)

        stop.set()
        rt.cancel()
        with contextlib.suppress(Exception):
            await DisplayService.stop_all_streams(rsd)

    frames = tap.frames
    print(f"\n{'=' * 60}")
    print(f"解出帧数: {len(frames)} (预热段 {n_before})  解码跳过 {tap.skipped}")
    if len(frames) < 6:
        print("帧数过少，无法分析 —— 检查手机是否亮屏、是否有画面在变")
        return

    iv = sorted(b[0] - a[0] for a, b in zip(frames, frames[1:]) if 0 < b[0] - a[0] < 0.2)
    print(f"帧率    : {1000 / statistics.median(iv):.1f} fps   "
          f"帧间隔 中位 {statistics.median(iv)*1000:.1f}ms / p95 {iv[int(len(iv)*0.95)]*1000:.1f}ms")

    if baseline is not None:
        last = frames[-1][1]
        h, w = min(baseline.shape[0], last.shape[0]), min(baseline.shape[1], last.shape[1])
        base = baseline[:h, :w].astype(np.int16)
        hit = None
        for t, f in frames[n_before:]:
            if t < t0:
                continue
            if np.abs(f[:h, :w].astype(np.int16) - base).mean() > 1.5:
                hit = t
                break
        if hit:
            print(f"延迟    : 笔画首现于发出后 {(hit - t0) * 1000:.0f}ms   (输入 -> 画面)")
        else:
            print("延迟    : 未检出画面变化 —— 请让手机停在画图 App 且屏幕常亮后重试")

    Image.fromarray(frames[max(n_before - 1, 0)][1]).save(OUT / "lat_pre.png")
    Image.fromarray(frames[-1][1]).save(OUT / "lat_post.png")
    print(f"证据图  : {OUT}/lat_pre.png  lat_post.png")
    print("=" * 60)


if __name__ == "__main__":
    import logging

    logging.disable(logging.INFO)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
