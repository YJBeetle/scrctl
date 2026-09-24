#!/usr/bin/env python3
"""扫 Annex-B 文件，报告每个 NAL 的类型与尺寸分布。

用法: nalsizes.py FILE [上限]
"""
import sys, collections

path = sys.argv[1]
cap = int(sys.argv[2]) if len(sys.argv) > 2 else 65535
data = open(path, 'rb').read()
n = len(data)

# 扫零串找 start code：>=2 个零紧跟 0x01。记下 (start code 起点, NAL 起点)。
marks = []
i = 0
while i < n:
    if data[i] != 0:
        i += 1
        continue
    j = i
    while j < n and data[j] == 0:
        j += 1
    if j < n and data[j] == 1 and j - i >= 2:
        marks.append((i, j + 1))
    i = max(j + 1, i + 1)

names = {32: 'VPS', 33: 'SPS', 34: 'PPS', 35: 'AUD', 39: 'SEI-PFX', 40: 'SEI-SFX',
         48: 'AGG?', 49: 'FU-A?'}
by_type = collections.defaultdict(list)
for k, (_, nal_start) in enumerate(marks):
    end = marks[k + 1][0] if k + 1 < len(marks) else n
    if end - nal_start < 2:
        continue
    t = (data[nal_start] >> 1) & 0x3F
    by_type[t].append(end - nal_start)

print(f'NAL 总数 {len(marks)}   文件 {n} 字节')
total_over = 0
for t, lst in sorted(by_type.items()):
    lst.sort()
    over = sum(1 for x in lst if x > cap)
    total_over += over
    name = names.get(t, 'VCL-slice' if t <= 31 else f'type{t}')
    pick = lambda p: lst[min(len(lst) - 1, int(len(lst) * p))]
    print(f'  {name:11s} n={len(lst):5d} min={lst[0]:7d} p50={pick(0.5):7d} '
          f'p95={pick(0.95):7d} max={lst[-1]:7d} >{cap}={over}')
print(f'合计超 {cap} 的 NAL: {total_over}')
