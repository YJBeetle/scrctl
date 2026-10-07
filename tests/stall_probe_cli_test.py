"""真实 stall_probe 的离线 CLI 回归：所有命令在连接设备之前退出。"""
import os
from pathlib import Path
import platform
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
# catalog 合并前可临时只测英文；CTest 默认必须验证实际的中英文输出。
runner_args = sys.argv[2:]
english_only = bool(runner_args and runner_args[0] == "--english-only")
prefix = runner_args[1:] if english_only else runner_args
# 可追加 sandbox-exec 等运行前缀，禁止测试进程访问网络。
base = {k: v for k, v in os.environ.items() if k not in
        ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "SCRCTL_LOCALEDIR")}
checks = 0


def run(args, variables=None):
    global checks
    result = subprocess.run([*prefix, binary, *args],
                            env={**base, "LANG": "en_US.UTF-8", **(variables or {})},
                            capture_output=True, text=True, encoding="utf-8", timeout=3)
    checks += 1
    # 连接失败会打印此提示并返回 1；参数处理应在 open_device 之前完成。
    for marker in ("Failed to establish device session", "无法建立设备会话",
                   "Failed to start video stream", "无法启动视频流",
                   "Test:", "测试配置：", "Summary:", "统计："):
        assert marker not in result.stdout + result.stderr, (args, result.stdout, result.stderr)
    return result


options = ("--help", "--wifi", "--serial", "--verbose", "--stall-ms",
           "--drop-at-packet", "--ignore-video-after", "--fail-decode",
           "--fail-every-keyframe", "--watch", "--keep-pli", "--lang")

# 与已有探针测试一样，仅在 glibc 没有可用的 UTF-8 消息 locale 时允许英文回退。
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
    use_chinese = chinese and chinese_available
    expected = (
        ("注入丢包和解码失败，检查视频恢复行为", "选项", "显示帮助",
         "--help 不连接设备。", "等待关键帧的上限，单位为毫秒")
        if use_chinese else
        ("Test video recovery with packet loss and decode failures", "Options", "Show help",
         "--help does not connect to a device.", "Keyframe wait limit in milliseconds"))
    for text in expected:
        assert text in result.stdout, (args, text, result.stdout)
    for option in options:
        assert option in result.stdout, (args, option, result.stdout)
    if not use_chinese:
        assert not any("\u4e00" <= c <= "\u9fff" for c in result.stdout), result.stdout


help_is(["--help"])
help_is(["-h"])
help_is(["--lang", "en", "--help"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--help", "--lang", "en"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--lang", "en", "--help"], variables={"LC_ALL": "zh_CN.UTF-8"})
help_is(["--lang", "auto", "--help"])
help_is(["--help"], variables={"LANG": ""})
help_is(["--help"], variables={"LANG": "C"})
help_is(["--help"], variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})
help_is(["--help"], variables={"LANG": "zh_CN.UTF-8", "LC_MESSAGES": "en_US.UTF-8"})
help_is(["--help"], variables={"LC_ALL": "en_US.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8"})
help_is(["--help"], variables={"LANGUAGE": "zh_CN"})
# 有效参数及全部别名仍须能走帮助路径，不能尝试打开这条虚构的设备连接。
help_is(["--wifi", "192.0.2.123", "-s", "offline-udid", "-v", "--stall-ms", "4000",
         "--drop-at-packet", "700", "--ignore-video-after", "900", "--fail-decode", "1",
         "--fail-every-keyframe", "0", "--watch", "20000", "--keep-pli", "--help"])
help_is(["--serial", "offline-udid", "--verbose", "--drop-at-packet", "0",
         "--ignore-video-after", "0", "--fail-decode", "0", "--fail-every-keyframe", "4",
         "--watch", "1", "--stall-ms", "1", "--help"])


def invalid_is(args, code, marker=None, chinese=False, variables=None):
    result = run(args, variables)
    assert result.returncode == code, (args, code, result.returncode, result.stdout, result.stderr)
    assert result.stderr and not result.stdout, (args, result.stdout, result.stderr)
    if marker:
        assert marker in result.stderr, (args, marker, result.stderr)
    if not chinese:
        assert not any("\u4e00" <= c <= "\u9fff" for c in result.stderr), result.stderr


# 原 CLI11 错误码保持不变：Extras=109，ArgumentMismatch=114，Validation=105。
for args in (["--unknown"], ["unexpected-position"], ["--wifi", "192.0.2.123", "--unknown"],
             ["--serial", "offline-udid", "--unknown"]):
    invalid_is(args, 109, "Invalid arguments:")
for option in ("--wifi", "--serial", "-s", "--stall-ms", "--drop-at-packet",
               "--ignore-video-after", "--fail-decode", "--fail-every-keyframe", "--watch",
               "--lang"):
    invalid_is([option], 114, "Invalid arguments:")
for option in ("--stall-ms", "--watch"):
    for value in ("0", "-1", "bad", "3x"):
        invalid_is([option, value], 105, "Invalid arguments:")
for option in ("--drop-at-packet", "--ignore-video-after", "--fail-decode", "--fail-every-keyframe"):
    for value in ("-1", "bad", "3x"):
        invalid_is([option, value], 105, "Invalid arguments:")
# 数字通过检查后仍不能超出 int 的转换范围；这是原 CLI11 ConversionError=104。
for option in ("--stall-ms", "--drop-at-packet", "--ignore-video-after", "--fail-decode",
               "--fail-every-keyframe", "--watch"):
    invalid_is([option, "2147483648"], 104, "Invalid arguments:")
for args in (["--lang", "invalid"], ["--lang=invalid", "--help"],
             ["--help", "--lang=invalid"]):
    invalid_is(args, 2, "--lang must be auto, en or zh-CN")

# 四处跨参数校验及已有边界。每个失败都在 open_device 之前返回 2。
constraints = [
    (["--keep-pli"], "--keep-pli requires", "--keep-pli 需要"),
    (["--keep-pli", "--fail-decode", "1", "--fail-every-keyframe", "4"],
     "--keep-pli requires", "--keep-pli 需要"),
    (["--ignore-video-after", "740"], "must exceed", "必须大于"),
    (["--drop-at-packet", "200", "--ignore-video-after", "240"], "must exceed", "必须大于"),
    (["--drop-at-packet", "199"], "must be 0 or at least 200", "必须为 0 或至少 200"),
    (["--keep-pli", "--fail-decode", "1", "--drop-at-packet", "199"],
     "must be 0 or at least 200", "必须为 0 或至少 200"),
    (["--fail-every-keyframe", "1"], "must be 0 or at least 4", "必须为 0 或至少 4"),
    (["--fail-every-keyframe", "3", "--drop-at-packet", "1", "--ignore-video-after", "1"],
     "must be 0 or at least 4", "必须为 0 或至少 4"),
]
for args, english, _ in constraints:
    invalid_is(args, 2, english)

if not english_only:
    for args, variables in (
        (["--lang", "zh-CN", "--help"], {"LC_ALL": "C"}),
        (["--help", "--lang", "zh-CN"], {"LC_ALL": "C"}),
        (["--help"], {"LANG": "zh_CN.UTF-8"}),
        (["--lang", "auto", "--help"], {"LANG": "zh_CN.UTF-8"}),
        (["--help"], {"LANG": "en_US.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8"}),
        (["--help"], {"LANG": "en_US.UTF-8", "LC_MESSAGES": "en_US.UTF-8",
                      "LC_ALL": "zh_CN.UTF-8"}),
    ):
        help_is(args, chinese=True, variables=variables)
    for args in (["--lang", "zh-CN", "--unknown"], ["--unknown", "--lang", "zh-CN"]):
        invalid_is(args, 109, "参数无效：" if chinese_available else "Invalid arguments:",
                   chinese=chinese_available, variables={"LC_ALL": "C"})
    invalid_is(["--unknown"], 109,
               "参数无效：" if chinese_available else "Invalid arguments:",
               chinese=chinese_available, variables={"LANG": "zh_CN.UTF-8"})
    invalid_is(["--keep-pli"], 2,
               "--keep-pli 需要" if chinese_available else "--keep-pli requires",
               chinese=chinese_available, variables={"LANG": "zh_CN.UTF-8"})
    for args, english, chinese in constraints:
        for ordered in (["--lang", "zh-CN", *args], [*args, "--lang", "zh-CN"]):
            invalid_is(ordered, 2, chinese if chinese_available else english,
                       chinese=chinese_available, variables={"LC_ALL": "C"})

print(f"{checks} offline stall CLI checks passed (help, language selection, parse errors and constraints)")
