"""探针参数必须在包自检和设备连接之前完成检查。"""
from pathlib import Path
import subprocess
import sys


binary = str(Path(sys.argv[1]).resolve())
# 可传入 sandbox-exec 等前缀，在本机回归时强制禁止网络访问。
prefix = sys.argv[2:]
checks = 0


def run(args):
    global checks
    result = subprocess.run([*prefix, binary, *args], capture_output=True,
                            text=True, encoding="utf-8", timeout=3)
    checks += 1
    return result


options = [
    "--seconds", "--attempts", "--hz", "--timeout", "--what", "--no-timeout-key",
    "--dump-packets", "--dump-status", "--audio-out", "--audio-leg", "--audio-rr",
    "--offer", "--avc-features", "--event-channel", "--hold", "--hold-idle",
    "--hold-no-poll", "--display-subscribe", "--hid-attach", "--ping6", "--udp-mdns",
    "--udp-canary", "--flow-label", "--request-always", "--verbose",
]
for args in (["--help"], ["-h"], ["--hold-idle", "--help"]):
    result = run(args)
    assert result.returncode == 0, (args, result.stderr)
    assert "without connecting to a device" in result.stdout, result.stdout
    assert "包自检" not in result.stdout, result.stdout
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
    # stdout 无缓冲；建立设备会话前一定先打印包自检，因此错误不能到达这条路径。
    assert "包自检" not in result.stdout, (args, result.stdout)

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

print(f"{checks} offline CLI checks passed (help, errors, all options and arms)")
