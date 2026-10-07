"""运行真实 applist_probe 的离线 CLI 回归；帮助和非法参数均在会话建立前退出。"""
import os
from pathlib import Path
import platform
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
extra = sys.argv[2:]
english_only = "--english-only" in extra
prefix = [arg for arg in extra if arg != "--english-only"]
base = {key: value for key, value in os.environ.items() if key not in
        ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "SCRCTL_LOCALEDIR")}
checks = 0


def run(args, variables=None):
    global checks
    result = subprocess.run([*prefix, binary, *args],
                            env={**base, "LANG": "en_US.UTF-8", **(variables or {})},
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


connection_markers = ("Failed to establish device session", "Device session ready:",
                      "Failed to list device processes", "Failed to list installed apps",
                      "Process tokens:", "Installed apps:", "建立设备会话失败",
                      "设备会话已就绪", "查询设备进程失败", "查询已安装应用失败")


def help_is(args, chinese=False, variables=None):
    result = run(args, variables)
    assert result.returncode == 0 and not result.stderr, (args, result.returncode, result.stderr)
    zh = chinese and chinese_available
    expected = ("查看设备进程或已安装应用", "选项", "显示帮助") if zh else (
        "Inspect device processes or installed apps", "Options", "Show help")
    assert all(text in result.stdout for text in expected), (args, result.stdout)
    for option in ("-h", "--help", "-v", "--verbose", "--list", "--grep", "--lang", "UDID"):
        assert option in result.stdout, (option, result.stdout)
    assert "TEXT" in result.stdout
    assert not any(marker in result.stdout for marker in connection_markers), (args, result.stdout)
    if not zh:
        assert not any("\u4e00" <= char <= "\u9fff" for char in result.stdout), result.stdout
        words = " ".join(result.stdout.split())
        for detail in ("up to three processes", "--list shows every installed app and ignores --grep",
                       "--help does not connect to a device", "last value is used"):
            assert detail in words, (detail, result.stdout)


for args in (["--help"], ["-h"], ["--list", "--help"], ["--verbose", "--help"],
             ["-v", "--list", "offline-udid", "--help"],
             ["offline-udid", "--list", "--verbose", "--help"],
             ["--grep", "Sample.app", "--help"], ["--grep=Sample.app", "--help"],
             ["--grep", "", "--help"], ["--grep", "含中文的路径", "--help"],
             ["--grep", "--literal-text", "--help"],
             ["--grep", "--list", "--help"],
             ["--list", "--grep", "Sample.app", "--help"],
             ["--grep", "Sample.app", "--list", "--help"],
             ["--grep", "first", "--grep", "last", "--help"],
             ["first-udid", "second-udid", "--help"],
             ["first-udid", "--list", "second-udid", "-h"],
             ["--list", "--list", "--verbose", "--verbose", "--help"],
             ["--help", "--", "--literal-udid"]):
    help_is(args)

for args in (["--lang", "en", "--help"], ["--help", "--lang", "en"],
             ["--lang=en", "--help"], ["--help", "--lang=en"]):
    help_is(args, variables={"LANG": "zh_CN.UTF-8"})
help_is(["--help"], variables={"LANG": "C"})
help_is(["--help"], variables={"LANG": "fr_FR.UTF-8"})
help_is(["--help"], variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})
help_is(["--lang", "auto", "--help"], variables={"LANG": "zh_CN.UTF-8", "LC_MESSAGES": "C"})
help_is(["--help"], variables={"LANG": "zh_CN.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8", "LC_ALL": "C"})

invalid = (["--unknown"], ["--unknown", "offline-udid"], ["offline-udid", "--unknown"],
           ["--list=bad"], ["--verbose=bad"], ["--grep"],
           ["--lang"], ["--lang="], ["--lang", "invalid"], ["--lang=invalid"],
           ["--lang=invalid", "--help"], ["--help", "--lang=invalid"])
for args in invalid:
    result = run(args)
    assert result.returncode == 2 and result.stderr and not result.stdout, (
        args, result.returncode, result.stdout, result.stderr)
    expected = "--lang must be auto, en or zh-CN" if any("invalid" in arg for arg in args) else "Invalid arguments:"
    assert expected in result.stderr, (args, result.stderr)
    assert not any(marker in result.stderr for marker in connection_markers), (args, result.stderr)

if not english_only:
    for args in (["--lang", "zh-CN", "--help"], ["--help", "--lang", "zh-CN"],
                 ["--lang=zh-CN", "-h"], ["-h", "--lang=zh-CN"]):
        help_is(args, chinese=True, variables={"LC_ALL": "C"})
    help_is(["--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
    help_is(["--lang", "auto", "--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
    help_is(["--help"], chinese=True, variables={"LANG": "en_US.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8"})
    help_is(["--help"], chinese=True, variables={"LANG": "en_US.UTF-8", "LC_MESSAGES": "C", "LC_ALL": "zh_CN.UTF-8"})
    for args in (["--unknown", "--lang", "zh-CN"],
                 ["--lang", "zh-CN", "--grep"]):
        result = run(args)
        assert result.returncode == 2 and not result.stdout
        assert ("参数无效" if chinese_available else "Invalid arguments:") in result.stderr, result.stderr
        assert not any(marker in result.stderr for marker in connection_markers), result.stderr

print(f"{checks} offline applist CLI checks passed (help, aliases, validation and language selection)")
