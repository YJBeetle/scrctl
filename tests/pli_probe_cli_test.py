"""运行真实 PLI 探针的离线 CLI 回归，只使用帮助、规划或参数失败路径。"""
import os
from pathlib import Path
import re
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
prefix = sys.argv[2:]
environment = {**os.environ, "LC_ALL": "C", "LANG": "C"}
checks = 0
runtime_markers = ("建立会话失败", "起流失败", "流已建立", "打开 HID 服务失败",
                   "已观察到视频 RTP", "PLI 已写入隧道", "释放触摸失败")


def run(args):
    global checks
    result = subprocess.run([*prefix, binary, *args], env=environment,
                            capture_output=True, text=True, encoding="utf-8", timeout=5)
    checks += 1
    assert not any(marker in result.stdout + result.stderr for marker in runtime_markers), (
        args, result.stdout, result.stderr)
    return result


for args in (["--help"], ["-h"], ["--dry-run", "--help"],
             ["--random-sender", "-t", "5", "-w", "7", "--help"]):
    result = run(args)
    assert result.returncode == 0 and not result.stderr, (args, result.returncode, result.stderr)
    assert "发送单次 PLI" in result.stdout and "rr_keepalive_probe" in result.stdout, result.stdout
    assert "不连接设备" in result.stdout, result.stdout
    assert all(option in result.stdout for option in ("-t", "-w", "--random-sender", "--dry-run"))
    assert "实验参数：" not in result.stdout, result.stdout


def plan_is(args, baseline_seconds, watch_seconds, sender="negotiated"):
    result = run([*args, "--dry-run"])
    assert result.returncode == 0 and not result.stderr, (args, result.returncode, result.stderr)
    assert len(result.stdout.splitlines()) == 1, result.stdout
    assert "实验参数：" in result.stdout and "timeout=20 秒" in result.stdout
    assert "RR=off" in result.stdout and f"sender={sender}" in result.stdout, result.stdout
    actual = dict(re.findall(r"(baseline_ms|watch_ms|total_ms)=(\d+)", result.stdout))
    expected = {"baseline_ms": str(baseline_seconds * 1000),
                "watch_ms": str(watch_seconds * 1000),
                "total_ms": str((baseline_seconds + watch_seconds) * 1000)}
    assert actual == expected, (args, expected, actual, result.stdout)


plan_is([], 3, 6)
plan_is(["-t", "0", "-w", "1"], 0, 1)
plan_is(["--dry-run", "-w", "7", "--random-sender", "-t", "5"], 5, 7, "fixed-experimental")
# 旧入口重复参数采用最后值；十进制前导零不能因 CLI11 的默认 base0 转换改变含义。
plan_is(["-t", "1", "-t", "4", "-w", "2", "-w", "8"], 4, 8)
plan_is(["-t", "010", "-w", "09"], 10, 9)
plan_is(["-t", "+3", "-w", " 4"], 3, 4)
# 两个参数均在旧 int 输入范围内，但总和以及乘 1000 均会超出 signed int。
plan_is(["-t", "2147483647", "-w", "2147483647"], 2147483647, 2147483647)
plan_is(["-t", "2147484", "-w", "7"], 2147484, 7)


invalid = (["--dry-run", "-t"], ["--dry-run", "-w"],
           ["--unknown", "--dry-run"], ["--dry-run", "--unknown"],
           ["--dry-run", "extra"], ["--dry-run", "--random-sender=bad"],
           ["--dry-run", "-t", "-1"], ["--dry-run", "-w", "-1"],
           ["--dry-run", "-w", "0"], ["--dry-run", "-t", "+-0"],
           ["--dry-run", "-t", "2147483648"],
           ["--dry-run", "-w", "2147483648"],
           ["--dry-run", "-t", "999999999999999999999999"],
           ["--dry-run", "-w", "-2147483649"])
for value in ("bad", "3tail", "1.5", "1e3", "0x10", ""):
    for option in ("-t", "-w"):
        invalid += (["--dry-run", option, value],)
for args in invalid:
    result = run(args)
    assert result.returncode == 2 and not result.stdout and result.stderr, (
        args, result.returncode, result.stdout, result.stderr)
    assert "参数错误：" in result.stderr, (args, result.stderr)

print(f"{checks} offline PLI CLI checks passed (help, planning, duration bounds and errors)")
