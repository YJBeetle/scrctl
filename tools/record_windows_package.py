"""记录 MSYS2 包内 DLL 的原始文件与本地包元数据；不下载第三方源码。"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def checked_output(command):
    # pacman -Qi 是本地查询；固定其语言，保留原文而不推断 SPDX 表达式。
    environment = os.environ.copy()
    environment.update(LC_ALL="C", LANG="C")
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                            env=environment, timeout=15)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {command!r}: "
                           f"{result.stderr.strip()}")
    return result.stdout


def parse_package_info(raw):
    """解析英文 pacman -Qi 字段和续行，另保留未改写的原始输出。"""
    fields = {}
    current = None
    for line in raw.splitlines():
        if not line.strip():
            continue
        if line[0].isspace():
            if current is None:
                raise RuntimeError("Package metadata has a continuation without a field")
            fields[current] += "\n" + line.strip()
            continue
        match = re.fullmatch(r"([^:]+?)\s*:\s*(.*)", line)
        if not match:
            raise RuntimeError(f"Invalid package metadata field: {line!r}")
        current, value = match.group(1).strip(), match.group(2).strip()
        if current in fields:
            raise RuntimeError(f"Duplicate package metadata field: {current}")
        fields[current] = value
    if not fields.get("Name") or not fields.get("Version"):
        raise RuntimeError("Package metadata is missing Name or Version")
    return fields


class MsysPackages:
    def __init__(self, pacman="pacman", cygpath="cygpath", run=checked_output,
                 native_windows_paths=None, pacman_conf="pacman-conf"):
        self.pacman, self.cygpath, self.run = pacman, cygpath, run
        self.native_windows_paths = (os.name == "nt" if native_windows_paths is None
                                     else native_windows_paths)
        self.cache = {}
        self.pacman_conf = pacman_conf

    def source_identity(self, owner, version):
        database_path = self.run([self.pacman_conf, "DBPath"]).strip()
        if self.native_windows_paths:
            database_path = self.run([self.cygpath, "-w", database_path]).strip()
        directory_version = version.split(":")[-1] if self.native_windows_paths else version
        description = Path(database_path) / "local" / f"{owner}-{directory_version}" / "desc"
        raw = description.read_text(encoding="utf-8")
        fields = {}
        for block in raw.split("\n\n"):
            lines = block.strip().splitlines()
            if lines and re.fullmatch(r"%[A-Z]+%", lines[0]):
                fields[lines[0][1:-1]] = lines[1:]
        if fields.get("NAME") != [owner] or fields.get("VERSION") != [version]:
            raise RuntimeError(f"Local package database changed or mismatched: {owner}")
        base = fields.get("BASE", [])
        if len(base) != 1 or not re.fullmatch(r"[A-Za-z0-9@._+\-]+", base[0]):
            raise RuntimeError(f"Missing exact source package base for {owner}")
        result = {"package_base": base[0], "local_database_desc": raw}
        cache_directories = self.run([self.pacman_conf, "CacheDir"]).splitlines()
        for directory in cache_directories:
            if self.native_windows_paths:
                directory = self.run([self.cygpath, "-w", directory]).strip()
            archive_version = version.split(":")[-1]
            candidates = sorted(Path(directory).glob(f"{owner}-{archive_version}-*.pkg.tar.*"))
            for candidate in candidates:
                if candidate.name.endswith(".sig"):
                    continue
                buildinfo = self.run(["bsdtar", "-xOf", str(candidate), ".BUILDINFO"])
                values = {}
                for line in buildinfo.splitlines():
                    if " = " in line:
                        key, value = line.split(" = ", 1)
                        values.setdefault(key, []).append(value)
                if (values.get("pkgname") != [owner] or values.get("pkgbase") != base
                        or values.get("pkgver") != [version]):
                    raise RuntimeError(f"Cached build metadata mismatched: {owner}")
                checksum = values.get("pkgbuild_sha256sum", [])
                if len(checksum) != 1 or not re.fullmatch(r"[0-9a-f]{64}", checksum[0]):
                    raise RuntimeError(f"Cached build metadata has no recipe hash: {owner}")
                result.update(pkgbuild_sha256=checksum[0], buildinfo_raw=buildinfo,
                              cached_binary_path=str(candidate),
                              cached_binary_sha256=digest(candidate))
                return result
        return result

    def verify_cached_original(self, owner, original, original_sha256):
        metadata = self.cache[owner]
        archive = metadata.get("cached_binary_path")
        if archive is None:
            return
        member = str(original)
        if self.native_windows_paths:
            member = self.run([self.cygpath, "-u", member]).strip()
        member = member.lstrip("/")
        result = subprocess.run(["bsdtar", "-xOf", archive, member], capture_output=True, timeout=120)
        if result.returncode or hashlib.sha256(result.stdout).hexdigest() != original_sha256:
            raise RuntimeError(f"Cached binary package differs from installed DLL: {original.name}")

    def owner(self, path):
        # MinGW Python 的 Path 是 Windows 路径；pacman 属于 MSYS，显式转换边界。
        query_path = str(path)
        if self.native_windows_paths:
            query_path = self.run([self.cygpath, "-u", query_path]).strip()
            if not query_path.startswith("/") or "\n" in query_path:
                raise RuntimeError(f"Invalid MSYS path for {path}")
        owner = self.run([self.pacman, "-Qqo", query_path]).strip()
        if not re.fullmatch(r"[A-Za-z0-9@._+\-]+", owner):
            raise RuntimeError(f"Unknown or ambiguous package owner for {path}")
        return owner

    def metadata(self, owner):
        if owner not in self.cache:
            version_line = self.run([self.pacman, "-Q", owner]).split()
            if len(version_line) != 2 or version_line[0] != owner:
                raise RuntimeError(f"Invalid installed package version for {owner}")
            raw = self.run([self.pacman, "-Qi", owner])
            fields = parse_package_info(raw)
            if fields["Name"] != owner or fields["Version"] != version_line[1]:
                raise RuntimeError(f"Installed package metadata changed or mismatched: {owner}")
            self.cache[owner] = {"version": version_line[1], "fields": fields,
                                 "license_declaration_text": fields.get("Licenses"),
                                 "pacman_qi_raw": raw,
                                 **self.source_identity(owner, version_line[1])}
        return self.cache[owner]


def record_package(install_directory, runtime_directory, source_commit, database):
    root = Path(install_directory).resolve(strict=True)
    runtime = Path(runtime_directory).resolve(strict=True)
    executable = root / "bin/scrctl.exe"
    if not executable.is_file() or not runtime.is_dir():
        raise RuntimeError("An installed scrctl.exe and a toolchain runtime directory are required")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", source_commit):
        raise RuntimeError("A full 40-character source commit is required")
    doc = root / "share/doc/scrctl"
    commit_file = doc / "source-commit.txt"
    if commit_file.exists() and commit_file.read_text(encoding="utf-8").strip() != source_commit:
        raise RuntimeError("Installed source-commit.txt differs from the requested source")
    if runtime == executable.parent or root in runtime.parents:
        raise RuntimeError("The source runtime directory must be outside the install")

    sources = {}
    for path in runtime.iterdir():
        if path.is_file() and path.suffix.lower() == ".dll":
            name = path.name.casefold()
            if name in sources:
                raise RuntimeError(f"Ambiguous source DLL name: {path.name}")
            sources[name] = path
    bundled = sorted((path for path in executable.parent.iterdir()
                      if path.is_file() and path.suffix.lower() == ".dll"),
                     key=lambda path: path.name.casefold())
    if not bundled:
        raise RuntimeError("The install contains no runtime DLLs")
    packages, dependencies = {}, []
    seen = set()
    for path in bundled:
        name = path.name.casefold()
        if name in seen:
            raise RuntimeError(f"Ambiguous bundled DLL name: {path.name}")
        seen.add(name)
        original = sources.get(name)
        if original is None:
            raise RuntimeError(f"No toolchain source DLL for {path.name}")
        original_hash, bundled_hash = digest(original), digest(path)
        if original_hash != bundled_hash:
            raise RuntimeError(f"Source and bundled DLL SHA256 differ: {path.name}")
        owner = database.owner(original)
        if not re.fullmatch(r"[A-Za-z0-9@._+\-]+", owner):
            raise RuntimeError(f"Unknown package owner for {path.name}")
        metadata = database.metadata(owner)
        database.verify_cached_original(owner, original, original_hash)
        packages[owner] = metadata
        dependencies.append({"library": path.name, "original_path": str(original),
                             "bundled_path": path.relative_to(root).as_posix(),
                             "original_sha256": original_hash, "bundled_sha256": bundled_hash,
                             "msys2_package": owner, "package_version": metadata["version"]})

    metadata = {"source_commit": source_commit, "executable_sha256": digest(executable),
                "runtime_directory": str(runtime), "msys2_packages": packages,
                "runtime_dependencies": dependencies,
                "scope": "MSYS2 development CI package. Every installed runtime DLL matches "
                         "a same-name toolchain file by SHA256 and has a local pacman owner. "
                         "Package metadata and license declarations are provenance, not a "
                         "complete corresponding-source delivery or redistribution approval. "
                         "Third-party source, build recipes and patches are not downloaded."}
    # 全部 DLL 验证后再生成报告；未知来源不会得到部分成功清单。
    doc.mkdir(parents=True, exist_ok=True)
    with (doc / "runtime-origins.tsv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, delimiter="\t", lineterminator="\n",
                                fieldnames=list(dependencies[0]))
        writer.writeheader()
        writer.writerows(dependencies)
    (doc / "package-metadata.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("install_directory", type=Path)
    parser.add_argument("--runtime-directory", required=True, type=Path)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--pacman", default="pacman")
    parser.add_argument("--cygpath", default="cygpath")
    args = parser.parse_args()
    try:
        result = record_package(args.install_directory, args.runtime_directory,
                                args.source_commit, MsysPackages(args.pacman, args.cygpath))
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Windows package provenance failed: {error}", file=sys.stderr)
        return 1
    print(f"Recorded {len(result['runtime_dependencies'])} runtime DLLs from "
          f"{len(result['msys2_packages'])} installed MSYS2 packages")
    return 0


if __name__ == "__main__":
    sys.exit(main())
