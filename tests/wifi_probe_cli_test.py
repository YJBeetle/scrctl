"""真实 Wi-Fi 探针的离线 CLI 检查；帮助及参数错误须先于文件、USB 和网络操作退出。"""
import os
from pathlib import Path
import platform
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
# 本机额外验证时可传 sandbox-exec 前缀；CTest 不依赖平台沙箱。
prefix = sys.argv[2:]
base = {k: v for k, v in os.environ.items() if k not in
        ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "SCRCTL_LOCALEDIR")}
checks = 0


def run(args, variables=None):
    global checks
    result = subprocess.run([*prefix, binary, *args], capture_output=True,
                            env={**base, "LANG": "en_US.UTF-8", **(variables or {})},
                            text=True, encoding="utf-8", timeout=3)
    checks += 1
    return result


# 与其他 CLI 测试相同：glibc 只有 C 类 message locale 时 gettext 会回退英文。
chinese_available = True
if platform.libc_ver()[0] == "glibc":
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


options = ("--address", "--port", "--record", "--pmd3-record", "--host-id", "--udid",
           "--verbose", "--tunnel", "--rsd", "--pair-setup", "--pair-setup-xpc",
           "--xpc-service", "--usb-services", "--no-save", "--no-verify-probe",
           "--host-name", "--pairing-kind", "--lang")


def help_is(args, chinese=False, variables=None):
    result = run(args, variables)
    assert result.returncode == 0 and not result.stderr, (args, result.stderr)
    expected = ("选项", "显示帮助") if chinese and chinese_available else ("Options", "Show help")
    assert all(text in result.stdout for text in expected), (args, result.stdout)
    description = ("检查 RemotePairing 连接和 USB 配对服务" if chinese and chinese_available else
                   "Inspect RemotePairing connections and USB pairing services")
    assert description in result.stdout, (args, result.stdout)
    assert all(option in result.stdout for option in options), result.stdout
    if not chinese or not chinese_available:
        assert not any("\u4e00" <= c <= "\u9fff" for c in result.stdout), result.stdout


for args in (["--help"], ["-h"], ["--usb-services", "--udid", "offline-udid", "--help"],
             ["--pair-setup", "--help"],
             ["--pair-setup", "--address", "192.0.2.1", "--port", "65535",
              "--host-id", "offline-host", "--udid", "offline-udid", "--host-name", "test-host",
              "--pairing-kind", "setupManualPairing", "--no-save", "--no-verify-probe", "--help"],
             ["--pair-setup-xpc", "--xpc-service", "test-service", "--no-save", "--help"],
             ["--address", "192.0.2.1", "--port", "1", "--record", "unused.pair",
              "--rsd", "--tunnel", "--verbose", "--help"],
             ["--address", "2001:db8::1", "--pmd3-record", "unused.plist", "--host-id",
              "offline-host", "--udid", "offline-udid", "-v", "--help"]):
    help_is(args)

help_is(["--lang", "en", "--help"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--help", "--lang", "zh-CN"], chinese=True, variables={"LC_ALL": "C"})
help_is(["--lang", "zh-CN", "--help"], chinese=True)
help_is(["--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
help_is(["--lang", "auto", "--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
help_is(["--help"], variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})
help_is(["--help"], variables={"LANG": "zh_CN.UTF-8", "LC_MESSAGES": "en_US.UTF-8"})
help_is(["--help"], chinese=True,
        variables={"LANG": "en_US.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8"})
help_is(["--help"], variables={"LANG": "unrecognized_LOCALE"})


invalid = [["--unknown"], ["unexpected-position"], [], ["--address", "192.0.2.1"],
           ["--record", "missing.pair"], ["--port", "49152"],
           ["--pmd3-record", "missing.plist"],
           ["--address", "192.0.2.1", "--pmd3-record", "missing.plist", "--host-id", ""],
           ["--address", "192.0.2.1", "--record", "missing.pair", "--pmd3-record", "missing.plist",
            "--host-id", "offline-host"],
           ["--usb-services", "--address", "192.0.2.1"],
           ["--pair-setup-xpc", "--address", "192.0.2.1"],
           ["--usb-services", "--port", "49152"],
           ["--pair-setup-xpc", "--port", "49152"],
           ["--pair-setup", "--port", "49152"],
           ["--xpc-service", "test-service"],
           ["--pair-setup", "--xpc-service", "test-service"],
           ["--host-id", "offline-host"],
           ["--usb-services", "--host-id", "offline-host"],
           ["--address", "192.0.2.1", "--record", "missing.pair", "--udid", "offline-udid"]]
for option in ("--address", "--port", "--record", "--pmd3-record", "--host-id", "--udid",
               "--xpc-service", "--host-name", "--pairing-kind", "--lang"):
    invalid.append([option])
for value in ("0", "-1", "65536", "bad", "1x", "1.5", "1e3", "999999999999999999999", ""):
    invalid.append(["--address", "192.0.2.1", "--record", "missing.pair", "--port", value])
modes = ("--usb-services", "--pair-setup", "--pair-setup-xpc")
for first, second in ((modes[0], modes[1]), (modes[0], modes[2]), (modes[1], modes[2])):
    invalid.append([first, second])
    invalid.append([second, first])
for option in ("--no-save", "--no-verify-probe", "--host-name", "--pairing-kind"):
    value = [] if option.startswith("--no-") else ["test-value"]
    invalid.append(["--usb-services", option, *value])
    invalid.append(["--address", "192.0.2.1", "--record", "missing.pair", option, *value])
for mode in modes:
    for option in ("--record", "--pmd3-record", "--tunnel", "--rsd"):
        value = ["missing.pair"] if option in ("--record", "--pmd3-record") else []
        extra = ["--host-id", "offline-host"] if option == "--pmd3-record" else []
        invalid.append([mode, option, *value, *extra])


def invalid_is(args, chinese=False, variables=None):
    result = run(args, variables)
    assert result.returncode == 2, (args, result.returncode, result.stdout, result.stderr)
    assert not result.stdout and result.stderr, (args, result.stdout, result.stderr)
    expected = "参数无效：" if chinese and chinese_available else "Invalid arguments:"
    # 连接、记录读取或 USB 失败不能冒充 CLI 错误，且不存在协议阶段的 stdout。
    assert result.stderr.startswith(expected), (args, result.stderr)


for args in invalid:
    invalid_is(args)
for args in (["--lang", "invalid"], ["--lang=invalid", "--help"]):
    result = run(args)
    assert result.returncode == 2 and not result.stdout, (args, result)
    assert result.stderr.startswith("--lang must be auto, en or zh-CN"), result.stderr
invalid_is(["--lang", "zh-CN", "--port", "65536"], chinese=True)
invalid_is(["--unknown"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
invalid_is(["--usb-services", "--no-save", "--lang", "en"],
           variables={"LANG": "zh_CN.UTF-8"})
invalid_is(["--pair-setup", "--pair-setup-xpc", "--lang", "zh-CN"], chinese=True)

print(f"{checks} offline Wi-Fi CLI checks passed (help, modes, option scope, ports and locales)")
