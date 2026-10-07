"""记录包内 dylib 的实际来源、版本与随 keg 提供的许可资料；不下载或修改依赖。"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess


def output(command):
    return subprocess.check_output(command, text=True, encoding="utf-8").strip()


def digest(path):
    with path.open("rb") as stream:
        checksum = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
        return checksum.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("install_directory", type=Path)
    parser.add_argument("--source-commit", required=True)
    args = parser.parse_args()
    root = args.install_directory.resolve(strict=True)
    doc = root / "share/doc/scrctl"
    packages = {}
    dependencies = []
    with (doc / "runtime-origins.tsv").open(encoding="utf-8") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            original = Path(row["original_path"])
            bundled = root / "bin/lib" / row["library"]
            if not bundled.is_file():
                raise RuntimeError(f"Dependency missing from install: {bundled}")
            keg = next((parent for parent in original.parents
                        if (parent / "INSTALL_RECEIPT.json").is_file()), None)
            dependency = {"library": row["library"], "original_path": str(original),
                          "original_sha256": digest(original), "bundled_sha256": digest(bundled),
                          "architectures": output(["/usr/bin/lipo", "-archs", str(bundled)]).split()}
            if keg:
                name, version = keg.parent.name, keg.name
                dependency["homebrew_package"] = f"{name}/{version}"
                if name not in packages:
                    destination = doc / "dependency-licenses" / name
                    destination.mkdir(parents=True, exist_ok=True)
                    receipt = json.loads((keg / "INSTALL_RECEIPT.json").read_text())
                    shutil.copy2(keg / "INSTALL_RECEIPT.json", destination)
                    # 原始 formula 含完整的许可表达式、源码 URL 和校验值；不自制
                    # Ruby 解析器来推断 SPDX 组合。仅摘录声明行便于检索，原文件保留。
                    declarations = []
                    if (keg / ".brew").is_dir():
                        shutil.copytree(keg / ".brew", destination / "formula", dirs_exist_ok=True)
                        for formula in sorted((keg / ".brew").glob("*.rb")):
                            declarations.extend(re.findall(r"(?m)^\s*license[^\n]*",
                                                           formula.read_text(encoding="utf-8")))
                    documents = []
                    for entry in sorted(keg.iterdir()):
                        if entry.is_file() and entry.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "COPYRIGHT")):
                            shutil.copy2(entry, destination)
                            documents.append(entry.name)
                    if (keg / "share/licenses").is_dir():
                        shutil.copytree(keg / "share/licenses", destination / "share-licenses",
                                        dirs_exist_ok=True)
                        documents.append("share-licenses/")
                    packages[name] = {"version": version, "license_declaration_lines": declarations,
                                      "license_documents": documents, "source": receipt.get("source"),
                                      "receipt_architecture": receipt.get("arch")}
            else:
                dependency["homebrew_package"] = None
            dependencies.append(dependency)

    metadata = {"source_commit": args.source_commit, "host_macos": platform.mac_ver()[0],
                "architectures": output(["/usr/bin/lipo", "-archs", str(root / "bin/scrctl")]).split(),
                "signing": "ad hoc; not Developer ID signed or notarized",
                "scope": "Development CI package. System frameworks are supplied by macOS. "
                         "Bundled dependencies retain their own licenses. Formula/receipt/license "
                         "records do not replace corresponding source or redistribution obligations.",
                "homebrew_packages": packages, "runtime_dependencies": dependencies}
    (doc / "package-metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n",
                                               encoding="utf-8")
    (doc / "source-commit.txt").write_text(args.source_commit + "\n", encoding="utf-8")
    (doc / "package-versions.txt").write_text("\n".join(f"{name} {data['version']}"
        for name, data in sorted(packages.items())) + "\n", encoding="utf-8")
    print(f"Recorded {len(dependencies)} runtime libraries from {len(packages)} Homebrew packages")


if __name__ == "__main__":
    main()
