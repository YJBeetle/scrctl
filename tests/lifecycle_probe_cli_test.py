"""历史媒体实验：参数与时间预算在连接设备前校验，dry-run 不运行媒体实验。"""
from pathlib import Path
import subprocess
import sys
import tempfile


names = ("wake_latency", "hid_gate", "restart_gap", "lease_renew", "lifetime", "two_session")
if len(sys.argv) != 7:
    raise SystemExit("usage: lifecycle_probe_cli_test.py WAKE HID_GATE RESTART_GAP LEASE_RENEW LIFETIME TWO_SESSION")
binaries = dict(zip(names, (str(Path(arg).resolve()) for arg in sys.argv[1:])))
checks = 0


def run(name, args):
    global checks
    result = subprocess.run([binaries[name], *map(str, args)], capture_output=True,
                            text=True, encoding="utf-8", timeout=5)
    checks += 1
    return result


def plan(name, args, *expected):
    result = run(name, ["--dry-run", *args])
    assert result.returncode == 0 and not result.stderr, (name, args, result)
    assert all(text in result.stdout for text in expected), (name, args, expected, result.stdout)


def invalid(name, args):
    result = run(name, ["--dry-run", *args])
    assert result.returncode == 2 and result.stderr and not result.stdout, (name, args, result)


for name in names:
    for flag in ("--help", "-h"):
        result = run(name, [flag])
        assert result.returncode == 0 and not result.stderr, (name, result)
        assert "--dry-run" in result.stdout and "stopAll" in result.stdout, (name, result.stdout)
    invalid(name, ["--unknown"])

plan("wake_latency", [], "timeout=20", "RR=off", "quiet_ms=9000", "wait_ms=45000")
plan("wake_latency", ["--rr", "device-id"], "RR=on")
plan("hid_gate", [], "timeout=20", "RR=off", "quiet_ms=9000", "wait_ms=45000")
plan("restart_gap", [], "timeout=20", "RR=off", "gaps=0,250,500,1000,2000")
plan("lease_renew", [], "timeout=20", "RR=on", "observation_ms=50000", "feed=off")
plan("lifetime", [], "timeout=20", "RR=off", "observation_ms=70000", "feed_until=-1")
plan("two_session", [], "timeout=3600", "RR=off", "gap_ms=1500", "concurrent_ms=6000")

# leading zero 和 plus 必须仍是十进制，不能变成八进制或丢掉数值尾部。
positive = {
    "wake_latency": ("--trials", "--quiet", "--wait-ms"),
    "hid_gate": ("--lease", "--quiet-ms", "--wait-ms"),
    "restart_gap": ("--trials", "--lease", "--wait-ms"),
    "lease_renew": ("--seconds", "--press-ms"),
    "lifetime": ("--rounds", "--max-seconds", "--lease"),
    "two_session": (),
}
for name, options in positive.items():
    for option in options:
        for value in ("0", "-1", "bad", "10x", "1.5", "0x10", "0o10", "", "+-0", "++1",
                      "2147483648", "9999999999999999999"):
            invalid(name, [option, value])
        invalid(name, [option])

plan("wake_latency", ["--trials", "010", "--quiet", "010", "--wait-ms", "+0100"],
     "trials=10", "quiet_ms=10", "wait_ms=100")
plan("hid_gate", ["--lease", "010", "--quiet-ms", "010", "--wait-ms", "0100", "device-id"],
     "timeout=10", "quiet_ms=10", "wait_ms=100")
plan("restart_gap", ["--gaps", "000,+010, 020 ", "--lease", "010", "--trials", "010"],
     "gaps=0,10,20", "timeout=10", "trials=10")
plan("lease_renew", ["--seconds", "010", "--press-ms", "010", "--feed", "device-id"],
     "observation_ms=10000", "press_ms=10", "feed=on")
plan("lifetime", ["--max-seconds", "010", "--rounds", "010", "--feed-until", "+010"],
     "observation_ms=10000", "rounds=10", "feed_until=10")
plan("two_session", ["--gap", "010"], "gap_ms=10")

# INT_MAX 秒必须先转为 64 位再乘 1000；大预算只规划，不等待。
plan("lease_renew", ["--seconds", "2147483647"], "observation_ms=2147483647000")
plan("lifetime", ["--max-seconds", "2147483647"], "observation_ms=2147483647000")
plan("wake_latency", ["--trials", "2147483647", "--quiet", "2147483646", "--wait-ms", "2147483647"],
     "trials=2147483647", "quiet_ms=2147483646", "wait_ms=2147483647")
plan("restart_gap", ["--gaps", "0,2147483647", "--trials", "2147483647"], "gaps=0,2147483647")
plan("two_session", ["--gap", "0"], "gap_ms=0")
plan("two_session", ["--gap", "2147483647"], "gap_ms=2147483647")
plan("lifetime", ["--feed-until", "-1"], "feed_until=-1")
plan("lifetime", ["--feed-until", "0"], "feed_until=0")
for value in ("-2", "+-1", "1x", "2147483648"):
    invalid("lifetime", ["--feed-until", value])
for value in ("-1", "1x", "", "+-0", "2147483648"):
    invalid("two_session", ["--gap", value])
for value in ("", ",", "0,", ",0", "0,,1", "-1", "0,-1", "0,1x", "0,+-0", "2147483648"):
    invalid("restart_gap", ["--gaps", value])
for name, option in (("wake_latency", "--quiet"), ("hid_gate", "--quiet-ms")):
    invalid(name, [option, "10", "--wait-ms", "10"])
    invalid(name, [option, "11", "--wait-ms", "10"])

# 带空格或 shell 元字符的目录在规划阶段不能创建，更不能经过 shell 执行。
with tempfile.TemporaryDirectory(prefix="scrctl-lifecycle-cli-") as temp:
    target = Path(temp) / "not created; directory"
    plan("hid_gate", ["--dir", target], "RR=off")
    assert not target.exists()

print(f"{checks} CLI checks passed; no device session was started")
