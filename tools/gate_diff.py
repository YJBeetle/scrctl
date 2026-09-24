#!/usr/bin/env python3
"""比对 hid_gate_probe 留下的截图，判定每一次触摸注入有没有落地。

为什么用 PIL 而不是在 C++ 里比：那边没有 PNG 解码器，而这里要判定的事实
（"画面上多了一条白线没有"）用两行像素统计就够，不值得为它引一个依赖。

用法： python3 tools/gate_diff.py /tmp/gate
"""
import sys
from pathlib import Path

from PIL import Image

# (阶段标签, 注入前的图, 注入后的图, 那条线所在的纵向带 y0..y1)
PAIRS = [
    ("control", "a0-before", "a1-control", 0.23, 0.34),
    ("idle-dead", "b0-before", "b1-idle-dead", 0.39, 0.50),
    ("session-gone", "c0-before", "c1-session-gone", 0.55, 0.66),
    ("revived", "d0-before", "d1-revived", 0.71, 0.82),
]
X0, X1 = 0.50, 0.95


def band_stats(img, y0, y1):
    """取纵向带，返回 (亮像素数, 图)。亮像素=无边记里那条白笔迹。"""
    w, h = img.size
    crop = img.crop((int(w * X0), int(h * y0), int(w * X1), int(h * y1))).convert("L")
    px = crop.tobytes()
    bright = sum(1 for v in px if v > 128)
    return bright, crop


def main(d):
    d = Path(d)
    print(f"{'阶段':<14}{'注入前亮像素':>14}{'注入后':>10}{'新增':>9}  判定")
    ok = True
    for label, before, after, y0, y1 in PAIRS:
        pb, pa = d / f"{before}.png", d / f"{after}.png"
        if not (pb.exists() and pa.exists()):
            print(f"{label:<14} 缺图 {pb.name if not pb.exists() else pa.name}")
            ok = False
            continue
        ib = Image.open(pb)
        ia = Image.open(pa)
        b0, cb = band_stats(ib, y0, y1)
        b1, ca = band_stats(ia, y0, y1)
        # 两条都算：只看"多了亮像素"会漏掉"改了已有笔迹"的情况，只看差异像素
        # 又会被状态栏和光标闪烁骗到——所以把比对限制在同一条带里。
        diff = sum(1 for x, y in zip(cb.tobytes(), ca.tobytes()) if abs(x - y) > 64)
        landed = b1 - b0 > 200 or diff > 400
        ok = ok and landed
        print(f"{label:<14}{b0:>14,}{b1:>10,}{b1 - b0:>+9,}"
              f"  {'落地' if landed else '没落地'}   差异像素 {diff:,}")
    print("\n结论：" + ("四个阶段的注入都落地了" if ok else "有阶段没落地，见上表"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "/tmp/gate"))
