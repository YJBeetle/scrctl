"""HID 工具离线回归；只运行帮助、参数错误和 --dry-run，禁止发送输入。"""
import os
from pathlib import Path
import re
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
checks = 0
environment = {**os.environ, "LANG": "C", "LC_ALL": "C"}


def run(args):
    global checks
    result = subprocess.run([binary, *args], env=environment, capture_output=True,
                            text=True, encoding="utf-8", timeout=3)
    checks += 1
    for marker in ("会话就绪", "建立会话失败", "universalhidservice 已连接", "辅助视频已收"):
        assert marker not in result.stdout + result.stderr, (args, result.stdout, result.stderr)
    return result


for args in ([], ["--help"], ["-h"], ["--no-stream", "--help"],
             ["--tap", "--button", "lock", "--help"],
             ["fake-device", "-v", "--help"], ["-v"], ["fake-device"]):
    result = run(args)
    assert result.returncode == 0 and not result.stderr, (args, result)
    for option in ("--list", "--raw", "--tap", "--line", "--stroke", "--button", "--shot",
                   "--keys", "--paste", "--probe-reply", "--swipe-loop", "--no-stream",
                   "--dry-run", "UDID"):
        assert option in result.stdout, (args, option, result.stdout)
    for detail in ("20 秒", "RR", "stopAll", "不确认输入已生效"):
        assert detail in result.stdout, (args, detail, result.stdout)


def plan(args, expected, stream="on", device="auto"):
    result = run([*args, "--dry-run"])
    assert result.returncode == 0 and not result.stderr, (args, result)
    assert f"device={device} stream={stream} timeout=20 RR=off" in result.stdout, result.stdout
    actions = re.findall(r"^action\[(\d+)\]: (.*)$", result.stdout, re.MULTILINE)
    assert actions == [(str(i + 1), value) for i, value in enumerate(expected)], (
        args, actions, expected, result.stdout)


plan([], [])
plan(["--no-stream"], [], stream="off")
plan(["fake-device", "--no-stream"], [], stream="off", device="explicit")
plan(["--tap"], ["tap 0.5 0.5"])
plan(["--tap", "--button", "lock"], ["tap 0.5 0.5", "button lock"])
plan(["--tap", "--tap", "0", "1", "--tap"],
     ["tap 0.5 0.5", "tap 0 1", "tap 0.5 0.5"])
plan(["--shot", "before.png", "--button", "home", "--line", "0", "0", "1", "1",
      "--shot", "after.png"], ["shot before.png", "button home", "line 0 0 1 1", "shot after.png"])
plan(["--stroke", "--keys", "512", "--list", "--paste", "--raw", "--probe-reply",
      "--swipe-loop", "0", "--button", "mute", "--stroke", "--paste"],
     ["stroke", "keys 512", "list", "paste", "raw", "probe-reply", "swipe-loop 0",
      "button mute", "stroke", "paste"])
plan(["--raw", "--list", "--raw"], ["raw", "list", "raw"])
plan(["--shot=-file.png", "--button=volup", "--button=voldn"],
     ["shot -file.png", "button volup", "button voldn"])
plan(["fake-device", "-v", "--tap", ".25", "1e-1", "--no-stream"],
     ["tap 0.25 0.1"], stream="off", device="explicit")
plan(["--tap", "--", "fake-device"], ["tap 0.5 0.5"], device="explicit")
for value, normalized in (("0", "0"), ("010", "10"), ("+10", "10"),
                          (" 10 ", "10"), ("2147483647", "2147483647")):
    plan(["--swipe-loop", value], [f"swipe-loop {normalized}"])
for value, normalized in (("1", "1"), ("0512", "512"), ("+512", "512"),
                          (" 512 ", "512"), ("18446744073709551615", "18446744073709551615")):
    plan(["--keys", value], [f"keys {normalized}"])


invalid = [
    ["--unknown"], ["--tap", "--unknown"], ["--button"], ["--button", "--tap"],
    ["--button", "invalid"], ["--button", "HOME"], ["--shot"], ["--shot="],
    ["--shot", ""], ["--shot", "--tap"], ["--keys"], ["--swipe-loop"],
    ["--line"], ["--line", "0", "0", "1"], ["--line", "0", "0", "--paste"],
    ["--tap", "0.2"], ["--tap", "0.2", "--button", "home"],
    ["--tap", "wrong", "0.5"], ["--line", "0", "0", "1", "wrong"],
    ["first-device", "second-device"], ["--tap", "0", "1", "device", "extra"],
    ["--dry-run=bad"], ["--no-stream=bad"], ["--stroke=bad"], ["--paste=bad"],
    ["--list=bad"], ["--raw=bad"], ["--verbose=bad"], ["--probe-reply=bad"],
]
for value in ("nan", "NaN", "inf", "-inf", "1e999", "-0.001", "1.001", "1tail", ""):
    invalid.append(["--tap", value, "0.5"])
    invalid.append(["--line", "0", "0", "1", value])
for value in ("-1", "0", "1tail", "512.5", "18446744073709551616", "0x200", "", "nan",
              "++512", "+-512", "--paste"):
    invalid.append(["--keys", value])
for value in ("-1", "1tail", "1.5", "2147483648", "0x10", "", "nan", "++10", "+-10", "--paste"):
    invalid.append(["--swipe-loop", value])

for args in invalid:
    # 真实参数错误先于 dry-run / 设备访问返回；末尾 dry-run 防止测试失误操作设备。
    result = run([*args, "--dry-run"])
    assert result.returncode == 2 and not result.stdout and "参数无效:" in result.stderr, (
        args, result.returncode, result.stdout, result.stderr)

print(f"{checks} offline HID CLI checks passed (validation, ordered actions and dry-run)")
