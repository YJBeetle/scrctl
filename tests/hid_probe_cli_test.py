"""HID 工具离线回归；只运行帮助、参数错误和 --dry-run，禁止发送输入。"""
import os
from pathlib import Path
import platform
import re
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
extra = sys.argv[2:]
english_only = "--english-only" in extra
prefix = [arg for arg in extra if arg != "--english-only"]
checks = 0
base = {key: value for key, value in os.environ.items() if key not in
        ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "SCRCTL_LOCALEDIR")}


def run(args, variables=None):
    global checks
    result = subprocess.run([*prefix, binary, *args],
                            env={**base, "LANG": "en_US.UTF-8", **(variables or {})}, capture_output=True,
                            text=True, encoding="utf-8", timeout=3)
    checks += 1
    for marker in ("会话就绪", "建立会话失败", "universalhidservice 已连接", "辅助视频已收",
                   "设备会话已就绪", "建立设备会话失败", "Device session ready",
                   "Failed to establish device session", "universalhidservice connected",
                   "Connected to universalhidservice", "Auxiliary video received"):
        assert marker not in result.stdout + result.stderr, (args, result.stdout, result.stderr)
    return result


chinese_available = True
if not english_only and platform.libc_ver()[0] == "glibc":
    locale_probe = subprocess.run([sys.executable, "-c", """
import locale
import sys
for candidate in ('zh_CN.UTF-8', 'zh_CN.utf8', 'en_US.UTF-8', 'en_US.utf8'):
    try:
        locale.setlocale(locale.LC_MESSAGES, candidate)
    except locale.Error:
        continue
    sys.exit(0)
sys.exit(1)
"""], env=base, capture_output=True, timeout=3)
    assert locale_probe.returncode in (0, 1), locale_probe.stderr
    chinese_available = locale_probe.returncode == 0
    if not chinese_available:
        print("Only C message locales are available; checking English fallback for Chinese requests")


def has_chinese(text):
    return any("\u4e00" <= char <= "\u9fff" for char in text)


def help_is(args, chinese=False, variables=None):
    result = run(args, variables)
    assert result.returncode == 0 and not result.stderr, (args, result)
    zh = chinese and chinese_available
    expected = ("HID", "选项", "显示帮助") if zh else (
        "Send HID input in command-line order and capture screenshots", "Options", "Show help")
    assert all(text in result.stdout for text in expected), (args, result.stdout)
    for option in ("--list", "--raw", "--tap", "--line", "--stroke", "--button", "--shot",
                   "--keys", "--paste", "--probe-reply", "--swipe-loop", "--no-stream",
                   "--dry-run", "--lang", "UDID"):
        assert option in result.stdout, (args, option, result.stdout)
    words = " ".join(result.stdout.split())
    for detail in (("20 秒", "RR", "stopAll", "不能确认") if zh else
                   ("20-second", "RR", "stopAll", "does not confirm")):
        assert detail in words, (args, detail, result.stdout)
    if not zh:
        assert not has_chinese(result.stdout), result.stdout


# 原有八条帮助/默认入口检查保留，仍必须先于连接设备返回。
for args in ([], ["--help"], ["-h"], ["--no-stream", "--help"],
             ["--tap", "--button", "lock", "--help"],
             ["fake-device", "-v", "--help"], ["-v"], ["fake-device"]):
    help_is(args)


def plan(args, expected, stream="on", device="auto", chinese=False, variables=None):
    result = run([*args, "--dry-run"], variables)
    assert result.returncode == 0 and not result.stderr, (args, result)
    assert f"device={device} stream={stream} timeout=20 RR=off" in result.stdout, result.stdout
    actions = re.findall(r"^action\[(\d+)\]: (.*)$", result.stdout, re.MULTILINE)
    assert actions == [(str(i + 1), value) for i, value in enumerate(expected)], (
        args, actions, expected, result.stdout)
    # 机器字段不翻译；正文要实际切换，不能用相同英文输出冒充中文 dry-run。
    prose = "\n".join(line for line in result.stdout.splitlines()
                      if not line.startswith(("device=", "action[")))
    if chinese and chinese_available:
        assert has_chinese(prose), result.stdout
    else:
        assert not has_chinese(prose), result.stdout
    return result.stdout


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
    assert result.returncode == 2 and not result.stdout and "Invalid arguments:" in result.stderr, (
        args, result.returncode, result.stdout, result.stderr)
    assert not has_chinese(result.stderr), result.stderr


# 新增语言检查在原 98 项之后；选择语言不能执行查询、动作或辅助视频连接。
assert checks == 98, checks
for args in (["--lang", "en", "--help"], ["--help", "--lang", "en"],
             ["--lang=en", "--help"], ["--help", "--lang=en"], ["--lang", "en"]):
    help_is(args, variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "zh_CN.UTF-8"})
for args, variables in ((["--help"], {"LANG": "C"}),
                        (["--help"], {"LANG": ""}),
                        (["--help"], {"LANG": "fr_FR.UTF-8"}),
                        (["--help"], {"LANG": "en_US.UTF-8", "LANGUAGE": "zh_CN"}),
                        (["--help"], {"LANG": "zh_CN.UTF-8", "LC_ALL": "C"}),
                        (["--lang", "auto", "--help"], {"LANG": "zh_CN.UTF-8", "LC_MESSAGES": "C"}),
                        (["--help", "--lang=auto"], {"LANG": "zh_CN.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8", "LC_ALL": "C"})):
    help_is(args, variables=variables)

for args in (["--lang"], ["--lang="], ["--lang", "invalid"], ["--lang=invalid"],
             ["--lang=invalid", "--help"], ["--help", "--lang=invalid"]):
    result = run([*args, "--dry-run"])
    assert result.returncode == 2 and not result.stdout and result.stderr, (
        args, result.returncode, result.stdout, result.stderr)
    assert any(text in result.stderr for text in ("Invalid arguments:", "--lang must be auto, en or zh-CN")), result.stderr
    assert not has_chinese(result.stderr), result.stderr

language_actions = ["--button", "home", "--tap", "--list", "--shot", "offline.png",
                    "--keys", "512", "--line", "0", "0", "1", "1", "--paste", "--swipe-loop", "0"]
language_expected = ["button home", "tap 0.5 0.5", "list", "shot offline.png", "keys 512",
                     "line 0 0 1 1", "paste", "swipe-loop 0"]
english_plan = plan(["--lang", "en", *language_actions], language_expected,
                    variables={"LANG": "zh_CN.UTF-8"})
plan([*language_actions, "--lang=en", "--no-stream"], language_expected, stream="off",
     variables={"LC_ALL": "zh_CN.UTF-8"})
plan(["--lang", "auto", *language_actions], language_expected,
     variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})

if not english_only:
    for args in (["--lang", "zh-CN", "--help"], ["--help", "--lang", "zh-CN"],
                 ["--lang=zh-CN", "-h"], ["-h", "--lang=zh-CN"]):
        help_is(args, chinese=True, variables={"LC_ALL": "C"})
    for args, variables in ((["--help"], {"LANG": "zh_CN.UTF-8"}),
                            ([], {"LANG": "zh_CN.UTF-8"}),
                            (["--lang", "zh-CN"], {"LC_ALL": "C"}),
                            (["--lang", "auto", "--help"], {"LANG": "zh_CN.UTF-8"}),
                            (["--help", "--lang=auto"], {"LANG": "zh_CN.UTF-8"}),
                            (["--help"], {"LANG": "en_US.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8"}),
                            (["--help"], {"LANG": "en_US.UTF-8", "LC_MESSAGES": "C", "LC_ALL": "zh_CN.UTF-8"}),
                            (["--help"], {"LANG": "zh_CN.UTF-8", "LANGUAGE": "en"})):
        help_is(args, chinese=True, variables=variables)
    for options, variables in ((["--lang", "zh-CN"], {"LC_ALL": "C"}),
                               (["--lang=auto"], {"LANG": "zh_CN.UTF-8"}),
                               ([], {"LANG": "en_US.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8"})):
        chinese_plan = plan([*options, *language_actions], language_expected,
                            chinese=True, variables=variables)
        english_machine = [line for line in english_plan.splitlines()
                           if line.startswith(("device=", "action["))]
        chinese_machine = [line for line in chinese_plan.splitlines()
                           if line.startswith(("device=", "action["))]
        assert chinese_machine == english_machine, (english_machine, chinese_machine)
    plan([*language_actions, "--lang=zh-CN", "--no-stream"], language_expected,
         stream="off", chinese=True, variables={"LC_ALL": "C"})
    # unknown 参数的错误在 CLI11 收集完选项后报告，所以语言位于其前后都应生效。
    # trigger_on_parse 的坐标回调可立即失败，显式语言须在该动作之前。
    for args in (["--unknown", "--lang", "zh-CN"], ["--lang", "zh-CN", "--unknown"],
                 ["--lang", "zh-CN", "--tap", "nan", "0.5"],
                 ["--lang", "zh-CN", "--keys", "0"],
                 ["--lang", "zh-CN", "--shot", ""]):
        result = run([*args, "--dry-run"], {"LC_ALL": "C"})
        assert result.returncode == 2 and not result.stdout, (args, result)
        expected = "参数无效" if chinese_available else "Invalid arguments:"
        assert expected in result.stderr, (args, result.stderr)
        # 外层使用选择的语言，原始 CLI11/项目校验细节仍为英文。
        detail = result.stderr.split(expected, 1)[1]
        assert not has_chinese(detail), (args, result.stderr)
    result = run(["--unknown", "--dry-run"], {"LANG": "zh_CN.UTF-8"})
    assert result.returncode == 2 and not result.stdout
    assert ("参数无效" if chinese_available else "Invalid arguments:") in result.stderr, result.stderr

print(f"{checks} offline HID CLI checks passed (validation, ordered actions, dry-run and language selection)")
