"""搬移安装目录后验证 Mach-O 闭包、dyld 实际加载与随包翻译，不建立设备连接。"""
import argparse
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import uuid

from record_macos_package import macos_version_tuple, minimum_macos


def checked(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                            timeout=30, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{command}: exit {result.returncode}\n{result.stderr}")
    return result


def inside(path, root):
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def system_library(path):
    return path.startswith(("/usr/lib/", "/System/Library/"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("install_directory", type=Path)
    parser.add_argument("build_directory", type=Path)
    args = parser.parse_args()
    install = args.install_directory.resolve(strict=True)
    build = args.build_directory.resolve(strict=True)
    catalog = build / "locale"
    with tempfile.TemporaryDirectory(prefix="scrctl-install-") as temporary:
        temporary = Path(temporary)
        moved = temporary / "relocated package"
        shutil.copytree(install, moved, symlinks=True)
        # 放在同一个构建目录，避免工作区和系统临时目录不在同一文件系统时
        # rename 触发 EXDEV；随机名也不会覆盖已有开发目录。
        hidden = build / ("locale-hidden-" + uuid.uuid4().hex)
        # 隐藏构建目录的翻译，任何退出路径都恢复；不修改环境或系统库。
        if catalog.exists():
            catalog.rename(hidden)
        try:
            executable = moved / "bin/scrctl"
            libraries = sorted((moved / "bin/lib").rglob("*.dylib"))
            if not libraries:
                raise RuntimeError("Package has no bundled dylibs")
            host_macos = platform.mac_ver()[0]
            host_version = macos_version_tuple(host_macos)
            minimum_versions = {}
            for binary in [executable, *libraries]:
                if not inside(binary, moved):
                    raise RuntimeError(f"Library symlink escapes package: {binary}")
                # dyld 可能让高于当前系统的二进制先跑到 --help，仍不能据此
                # 宣称支持当前系统。直接按每个 Mach-O 的真实最低要求拒绝。
                commands = checked(["/usr/bin/otool", "-l", str(binary)]).stdout
                minimum = minimum_macos(binary, commands)
                if macos_version_tuple(minimum) > host_version:
                    raise RuntimeError(f"Unsupported macOS version for {binary.relative_to(moved)}: "
                                       f"requires {minimum}, host is {host_macos}")
                minimum_versions[str(binary.relative_to(moved))] = minimum
                checked(["/usr/bin/codesign", "--verify", "--strict", str(binary)])
                # otool 第一行是文件名；dylib 的第一条引用还包含自身的 install ID。
                linked = checked(["/usr/bin/otool", "-L", str(binary)]).stdout
                for line in linked.splitlines()[1:]:
                    dependency = line.strip().split(" (compatibility version", 1)[0]
                    if system_library(dependency):
                        continue
                    if not dependency.startswith("@executable_path/"):
                        raise RuntimeError(f"External install name in {binary}: {dependency}")
                    target = executable.parent / dependency[len("@executable_path/"):]
                    if not target.is_file() or not inside(target, moved):
                        raise RuntimeError(f"Missing bundled dependency for {binary}: {dependency}")
                if "LC_RPATH" in commands:
                    raise RuntimeError(f"Unremoved runtime search path in {binary}")

            # 清除全部 dyld/locale 覆盖；PATH 只保留 macOS 自带工具。dyld日志用于
            # 检查所有实际加载的非系统库，尤其是 otool 无法发现的 SDL3 dlopen。
            environment = {key: value for key, value in os.environ.items()
                           if not key.startswith("DYLD_") and key not in
                           ("LANG", "LC_ALL", "LC_MESSAGES", "LANGUAGE", "SCRCTL_LOCALEDIR",
                            "SDL_DYNAMIC_API", "SDL3_DYNAMIC_API")}
            environment.update(PATH="/usr/bin:/bin:/usr/sbin:/sbin", DYLD_PRINT_LIBRARIES="1")
            cases = [([], {"LC_ALL": "C", "LANG": "zh_CN.UTF-8"}, False),
                     ([], {"LC_ALL": "zh_CN.UTF-8"}, True),
                     (["--lang", "auto"], {"LANG": "zh_CN.UTF-8"}, True),
                     (["--lang", "en"], {"LC_ALL": "zh_CN.UTF-8"}, False),
                     (["--lang", "zh-CN"], {"LC_ALL": "C"}, True)]
            all_loaded = set()
            for options, locale, chinese in cases:
                result = checked([str(executable), *options, "--help"],
                                 env={**environment, **locale}, cwd=temporary)
                expected = "iOS 屏幕镜像与控制" if chinese else "iOS screen mirroring and control"
                if expected not in result.stdout:
                    raise RuntimeError(f"Installed help language mismatch: {result.stdout}")
                loaded = re.findall(r"^dyld\[\d+\]: (?:<[^>]+> )?(/.+)$", result.stderr, re.M)
                if not loaded:
                    raise RuntimeError(f"dyld library trace unavailable: {result.stderr}")
                for item in loaded:
                    if not system_library(item) and not inside(Path(item), moved):
                        raise RuntimeError(f"Library loaded from outside relocated package: {item}")
                all_loaded.update(loaded)
            if (moved / "bin/lib/libSDL3.dylib").exists() and not any(
                    Path(item).name == "libSDL3.dylib" for item in all_loaded):
                raise RuntimeError("Bundled SDL3 was not loaded by SDL2 compatibility layer")
            version = checked([str(executable), "--version"],
                              env={**environment, "LC_ALL": "C"}, cwd=temporary)
            if not version.stdout.strip().startswith("scrctl "):
                raise RuntimeError(f"Unexpected installed version: {version.stdout}")
            package_minimum = max(minimum_versions.values(), key=macos_version_tuple)
            report = (f"Relocated macOS install: {len(libraries)} dylibs, minimum macOS "
                      f"{package_minimum} <= host {host_macos}, signatures, install names, "
                      "dyld paths, English/Chinese/auto and version passed\n" + version.stdout +
                      "\nMinimum macOS by file:\n" + "\n".join(
                          f"{path}: {minimum}" for path, minimum in sorted(minimum_versions.items())) +
                      "\nLoaded libraries:\n" + "\n".join(sorted(all_loaded)) + "\n")
            (install / "share/doc/scrctl/install-check.txt").write_text(report, encoding="utf-8")
            print(report.splitlines()[0])
        finally:
            if hidden.exists():
                hidden.rename(catalog)


if __name__ == "__main__":
    main()
