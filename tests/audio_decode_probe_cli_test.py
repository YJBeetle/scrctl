"""离线音频解码入口：参数校验先于 I/O，读写失败不得报告保存成功。"""
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile


binary = str(Path(sys.argv[1]).resolve())
# 译文尚未合入的独立构建可用 --english-only；其余参数可作为执行沙箱等前缀。
extra = sys.argv[2:]
english_only = "--english-only" in extra
prefix = [arg for arg in extra if arg != "--english-only"]
base = {k: v for k, v in os.environ.items() if k not in
        ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "SCRCTL_LOCALEDIR")}
checks = 0


def run(args, variables=None, **kwargs):
    global checks
    result = subprocess.run([*prefix, binary, *map(str, args)],
                            env={**base, "LANG": "en_US.UTF-8", **(variables or {})},
                            capture_output=True, text=True, encoding="utf-8", timeout=5,
                            **kwargs)
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
    assert result.returncode == 0 and not result.stderr, (args, result.stderr)
    zh = chinese and chinese_available
    expected = ("将录制的 AAC-ELD 音频数据报解码为 WAV", "显示帮助") if zh else (
        "Decode a recorded AAC-ELD audio dump to a WAV file", "Show help")
    assert all(text in result.stdout for text in expected), (args, result.stdout)
    for text in ("INPUT", "OUTPUT", "RATE", "CHANNELS", "--lang", "101", "480", "48000", "32767"):
        assert text in result.stdout, (text, result.stdout)
    if not zh:
        assert "--help does not read or write files" in " ".join(result.stdout.split()), result.stdout
        assert not any("\u4e00" <= c <= "\u9fff" for c in result.stdout), result.stdout


for args in (["--help"], ["-h"], ["input.rtp", "output.wav", "--help"],
             ["input.rtp", "output.wav", "48000", "2", "--help"],
             ["--help", "--lang", "en"], ["--lang", "auto", "--help"]):
    help_is(args)
help_is(["--help", "--lang", "en"], variables={"LANG": "zh_CN.UTF-8"})
help_is(["--lang", "auto", "--help"], variables={"LANG": "zh_CN.UTF-8", "LC_ALL": "C"})
if not english_only:
    help_is(["--lang", "zh-CN", "--help"], chinese=True, variables={"LC_ALL": "C"})
    help_is(["--help", "--lang", "zh-CN"], chinese=True)
    help_is(["--help"], chinese=True, variables={"LANG": "zh_CN.UTF-8"})
    help_is(["--help"], chinese=True, variables={"LANG": "en_US.UTF-8", "LC_MESSAGES": "zh_CN.UTF-8"})


with tempfile.TemporaryDirectory(prefix="scrctl-audio-decode-cli-") as temp:
    root = Path(temp)
    missing = root / "missing input.rtp"
    protected_output = root / "existing output.wav"
    sentinel = b"existing output must survive CLI and input failures"
    protected_output.write_bytes(sentinel)
    invalid = [[], [missing], [missing, protected_output, "48000", "2", "extra"],
               ["--unknown"], ["--lang"], ["--lang", "invalid"],
               ["--lang=invalid", "--help"],
               [missing, protected_output, "0"], [missing, protected_output, "-1"],
               [missing, protected_output, "48000", "0"],
               [missing, protected_output, "48000", "-1"],
               [missing, protected_output, "48000", "32768"],
               [missing, protected_output, "1073741824", "2"],
               [missing, protected_output, "65539", "32767"]]
    for value in ("bad", "3x", "1.5", "1e3", "", "2147483648", "9999999999999999999"):
        invalid += [[missing, protected_output, value], [missing, protected_output, "48000", value]]
    for args in invalid:
        result = run(args)
        assert result.returncode == 2 and not result.stdout and result.stderr, (
            args, result.returncode, result.stdout, result.stderr)
        assert "Failed to load" not in result.stderr, result.stderr
        assert protected_output.read_bytes() == sentinel

    # 联合 byteRate 边界与四个位置参数被接受；业务失败只应来自不存在的输入文件。
    for values in ([], ["44100"], ["44100", "1"], ["1", "32767"],
                   ["2147483647", "1"], ["65538", "32767"]):
        result = run([missing, protected_output, *values])
        assert result.returncode == 1 and not result.stdout, (values, result.stdout, result.stderr)
        assert "Failed to load" in result.stderr and str(missing) in result.stderr, result.stderr
        assert protected_output.read_bytes() == sentinel

    result = run([missing, protected_output, "--help"])
    assert result.returncode == 0 and not result.stderr
    assert protected_output.read_bytes() == sentinel

    for content in (b"", b"\x00\x0c\x80"):
        dump = root / "incomplete input.rtp"
        dump.write_bytes(content)
        result = run([dump, protected_output])
        assert result.returncode == 1 and not result.stdout, (result.stdout, result.stderr)
        assert str(dump) in result.stderr and "no complete datagrams" in result.stderr, result.stderr
        assert protected_output.read_bytes() == sentinel

    if os.name == "posix" and Path("/dev/stdin").exists():
        # stdin 由 subprocess 创建为管道，fseek 必然失败；不能被误判为空 dump。
        result = run(["/dev/stdin", protected_output], input="not a seekable file")
        assert result.returncode == 1 and not result.stdout, (result.stdout, result.stderr)
        assert "/dev/stdin" in result.stderr and "Cannot seek input file" in result.stderr, result.stderr
        assert protected_output.read_bytes() == sentinel

    # 一条 PT100 RTPv2 数据报，不创建或调用解码器。无媒体后端的构建也应完整
    # 验证 WAV 写入；附加不完整尾记录用于保留有效前缀。
    packet = b"\x80\x64" + b"\0" * 10 + b"x"
    dump = root / "valid prefix.rtp"
    dump.write_bytes(struct.pack(">H", len(packet)) + packet + b"\x00\x08x")
    wav = root / "empty PCM.wav"
    result = run([dump, wav])
    assert "Loaded 1 datagrams" in result.stdout, (result.stdout, result.stderr)
    assert "Audio decoder:" not in result.stdout, result.stdout
    assert result.returncode == 1 and not result.stderr, (result.stdout, result.stderr)
    assert f"Saved WAV: {wav}" in result.stdout
    data = wav.read_bytes()
    assert len(data) == 44 and data[:4] == b"RIFF" and data[8:16] == b"WAVEfmt "
    assert struct.unpack_from("<I", data, 4)[0] == 36
    assert struct.unpack_from("<HHIIHH", data, 20) == (1, 2, 48000, 192000, 4, 16)
    assert data[36:40] == b"data" and struct.unpack_from("<I", data, 40)[0] == 0
    for output in (root, root / "missing directory" / "output.wav"):
        result = run([dump, output])
        assert result.returncode == 1 and f"Failed to write WAV '{output}'" in result.stderr, (
            result.stdout, result.stderr)
        assert "Saved WAV:" not in result.stdout
    if os.name == "posix":
        import resource
        import signal

        def limit_child_output():
            # 限制仅作用于测试子进程，用真实 stdio 刷新失败验证 fclose 检查。
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (16, 16))

        failed_wav = root / "close failure.wav"
        result = run([dump, failed_wav], preexec_fn=limit_child_output)
        assert result.returncode == 1 and "Failed to close output file" in result.stderr, (
            result.stdout, result.stderr)
        assert str(failed_wav) in result.stderr and "Saved WAV:" not in result.stdout
    print("WAV writer integration checks passed (valid prefix, header, open and flush failures)")

    # PT101 仍会创建所选后端；这里只提交无效载荷验证入口，不代替真实 AAC-ELD 验证。
    audio_packet = b"\x80\x65" + b"\0" * 10 + b"x"
    audio_dump = root / "invalid audio.rtp"
    audio_dump.write_bytes(struct.pack(">H", len(audio_packet)) + audio_packet)
    audio_wav = root / "invalid audio.wav"
    result = run([audio_dump, audio_wav, "--backend", "ffmpeg"])
    assert result.returncode == 1, (result.returncode, result.stdout, result.stderr)
    if "Failed to create audio decoder" in result.stderr:
        assert not audio_wav.exists()
    else:
        assert "Audio decoder: libavcodec/aac-eld" in result.stdout, result.stdout
        assert audio_wav.is_file()

if not english_only:
    result = run(["missing.rtp", "out.wav", "bad", "--lang", "zh-CN"])
    assert result.returncode == 2 and not result.stdout and result.stderr
    assert ("参数无效" if chinese_available else "Invalid arguments") in result.stderr, result.stderr

print(f"{checks} offline audio decode checks passed (CLI, language selection and file I/O)")
