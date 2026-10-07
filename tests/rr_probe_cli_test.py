"""真实探针的离线 CLI 回归：帮助/错误先退出，包输出的语言不改变字节。"""
import os
from pathlib import Path
import platform
import re
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
# 可传入 sandbox-exec 等前缀，在本机回归时强制禁止网络访问。
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


options = [
    "--seconds", "--attempts", "--hz", "--timeout", "--what", "--no-timeout-key",
    "--dump-packets", "--dump-status", "--audio-out", "--audio-leg", "--audio-rr",
    "--offer", "--avc-features", "--event-channel", "--hold", "--hold-idle",
    "--hold-no-poll", "--display-subscribe", "--hid-attach", "--ping6", "--udp-mdns",
    "--udp-canary", "--flow-label", "--request-always", "--verbose", "--lang",
]


def before_packets(result):
    # stdout 无缓冲；包自检先于所有设备连接，帮助和错误路径不能进入该阶段。
    assert "Packet self-check" not in result.stdout and "包自检" not in result.stdout, result.stdout


for args in (["--help"], ["-h"], ["--hold-idle", "--help"]):
    result = run(args)
    assert result.returncode == 0, (args, result.stderr)
    assert "without connecting to a device" in result.stdout, result.stdout
    before_packets(result)
    for option in options:
        assert option in result.stdout, (option, result.stdout)

invalid = [
    ["--unknown"], ["unexpected-position"], ["--dump-packets", "--unknown"],
    ["--seconds", "bad"], ["--seconds", "0"], ["--seconds", "-1"],
    ["--seconds", "2147483647"], ["--seconds", "3x"],
    ["--attempts", "0"], ["--attempts", "-1"], ["--attempts", "99999999999999"],
    ["--timeout", "-1"], ["--timeout", "4294967296"],
    ["--hz", "nan"], ["--hz", "inf"], ["--hz", "-inf"],
    ["--hz", "0"], ["--hz", "-1"], ["--hz", "1e-100"],
    ["--udp-canary", "65536"], ["--udp-canary", "-1"], ["--udp-canary", "oops"],
    ["--what", "oops"], ["--what", ""], ["--what", "none,"],
    ["--what", ",rrsrc"], ["--what", "none,,rrsrc"],
    ["--what", "fir+oops"], ["--what", "fir+"], ["--what", "fir++fb"],
]
for option in ("--seconds", "--attempts", "--hz", "--timeout", "--what",
               "--audio-out", "--offer", "--avc-features"):
    invalid.append([option])
for args in invalid:
    result = run(args)
    assert result.returncode > 0, (args, result.returncode, result.stdout, result.stderr)
    assert result.stderr, (args, result.stdout)
    before_packets(result)


# 原探针固定样本的字节及顺序。包头、SSRC、长度和私有字段均须保持；
# 仅 Companion 的诊断标签可翻译。这里不推断设备是否接受这些反馈。
packet_bytes = [
    ("RR", "81c90007 11111111 22222222 00000000 00000003 00000000 00000000 00000000"),
    ("RR+SDES", "81c90007 11111111 22222222 00000000 00000003 00000000 00000000 00000000 81ca0002 11111111 01000000"),
    ("SR", "80c80006 11111111 00000000 00000000 00000000 00000000 00000000"),
    ("RCTL", "80cc0007 11111111 5243544c 85000004 22220000 0000000a 00000000 0064ea61"),
    ("Companion", "80cc0003 11111111 00000005 22222222"),
]


def packets_are_unchanged(result, chinese=False):
    assert result.returncode == 0 and not result.stderr, (result.returncode, result.stderr)
    rows = re.findall(r"^(RR\+SDES|RR|SR|RCTL|Companion|伴随包)\s+([0-9a-f ]+)$",
                      result.stdout, re.MULTILINE)
    actual = [("Companion" if label == "伴随包" else label, value) for label, value in rows]
    assert actual == packet_bytes, (actual, result.stdout)
    assert rows[-1][0] == ("伴随包" if chinese else "Companion"), result.stdout
    if not chinese:
        assert not any("\u4e00" <= c <= "\u9fff" for c in result.stdout), result.stdout

arms = ("none,poll,poll5,rr,rrsame,rrsdes,rrp1,rrall,rrneg,rrnegp1,rrnegsr,"
        "rrmine,rrminep1,rrminesr,rrminesd,rrminecname,rrsrc,rrsrcsd,rctl,rctlrr,pli,fir")
valid = [
    ["--dump-packets"],
    ["--dump-packets", "--what", arms],
    ["--dump-packets", "--what", "none+fb,rrsrc+ltrp,fir+fb+ltrp, pli"],
    ["--dump-packets", "--seconds", "1", "--seconds", "2"],
    ["--dump-packets", "--udp-canary"],
    ["--dump-packets", "--udp-canary", "0"],
    ["--dump-packets", "--udp-canary", "65535"],
    ["--dump-packets", "--udp-canary=47891"],
    ["--dump-packets", "--hz", "0.5", "--timeout", "0"],
    ["--dump-packets", "--timeout", "4294967295"],
    ["--dump-packets", "--avc-features", ""],
    ["--dump-packets", "--seconds", "30", "--attempts", "2", "--hz", "10",
     "--timeout", "20", "--what", "none,rrsrc,pli,fir,fir+fb", "--no-timeout-key",
     "--dump-status", "--audio-out", "unused-audio.bin", "--audio-leg", "--audio-rr",
     "--offer", "unused-offer.bin", "--avc-features", "FLS;SW:1;", "--event-channel",
     "--hold", "--hold-idle", "--hold-no-poll", "--display-subscribe", "--hid-attach",
     "--ping6", "--udp-mdns", "--udp-canary", "47891", "--flow-label",
     "--request-always", "-v", "--verbose"],
]
for args in valid:
    result = run(args)
    assert result.returncode == 0, (args, result.returncode, result.stderr)
    assert "RR        " in result.stdout and "RCTL      " in result.stdout, result.stdout
    assert not result.stderr, result.stderr
    packets_are_unchanged(result)


# 与 i18n_test.py 保持同一环境边界：glibc 仅有 C 类 locale 时允许回退英文，
# 其余环境必须实际输出中文，不能因为漏译而静默跳过。
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
    description = ("比较 USB 连接上的 RTCP 保活与关键帧请求策略" if use_chinese else
                   "Compare RTCP keepalive and keyframe request strategies over USB")
    assert description in result.stdout, (args, result.stdout)
    assert ("显示帮助并退出，不连接设备" if use_chinese else
            "Show help and exit without connecting to a device") in result.stdout, result.stdout
    assert ("选项" if use_chinese else "Options") in result.stdout, result.stdout
    for option in options:
        assert option in result.stdout, (option, result.stdout)
    before_packets(result)
    if not use_chinese:
        assert not any("\u4e00" <= c <= "\u9fff" for c in result.stdout), result.stdout


help_is(["--lang", "en", "--help"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--help", "--lang", "en"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--lang", "zh-CN", "--help"], chinese=True, variables={"LC_ALL": "C"})
help_is(["--help", "--lang", "zh-CN"], chinese=True, variables={"LC_ALL": "C"})
help_is(["--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
help_is(["--lang", "auto", "--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
help_is(["--lang", "auto", "--help"], variables={"LANG": "en_US.UTF-8"})
help_is(["--help"], variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})
help_is(["--lang", "auto", "--help"], variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})
help_is(["--help"], variables={"LANG": "zh_CN.UTF-8", "LC_MESSAGES": "en_US.UTF-8"})
help_is(["--help"], variables={"LANG": "C"})

for args, variables, chinese in (
    (["--lang", "zh-CN", "--dump-packets"], {"LC_ALL": "C"}, True),
    (["--dump-packets", "--lang", "zh-CN"], {"LC_ALL": "C"}, True),
    (["--dump-packets"], {"LANG": "zh_CN.UTF-8"}, True),
    (["--lang", "en", "--dump-packets"], {"LANG": "zh_CN.UTF-8"}, False),
    (["--lang", "auto", "--dump-packets"], {"LANG": "zh_CN.UTF-8", "LC_ALL": "C"}, False),
):
    result = run(args, variables)
    use_chinese = chinese and chinese_available
    packets_are_unchanged(result, use_chinese)
    assert ("包自检" if use_chinese else "Packet self-check") in result.stdout, result.stdout

for args in (["--lang"], ["--lang", "invalid"], ["--help", "--lang=invalid"]):
    result = run(args)
    assert result.returncode > 0 and result.stderr, (args, result.returncode, result.stderr)
    before_packets(result)

for args in (["--lang", "zh-CN", "--unknown"], ["--unknown", "--lang", "zh-CN"]):
    result = run(args)
    assert result.returncode > 0, (args, result.returncode)
    assert ("参数无效：" if chinese_available else "Invalid arguments:") in result.stderr, result.stderr
    before_packets(result)

print(f"{checks} offline CLI checks passed (original options/arms, packet bytes and language selection)")
