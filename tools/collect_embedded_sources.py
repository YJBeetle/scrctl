#!/usr/bin/env python3
"""Export the actual source trees selected by a configured scrctl build.

Usage: collect_embedded_sources.py BUILD_DIRECTORY OUTPUT_DIRECTORY
No downloads, inferred installed-package versions, or execution of recipes.
System dependencies remain explicit and require the platform source collector.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import tempfile


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def archive_tree(source, destination, name):
    """Preserve source bytes and internal links, without .git or host metadata."""
    source = source.resolve(strict=True)
    if not source.is_dir():
        raise RuntimeError(f"source directory is not a directory: {source}")
    if destination.resolve().is_relative_to(source):
        raise RuntimeError("output must not be inside a dependency source directory")
    inventory = []
    # Explicit traversal allows validating links before tarfile sees any content.
    paths = [source]
    pending = [source]
    while pending:
        directory = pending.pop()
        for path in sorted(directory.iterdir()):
            if path.name in (".git", "__pycache__"):
                continue
            paths.append(path)
            if path.is_dir() and not path.is_symlink():
                pending.append(path)
    paths.sort(key=lambda path: path.relative_to(source).as_posix())
    for path in paths:
        relative = path.relative_to(source).as_posix()
        if path.is_symlink():
            try:
                target = path.readlink()
                resolved = path.resolve(strict=True)
            except (OSError, RuntimeError) as error:
                # Invalid Windows link syntax, dangling links and cycles must
                # all fail the export, with the same boundary error everywhere.
                raise RuntimeError(f"source link cannot be resolved safely: {path}") from error
            # Absolute symlinks cannot be relocated to a recipient's checkout.
            if target.is_absolute() or not resolved.is_relative_to(source):
                raise RuntimeError(f"source link escapes dependency tree: {path}")
            inventory.append({"path": relative, "symlink": str(target)})
        elif path.is_file():
            inventory.append({"path": relative, "sha256": sha256(path)})
        elif not path.is_dir():
            raise RuntimeError(f"unsupported source entry: {path}")
    with tarfile.open(destination, "w", format=tarfile.PAX_FORMAT) as archive:
        for path in paths:
            relative = path.relative_to(source)
            info = archive.gettarinfo(str(path), arcname=str(Path(name) / relative))
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = 0
            if info.isfile():
                with path.open("rb") as stream:
                    archive.addfile(info, stream)
            else:
                archive.addfile(info)
    return inventory


def collect(build_directory, output_directory):
    build_directory = Path(build_directory).resolve(strict=True)
    output_directory = Path(output_directory).resolve()
    manifest_path = build_directory / "source-materials-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("dependencies"), list):
        raise RuntimeError("unsupported or malformed CMake source materials manifest")
    if output_directory.exists() and any(output_directory.iterdir()):
        raise RuntimeError(f"output directory is not empty: {output_directory}")
    output_directory.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".embedded-sources-", dir=output_directory.parent))
    try:
        dependencies = []
        names = set()
        for item in manifest["dependencies"]:
            name = item.get("name", "")
            if not re.fullmatch(r"[a-z0-9_]+", name) or name in names:
                raise RuntimeError(f"invalid or repeated dependency name: {name!r}")
            names.add(name)
            entry = dict(item)
            if item.get("selection") == "source-tree":
                source = Path(item["source_directory"])
                if not source.is_absolute():
                    raise RuntimeError(f"CMake source directory must be absolute: {source}")
                archive = temporary / f"{name}-source.tar"
                inventory = archive_tree(source, archive, name)
                inventory_path = temporary / f"{name}-files.json"
                inventory_path.write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
                entry.update({"source_archive": archive.name,
                              "source_archive_sha256": sha256(archive),
                              "file_inventory": inventory_path.name,
                              "file_inventory_sha256": sha256(inventory_path),
                              "source_material_complete": True})
            elif item.get("selection") == "system-package":
                entry["source_material_complete"] = False
                entry["required_collector"] = "platform-package-sources"
            else:
                raise RuntimeError(f"unsupported dependency selection: {item.get('selection')!r}")
            dependencies.append(entry)
        # Avoid publishing a successful empty collection from a missing manifest.
        if not dependencies:
            raise RuntimeError("CMake source materials manifest has no dependencies")
        manifest["dependencies"] = dependencies
        manifest["scope"] = ("Exact configured dependency source trees, including local overrides. "
                             "Declared URLs/refs describe CMake defaults, not the identity of "
                             "an override or a system package. System-package source delivery "
                             "must be completed separately.")
        (temporary / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        overrides = {f"FETCHCONTENT_SOURCE_DIR_{item['name'].upper()}": item["name"]
                     for item in dependencies if item["selection"] == "source-tree"}
        (temporary / "rebuild-overrides.json").write_text(json.dumps(overrides, indent=2) + "\n", encoding="utf-8")
        (temporary / "README.txt").write_text(
            "Extract each NAME-source.tar into the same directory.\n"
            "Configure the delivered project with -DSCRCTL_FETCH_DEPENDENCIES=OFF and\n"
            "the -DFETCHCONTENT_SOURCE_DIR_NAME=/absolute/path/to/NAME entries listed\n"
            "in rebuild-overrides.json. Preserve the original CMake build options;\n"
            "system packages and the compiler are supplied by the platform build recipe.\n"
            "To force the exported JSON/CLI11/xdb trees rather than installed packages,\n"
            "also set -DCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=ON,\n"
            "-DCMAKE_DISABLE_FIND_PACKAGE_CLI11=ON; xdb is the pinned source submodule.\n",
            encoding="utf-8")
        if output_directory.exists():
            output_directory.rmdir()
        temporary.rename(output_directory)
        return manifest
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_directory", type=Path)
    parser.add_argument("output_directory", type=Path)
    args = parser.parse_args()
    collect(args.build_directory, args.output_directory)


if __name__ == "__main__":
    main()
