#!/usr/bin/env python3
"""把 iPhone 的 displayservice 视频流转成 Annex-B HEVC，输出到 stdout。

配合 ffplay 实时观看：

    python3 stream.py | ffplay -loglevel warning -f hevc \\
        -probesize 32 -analyzeduration 0 pipe:0

统计信息走 stderr，不污染管道。
"""
from __future__ import annotations

import argparse
import asyncio
import contextlib
import statistics
import struct
import sys
import time

from pymobiledevice3.remote.core_device.display_service import DisplayService
from pymobiledevice3.remote.core_device.screen_stream import (
    _strip_displayservice_trailer,
    depacketize_hevc,
    open_media_receiver,
)
from pymobiledevice3.remote.userspace_tunnel import establish_userspace_rsd

from device import resolve_udid

START_CODE = b"\x00\x00\x00\x01"


def parse_rtp(dgram: bytes) -> tuple[int, bytes]:
    if len(dgram) < 12 or (dgram[0] >> 6) != 2:
        return 0, b""
    b0 = dgram[0]
    ts = struct.unpack(">I", dgram[4:8])[0]
    i = 12 + (b0 & 0x0F) * 4
    if b0 & 0x10:
        if len(dgram) < i + 4:
            return 0, b""
        i += 4 + struct.unpack(">H", dgram[i + 2 : i + 4])[0] * 4
    return ts, dgram[i:]


async def run(udid: str, display_id: int, show_stats: bool) -> None:
    out = sys.stdout.buffer
    log = lambda m: print(m, file=sys.stderr, flush=True)  # noqa: E731

    log("建立用户态隧道 ...")
    rsd = await establish_userspace_rsd(await resolve_udid(udid))

    async with DisplayService(rsd) as display:
        transport, receiver_ip = open_media_receiver(display, (4 << 20, 1 << 20))
        await display.start_video_stream(
            receiver_ip=receiver_ip,
            receiver_port=transport.port,
            sender_ip=rsd.service.address[0],
            display_id=display_id,
        )
        log("视频流已建立，开始输出 Annex-B HEVC 到 stdout")

        fu = bytearray()
        nals: list[bytes] = []
        cur_ts = None
        au: list[bytes] = []
        frames = 0
        bytes_out = 0
        t_start = time.monotonic()
        stamps: list[float] = []

        def emit() -> None:
            nonlocal au, frames, bytes_out, stamps
            if not au:
                return
            blob = b"".join(START_CODE + n for n in au)
            au = []
            out.write(blob)
            out.flush()
            frames += 1
            bytes_out += len(blob)
            stamps.append(time.monotonic())
            if len(stamps) > 2:
                stamps.pop(0)
            if show_stats and frames % 120 == 0:
                el = time.monotonic() - t_start
                iv = [b - a for a, b in zip(stamps, stamps[1:]) if 0 < b - a < 0.2]
                fps = 1 / statistics.median(iv) if iv else 0
                log(f"  {frames} 帧  {fps:.1f} fps  {bytes_out/el/1024:.0f} KiB/s")

        try:
            while True:
                data = await transport.recv()
                ts, payload = parse_rtp(data)
                if not payload:
                    continue
                if cur_ts is None:
                    cur_ts = ts
                elif ts != cur_ts:
                    emit()
                    cur_ts = ts
                nals.clear()
                depacketize_hevc(payload, fu, nals)
                for nal in nals:
                    if nal:
                        au.append(_strip_displayservice_trailer(nal))
        except (asyncio.CancelledError, OSError):
            pass
        finally:
            emit()
            transport.close()
            with contextlib.suppress(Exception):
                await DisplayService.stop_all_streams(rsd)
            log(f"结束：共 {frames} 帧")


if __name__ == "__main__":
    import logging

    logging.disable(logging.INFO)
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--udid", default=None, help="留空则自动发现唯一 USB 设备")
    ap.add_argument("--display-id", type=int, default=1)
    ap.add_argument("--no-stats", action="store_true")
    a = ap.parse_args()
    try:
        asyncio.run(run(a.udid, a.display_id, not a.no_stats))
    except KeyboardInterrupt:
        pass
