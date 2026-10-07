"""应用启动探针的离线 CLI 回归；仅运行帮助、错误参数和 --shape。"""
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
english_only = "--english-only" in sys.argv[2:]
base = {key: value for key, value in os.environ.items() if key not in
        ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "SCRCTL_LOCALEDIR")}
checks = 0


def run(args, variables=None):
    global checks
    result = subprocess.run([binary, *args],
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


def help_is(args, chinese=False, variables=None):
    result = run(args, variables)
    assert result.returncode == 0 and not result.stderr, (args, result.returncode, result.stderr)
    zh = chinese and chinese_available
    expected = ("查看应用启动请求或停止应用", "选项", "显示帮助") if zh else (
        "Inspect app launch requests or stop an app", "Options", "Show help")
    assert all(text in result.stdout for text in expected), (args, result.stdout)
    for option in ("--shape", "--stop", "--keep-running", "-v", "--verbose", "--lang", "BUNDLE_ID", "UDID"):
        assert option in result.stdout, (option, result.stdout)
    assert "com.example.App" in result.stdout
    assert "applicationSpecifier" not in result.stdout
    if not zh:
        assert not any("\u4e00" <= char <= "\u9fff" for char in result.stdout), result.stdout
        words = " ".join(result.stdout.split())
        for detail in ("terminate an existing instance by default", "last value is used",
                       "does not connect to a device", "required unless --shape is used"):
            assert detail in words, (detail, result.stdout)


for args in (["--help"], ["-h"], ["com.example.App", "first-udid", "last-udid", "-v", "--help"],
             ["--shape", "--stop", "--keep-running", "--help"]):
    help_is(args)
help_is(["--lang", "en", "--help"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--help", "--lang=en"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--help"], variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})
help_is(["--lang", "auto", "--help"], variables={"LANG": "zh_CN.UTF-8", "LC_MESSAGES": "C"})


def shape_is(args, terminate_existing, variables=None):
    result = run(args, variables)
    assert result.returncode == 0 and not result.stderr, (args, result.returncode, result.stderr)
    # describe 的固定样本使用未加引号的字典键及 <N bytes> 数据摘要。
    # 仅将这两种表示转换为 JSON，再检查实际层级；此转换不用于设备任意回复。
    text = re.sub(r'<(\d+) bytes>', r'{"data_bytes": \1}', result.stdout)
    text = re.sub(r'([,{]\s*)([A-Za-z_][A-Za-z_0-9]*)(\s*:)', r'\1"\2"\3', text)
    request = json.loads(text)
    assert set(request) == {"applicationSpecifier", "options", "standardIOIdentifiers"}, request
    assert request["applicationSpecifier"] == {"bundleIdentifier": {"_0": "com.example.App"}}, request
    assert request["standardIOIdentifiers"] == {}, request
    options = request["options"]
    assert options["terminateExisting"] is terminate_existing, options
    assert options["platformSpecificOptions"]["data_bytes"] > 0, options
    remaining = {key: value for key, value in options.items() if key != "platformSpecificOptions"}
    assert remaining == {"arguments": [], "environmentVariables": {},
                         "standardIOUsesPseudoterminals": True, "startStopped": False,
                         "terminateExisting": terminate_existing, "user": {"shortName": "mobile"}}, options
    return result.stdout


default_shape = shape_is(["--shape"], True)
keep_shape = shape_is(["--shape", "--keep-running"], False)
assert default_shape != keep_shape
assert shape_is(["--shape", "--stop", "-v"], True) == default_shape
assert shape_is(["com.other.App", "first-udid", "last-udid", "--shape"], True) == default_shape
assert shape_is(["--keep-running", "--shape", "--stop", "--keep-running"], False) == keep_shape
assert shape_is(["--shape", "--lang", "en"], True, {"LANG": "zh_CN.UTF-8"}) == default_shape

# 每个错误案例均没有 bundle ID，或同时带 --shape，避免测试失误触发启动/停止。
invalid = ([""], [], ["--stop"], ["--keep-running"], ["--verbose"],
           ["--shape", "--unknown"], ["com.example.App", "--unknown", "--shape"],
           ["--shape=bad"], ["--shape", "--stop=bad"], ["--shape", "--keep-running=bad"],
           ["--shape", "--verbose=bad"], ["--lang"], ["--shape", "--lang=invalid"],
           ["--help", "--lang=invalid"])
for args in invalid:
    result = run(args)
    assert result.returncode == 2 and result.stderr and not result.stdout, (
        args, result.returncode, result.stdout, result.stderr)
    assert any(text in result.stderr for text in (
        "A bundle ID is required", "Invalid arguments:", "--lang must be auto, en or zh-CN")), result.stderr
    assert "Failed to establish device session" not in result.stderr, result.stderr

if not english_only:
    help_is(["--lang", "zh-CN", "--help"], chinese=True, variables={"LC_ALL": "C"})
    help_is(["--help", "--lang", "zh-CN"], chinese=True)
    help_is(["--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
    help_is(["--help"], chinese=True, variables={"LANG": "en_US.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8"})
    help_is(["--help"], chinese=True, variables={"LANG": "en_US.UTF-8", "LC_MESSAGES": "C", "LC_ALL": "zh_CN.UTF-8"})
    assert shape_is(["--shape", "--lang", "zh-CN"], True, {"LC_ALL": "C"}) == default_shape
    for args, zh, en in ((["--lang", "zh-CN"], "需要提供应用 bundle ID", "A bundle ID is required"),
                        (["--shape", "--unknown", "--lang", "zh-CN"], "参数无效", "Invalid arguments:")):
        result = run(args)
        assert result.returncode == 2 and not result.stdout
        assert (zh if chinese_available else en) in result.stderr, result.stderr

print(f"{checks} offline app launch CLI checks passed (help, validation, language and launch request structure)")
