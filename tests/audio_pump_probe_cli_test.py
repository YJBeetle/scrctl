"""真实音频探针的离线 CLI 回归；帮助和非法参数必须在设备连接前返回。"""
import os
from pathlib import Path
import platform
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
# 本地构建还未合入新译文时可用 --english-only；其余参数可作为 sandbox-exec 等前缀。
extra = sys.argv[2:]
english_only = "--english-only" in extra
prefix = [arg for arg in extra if arg != "--english-only"]
base = {k: v for k, v in os.environ.items() if k not in
        ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "SCRCTL_LOCALEDIR")}
checks = 0


def run(args, variables=None):
    global checks
    result = subprocess.run([*prefix, binary, *args], env={**base, "LANG": "en_US.UTF-8",
                                                       **(variables or {})},
                            capture_output=True, text=True, encoding="utf-8", timeout=3)
    checks += 1
    return result


chinese_available = True
if not english_only and platform.libc_ver()[0] == "glibc":
    probe = subprocess.run([sys.executable, "-c", """
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
    assert probe.returncode in (0, 1), probe.stderr
    chinese_available = probe.returncode == 0
    if not chinese_available:
        print("Only C message locales are available; checking English fallback for Chinese requests")


options = ("--seconds", "--with-video", "--verbose", "--mute", "--kill-at",
           "--revideo-at", "--realtime", "--late-open", "--lang", "UDID")


def help_is(args, chinese=False, variables=None):
    result = run(args, variables)
    assert result.returncode == 0 and not result.stderr, (args, result.stderr)
    zh = chinese and chinese_available
    expected = ("观察音频流、恢复过程和缓冲水位", "选项", "显示帮助") if zh else (
        "Inspect audio streaming, recovery and buffer levels", "Options", "Show help")
    assert all(text in result.stdout for text in expected), (args, result.stdout)
    for option in options:
        assert option in result.stdout, (option, result.stdout)
    for detail in ("25", "VolumeDown", "VolumeUp", "480", "10"):
        assert detail in result.stdout, (detail, result.stdout)
    assert "Audio stream started" not in result.stdout and "音频流已启动" not in result.stdout
    if not zh:
        assert not any("\u4e00" <= c <= "\u9fff" for c in result.stdout), result.stdout
        assert "--help does not connect to a device" in result.stdout
        assert "does not restore the original volume" in result.stdout


# 有效模式、别名、参数位置和重复标量都在帮助路径验证，避免触碰当前真机。
for args in (["--help"], ["-h", "-v", "-s", "1", "offline-udid"],
             ["--seconds", "30", "--seconds", "1", "--with-video", "--mute", "--help"],
             ["--kill-at", "0", "--revideo-at", "0", "--help"],
             ["--kill-at", "-1", "--revideo-at", "-1", "--help"],
             ["--realtime", "--late-open", "0", "--help"],
             ["--late-open", "5", "--help"],
             ["--seconds", "2147483647", "--kill-at", "2147483647", "--revideo-at",
              "2147483647", "--late-open", "2147483647", "--help"],
             ["first-udid", "second-udid", "--help"],
             ["--verbose", "--verbose", "--mute", "--mute", "--help"]):
    help_is(args)

help_is(["--help", "--lang", "en"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--lang", "auto", "--help"], variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})
if not english_only:
    help_is(["--lang", "zh-CN", "--help"], chinese=True, variables={"LC_ALL": "C"})
    help_is(["--help", "--lang", "zh-CN"], chinese=True)
    help_is(["--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
    help_is(["--help"], chinese=True, variables={"LANG": "en_US.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8"})

invalid = [["--unknown"], ["--unknown", "offline-udid"], ["--seconds", "0"],
           ["--seconds", "-1"], ["--kill-at", "-2"], ["--revideo-at", "-2"],
           ["--late-open", "-1"], ["--lang"], ["--lang", "invalid"],
           ["--lang=invalid", "--help"]]
for option in ("--seconds", "-s", "--kill-at", "--revideo-at", "--late-open"):
    invalid.append([option])
    for value in ("bad", "3x", "1.5", "", "2147483648", "-2147483649", "9999999999999999999"):
        invalid.append([option, value])
for args in invalid:
    result = run(args)
    assert result.returncode == 2 and result.stderr and not result.stdout, (
        args, result.returncode, result.stdout, result.stderr)
    # 设备建立失败的业务退出码是1；任何参数错误都必须先以2退出。
    assert "Failed to establish device session" not in result.stderr, (args, result.stderr)
    assert "建立设备会话失败" not in result.stderr, (args, result.stderr)

if not english_only:
    result = run(["--seconds", "bad", "--lang", "zh-CN"])
    assert result.returncode == 2 and not result.stdout and result.stderr
    assert ("参数无效" if chinese_available else "Invalid arguments") in result.stderr, result.stderr

print(f"{checks} offline audio CLI checks passed (help, aliases, validation and language selection)")
