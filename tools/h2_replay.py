#!/usr/bin/env python3
"""把一个 SCRCTL_H2_DUMP 文件当 HTTP/2 重放，报出帧序列和第一处对不上的地方。

为什么需要它：HTTP/2 层报"帧长过大"时，光看缓冲区里的十六进制猜不出成因——是链路
给了坏字节、是我们消费错长度、还是别的流的字节混进来了，三种都能自圆其说。把入流
原始字节 dump 下来重放一遍就能一次分掉：

  * 整个文件帧帧对得上、只在某处断 -> 那个位置之前我们少读了字节（丢在 TCP 层）；
  * 文件自己在同一处就歪了 -> 设备/链路给的字节有问题。

配合报错里打的"流内偏移"（= 累计进来 − 手上还剩）定位：

  SCRCTL_H2_DUMP=/tmp/h2 ./build-cmake/scrctl --stats
  python3 tools/h2_replay.py /tmp/h2.24.bin 32946

第二个参数可省略（从头开始走），给了就只报那个偏移附近的现场。
"""
import sys

TYPES = {0: 'DATA', 1: 'HEADERS', 2: 'PRIORITY', 3: 'RST_STREAM', 4: 'SETTINGS',
         5: 'PING', 6: 'GOAWAY', 7: 'WINDOW_UPDATE', 8: 'CONTINUATION'}


def walk(b, start=0, limit=400):
    """从 start 起按帧头走，返回 (停下的位置, 帧数, 状态)。"""
    i, n = start, 0
    while i + 9 <= len(b):
        ln = b[i] << 16 | b[i + 1] << 8 | b[i + 2]
        ty, fl = b[i + 3], b[i + 4]
        sid = int.from_bytes(b[i + 5:i + 9], 'big') & 0x7FFFFFFF
        name = TYPES.get(ty, f'?{ty}')
        if ln > 16384:
            print(f'#{n} @{i} 不是帧头：len={ln} type={name} flags={fl:#x} sid={sid}')
            return i, n, 'bad-header'
        if i + 9 + ln > len(b):
            print(f'#{n} @{i} {name} len={ln} flags={fl:#x} sid={sid} -> 尾部还差 '
                  f'{i + 9 + ln - len(b)} 字节')
            return i, n, 'short'
        if n < 12 or n > limit - 4:
            print(f'#{n} @{i} {name} len={ln} flags={fl:#x} sid={sid}')
        elif n == 13:
            print('   ...')
        i += 9 + ln
        n += 1
        if n > limit:
            return i, n, 'many'
    return i, n, 'ok'


def nearest_header(b, at, span=4096):
    """在 at 前后找一个"像帧头"的位置，用来量出到底错位多少字节。"""
    for d in range(0, span):
        for cand in (at + d, at - d):
            if cand + 9 > len(b) or cand < 0:
                continue
            ln = b[cand] << 16 | b[cand + 1] << 8 | b[cand + 2]
            ty = b[cand + 3]
            sid = int.from_bytes(b[cand + 5:cand + 9], 'big') & 0x7FFFFFFF
            if ty in TYPES and 0 < ln <= 16384 and cand + 9 + ln <= len(b) and b[cand + 4] < 0x40:
                return cand, ln, TYPES[ty], sid
    return None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    data = open(sys.argv[1], 'rb').read()
    print(f'{sys.argv[1]}: {len(data)} 字节')
    end, n, why = walk(data)
    print(f'-> 从头解出 {n} 帧，停在 {end}，状态 {why}')
    if len(sys.argv) > 2:
        at = int(sys.argv[2])
        print(f'\n报错现场 {at} 前后 32 字节:')
        print('  ', data[max(0, at - 32):at].hex())
        print('  ', data[at:min(len(data), at + 32)].hex())
        found = nearest_header(data, at)
        if found:
            cand, ln, name, sid = found
            print(f'最近的像帧头位置 @{cand}（{name} len={ln} sid={sid}）'
                  f' -> 相对现场 {"少读" if cand > at else "多读"} {abs(cand - at)} 字节')
        else:
            print('附近找不到像帧头的位置')
    return 0


if __name__ == '__main__':
    sys.exit(main())
