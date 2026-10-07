"""以真实探针的独立进程验证帮助、参数错误及语言选择，不连接设备。"""
import os
from pathlib import Path
import platform
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
# 可传入 sandbox-exec 等前缀，额外禁止本机网络访问；CTest 不依赖平台沙箱。
prefix = sys.argv[2:]
base = {k: v for k, v in os.environ.items() if k not in
        ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "SCRCTL_LOCALEDIR")}
checks = 0


def run(args, variables=None):
    global checks
    result = subprocess.run([*prefix, binary, *args], env={**base, **(variables or {})},
                            capture_output=True, text=True, encoding="utf-8", timeout=3)
    checks += 1
    return result


# 与 i18n_test.py 的 C-only 检查相同：glibc 只有 C 类 locale 时 gettext 不能
# 加载中文。先用独立 Python 进程探测，不让探测本身改变测试进程的 locale。
chinese_available = True
if platform.libc_ver()[0] == "glibc":
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


def help_is(args, chinese=False, variables=None):
    result = run(args, variables)
    assert result.returncode == 0 and not result.stderr, (args, result.stderr)
    # 共享文案验证语言选择，新增探针描述另行断言，避免漏译时仍通过。
    expected = ("选项", "显示帮助") if chinese and chinese_available else ("Options", "Show help")
    assert all(text in result.stdout for text in expected), (args, result.stdout)
    description = ("获取设备截图并保存为 PNG" if chinese and chinese_available else
                   "Capture a device screenshot and save it as PNG")
    assert description in result.stdout, (args, result.stdout)
    for option in ("--help", "--verbose", "--out", "--lang", "UDID"):
        assert option in result.stdout, (option, result.stdout)
    assert "Device:" not in result.stdout and "设备：" not in result.stdout, result.stdout
    if not chinese or not chinese_available:
        assert not any("\u4e00" <= c <= "\u9fff" for c in result.stdout), result.stdout


help_is(["--help"], variables={"LANG": "en_US.UTF-8"})
help_is(["-h", "-v", "-o", "unused.png", "offline-udid"])
help_is(["--verbose", "--out", "未使用.png", "offline-udid", "--help"])
help_is(["--lang", "en", "--help"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--help", "--lang", "zh-CN"], chinese=True, variables={"LC_ALL": "C"})
help_is(["--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
help_is(["--lang", "auto", "--help"], variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})

for args in (["--unknown"], ["-o"], ["--out"], ["--lang"],
             ["--lang", "invalid"], ["--lang=invalid", "--help"],
             ["first-udid", "second-udid"]):
    result = run(args, {"LANG": "en_US.UTF-8"})
    assert result.returncode == 2, (args, result.returncode, result.stdout, result.stderr)
    assert result.stderr and not result.stdout, (args, result.stdout, result.stderr)
    # 设备连接及截图失败使用退出码 1；参数错误必须在这些操作之前 exit 2。
    assert "Failed to establish device session" not in result.stderr, (args, result.stderr)

print(f"{checks} offline screenshot CLI checks passed (help, errors, aliases and language selection)")
