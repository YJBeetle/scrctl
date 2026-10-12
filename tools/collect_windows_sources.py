"""Collect signed, exact-version MSYS2 source packages for every bundled DLL owner.

Uses MSYS2's complete source-only archives, retaining the upstream sources,
PKGBUILD and patches together. Does not execute downloaded build recipes.
Run in the MSYS2 CI environment with its initialized trusted pacman keyring.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request


SOURCE_REPOSITORY = "https://mirror.msys2.org/mingw/sources/"
TAR = "bsdtar" if os.name == "nt" else (shutil.which("bsdtar") or "tar")


def digest(path):
    checksum = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(chunk)
    return checksum.hexdigest()


def command_output(command):
    result = subprocess.run(command, capture_output=True, timeout=600)
    if result.returncode:
        raise RuntimeError(f"Command failed: {command!r}: "
                           f"{result.stderr.decode('utf-8', errors='replace')[-2000:]}")
    return result.stdout


def download(url, target):
    """Publish only fully downloaded files; HTTPS failures never disable TLS."""
    partial = target.with_name(target.name + ".partial")
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "scrctl-source-materials/1"})
        with urllib.request.urlopen(request, timeout=120) as source, partial.open("wb") as output:
            if urllib.parse.urlsplit(source.url).scheme != "https":
                raise RuntimeError("Source repository redirected outside HTTPS")
            expected = source.headers.get("Content-Length")
            total = 0
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                output.write(chunk)
                total += len(chunk)
            if expected is not None and total != int(expected):
                raise RuntimeError(f"Incomplete source download: {url}")
        partial.replace(target)
    finally:
        partial.unlink(missing_ok=True)


def verify_signature(archive, signature):
    # Use the same trusted keyring as binary package installation.
    paths = [str(signature), str(archive)]
    if os.name == "nt":
        paths = [command_output(["cygpath", "-u", path]).decode().strip() for path in paths]
        # MSYS2's pacman-key wrapper requires lock-never in gpg.conf even for
        # verification. Call its GnuPG directly with read-only verification flags,
        # keeping the original keyring and requiring the same full/ultimate trust.
        gpg = command_output(["cygpath", "-w", "/usr/bin/gpg.exe"]).decode().strip()
        keyring = command_output(["pacman-conf", "GPGDir"]).decode().strip()
        if not keyring or "\n" in keyring:
            raise RuntimeError("Invalid pacman GPGDir")
        command = [gpg, "--homedir", keyring, "--batch", "--lock-never",
                   "--no-auto-check-trustdb", "--no-auto-key-retrieve",
                   "--status-fd", "1", "--verify", *paths]
        result = command_output(command).decode("utf-8", errors="replace")
        statuses = {line.split()[1] for line in result.splitlines()
                    if line.startswith("[GNUPG:] ") and len(line.split()) >= 2}
        rejected = {"BADSIG", "ERRSIG", "EXPSIG", "EXPKEYSIG", "REVKEYSIG",
                    "NO_PUBKEY", "NODATA", "FAILURE", "TRUST_UNDEFINED",
                    "TRUST_NEVER", "TRUST_MARGINAL"}
        if (not {"GOODSIG", "VALIDSIG"}.issubset(statuses)
                or not statuses.intersection({"TRUST_FULLY", "TRUST_ULTIMATE"})
                or statuses.intersection(rejected)):
            raise RuntimeError("Source signature is invalid or not fully trusted by pacman keyring")
        return result
    else:
        command = ["pacman-key", "--verify", *paths]
    return command_output(command).decode("utf-8", errors="replace")


def archive_members(archive):
    entries = command_output([TAR, "-tf", str(archive)]).decode("utf-8").splitlines()
    seen = set()
    for entry in entries:
        path = PurePosixPath(entry)
        if path.is_absolute() or ".." in path.parts or "\\" in entry or not path.parts:
            raise RuntimeError(f"Unsafe source archive member: {entry!r}")
        normalized = path.as_posix().rstrip("/")
        if normalized in seen:
            raise RuntimeError(f"Duplicate source archive member: {normalized}")
        seen.add(normalized)
    return seen


def archive_read(archive, member):
    return command_output([TAR, "-xOf", str(archive), member])


def archive_digest(archive, member, algorithm):
    checksum = hashlib.new(algorithm)
    # Stream large upstream archives instead of retaining them in Python memory.
    with subprocess.Popen([TAR, "-xOf", str(archive), member], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE) as process:
        for chunk in iter(lambda: process.stdout.read(1024 * 1024), b""):
            checksum.update(chunk)
        error = process.stderr.read()
        if process.wait(timeout=600):
            raise RuntimeError(f"Cannot read source member {member}: {error.decode(errors='replace')}")
    return checksum.hexdigest()


def archive_git_digest(archive, member, source, algorithm):
    """Match makepkg's deterministic git archive checksum without running recipes."""
    fragment = urllib.parse.urlsplit(source.split("::", 1)[-1]).fragment
    kind, separator, ref = fragment.partition("=")
    if kind not in {"tag", "commit"} or not separator or not ref:
        raise RuntimeError(f"Git checksum requires a tag or commit: {source}")
    prefix = member + "/"
    with tempfile.TemporaryDirectory(prefix="scrctl-source-git-") as temporary:
        directory = Path(temporary)
        (directory / "objects").mkdir()
        (directory / "refs").mkdir()
        # Re-export through libarchive for all supported input compression formats.
        # Copy only regular Git object/ref files; never install archive links,
        # hooks, local configuration or external object-store references.
        with tempfile.TemporaryFile() as errors:
            process = subprocess.Popen([TAR, "-cf", "-", "--format=pax", "@" + str(archive)],
                                       stdout=subprocess.PIPE, stderr=errors)
            try:
                with tarfile.open(fileobj=process.stdout, mode="r|") as stream:
                    for entry in stream:
                        if not entry.name.startswith(prefix):
                            continue
                        relative = PurePosixPath(entry.name[len(prefix):])
                        if not relative.parts or ".." in relative.parts or relative.is_absolute():
                            raise RuntimeError("Unsafe Git source archive path")
                        allowed = (relative.as_posix() in {"HEAD", "packed-refs", "shallow"}
                                   or relative.parts[0] in {"objects", "refs"})
                        if not allowed or entry.isdir():
                            continue
                        if not entry.isfile() or relative.as_posix() == "objects/info/alternates":
                            raise RuntimeError("Git source contains a link or external object store")
                        target = directory.joinpath(*relative.parts)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with stream.extractfile(entry) as content, target.open("xb") as output:
                            shutil.copyfileobj(content, output)
                if process.wait(timeout=600):
                    raise RuntimeError("Cannot read Git source package")
            finally:
                process.stdout.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()
        environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
        environment.update(GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
        command = ["git", "-c", "core.abbrev=no", "-c", "core.attributesFile=" + os.devnull,
                   "--git-dir=" + str(directory)]
        resolved = subprocess.run(command + ["rev-parse", "--verify", "--end-of-options", ref + "^{commit}"],
                                  env=environment, capture_output=True, timeout=60)
        commit = resolved.stdout.decode().strip()
        if resolved.returncode or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit):
            raise RuntimeError(f"Git source lacks the declared tag or commit: {source}")
        checksum = hashlib.new(algorithm)
        with tempfile.TemporaryFile() as errors:
            with subprocess.Popen(command + ["archive", "--format=tar", commit], env=environment,
                                  stdout=subprocess.PIPE, stderr=errors) as process:
                for chunk in iter(lambda: process.stdout.read(1024 * 1024), b""):
                    checksum.update(chunk)
                if process.wait(timeout=600):
                    raise RuntimeError(f"Cannot export declared Git source: {source}")
        return checksum.hexdigest()


def parse_srcinfo(raw):
    fields = {}
    for line in raw.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if " = " not in line:
            raise RuntimeError("Malformed .SRCINFO line")
        key, value = line.split(" = ", 1)
        if key == "pkgname":
            break  # Split-package overrides do not redefine the base's source arrays.
        fields.setdefault(key, []).append(value)
    return fields


def source_filename(source):
    if "::" in source:
        filename = source.split("::", 1)[0]
    else:
        url = source.split("#", 1)[0]
        filename = urllib.parse.unquote(urllib.parse.urlsplit(url).path).rstrip("/").rsplit("/", 1)[-1]
        if source.startswith(("git+", "git://")) and filename.endswith(".git"):
            filename = filename[:-4]
    if not filename or PurePosixPath(filename).name != filename or "\\" in filename:
        raise RuntimeError(f"Unsafe source filename: {source!r}")
    return filename


def inspect_archive(archive, base, version, expected_recipe_hash=None, reader=archive_read,
                    members_reader=archive_members, hash_reader=archive_digest):
    members = members_reader(archive)
    prefix = base + "/"
    recipe, srcinfo = prefix + "PKGBUILD", prefix + ".SRCINFO"
    if recipe not in members or srcinfo not in members:
        raise RuntimeError("Source package is missing PKGBUILD or .SRCINFO")
    recipe_hash = hashlib.sha256(reader(archive, recipe)).hexdigest()
    if expected_recipe_hash and recipe_hash != expected_recipe_hash:
        raise RuntimeError("Source PKGBUILD differs from binary .BUILDINFO recipe hash")
    fields = parse_srcinfo(reader(archive, srcinfo).decode("utf-8"))
    try:
        actual = fields["pkgver"][0] + "-" + fields["pkgrel"][0]
        if fields.get("epoch", ["0"])[0] != "0":
            # MSYS2's makepkg patch replaces Arch's ':' epoch separator with '~'.
            separator = "~" if re.match(r"^[0-9]+~", version) else ":"
            actual = fields["epoch"][0] + separator + actual
    except (KeyError, IndexError) as error:
        raise RuntimeError("Source package is missing exact version metadata") from error
    if fields.get("pkgbase") != [base] or actual != version:
        raise RuntimeError(f"Source base/version differs: {fields.get('pkgbase')!r} {actual!r}")
    inputs = []
    algorithms = {"md5sums": "md5", "sha1sums": "sha1", "sha224sums": "sha224",
                  "sha256sums": "sha256", "sha384sums": "sha384", "sha512sums": "sha512",
                  "b2sums": "blake2b"}
    for source_key, sources in fields.items():
        if source_key != "source" and not source_key.startswith("source_"):
            continue
        suffix = source_key[len("source"):]
        checks = {algorithm: fields[key + suffix] for key, algorithm in algorithms.items()
                  if key + suffix in fields}
        if not checks or any(len(values) != len(sources) for values in checks.values()):
            raise RuntimeError(f"Source checksums missing or misaligned: {source_key}")
        for index, source in enumerate(sources):
            member = prefix + source_filename(source)
            file_present = member in members
            directory_present = any(item.startswith(member + "/") for item in members)
            if not file_present and not directory_present:
                raise RuntimeError(f"Source package omits declared input: {source}")
            verified = {}
            for algorithm, values in checks.items():
                expected = values[index]
                if expected == "SKIP":
                    continue
                if directory_present:
                    if not source.split("::", 1)[-1].startswith(("git+", "git://")):
                        raise RuntimeError(f"Unsupported checksum for VCS directory: {source}")
                    actual_hash = archive_git_digest(archive, member, source, algorithm)
                else:
                    actual_hash = hash_reader(archive, member, algorithm)
                if actual_hash != expected:
                    raise RuntimeError(f"Upstream source checksum differs: {source}")
                verified[algorithm] = actual_hash
            inputs.append({"source": source, "member": member, "checksums": verified,
                           "verification": "declared-checksum" if verified else "signed-source-archive"})
    if not inputs:
        raise RuntimeError("Source package contains no declared inputs")
    return {"pkgbuild_sha256": recipe_hash, "inputs": inputs,
            "recipe_binding": "binary-buildinfo-sha256" if expected_recipe_hash else "exact-base-and-version"}


def collect(metadata_path, output_directory, jobs=4, fetch=download, verifier=verify_signature,
            inspector=inspect_archive):
    metadata_path = Path(metadata_path)
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    packages = metadata.get("msys2_packages")
    if not packages or not re.fullmatch(r"[0-9a-fA-F]{40}", metadata.get("source_commit", "")):
        raise RuntimeError("Expected Windows package metadata with source commit and DLL owners")
    owners = {item["msys2_package"] for item in metadata.get("runtime_dependencies", [])}
    if owners != set(packages):
        raise RuntimeError("DLL owner coverage differs from recorded MSYS2 packages")
    groups = {}
    for owner, package in packages.items():
        base, version = package.get("package_base", ""), package.get("version", "")
        if not re.fullmatch(r"mingw-w64-[A-Za-z0-9@._+\-]+", base):
            raise RuntimeError(f"Missing exact MinGW source package base: {owner}")
        if not re.fullmatch(r"(?:[0-9]+:)?[A-Za-z0-9._+~\-]+", version):
            raise RuntimeError(f"Invalid installed package version: {owner}")
        groups.setdefault((base, version), []).append(owner)
    output = Path(output_directory)
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output / "source-manifest.json"
    # Never let an old success manifest survive a failed refresh.
    manifest_path.unlink(missing_ok=True)

    def one(group):
        (base, version), group_owners = group
        directory = output / f"{base}-{version.split(':')[-1]}"
        directory.mkdir(exist_ok=True)
        archive = None
        for extension in ("zst", "xz", "gz", "bz2"):
            filename = f"{base}-{version.split(':')[-1]}.src.tar.{extension}"
            candidate = directory / filename
            url = SOURCE_REPOSITORY + urllib.parse.quote(filename)
            try:
                if not candidate.exists():
                    fetch(url, candidate)
            except urllib.error.HTTPError as error:
                if error.code == 404:
                    error.close()
                    continue
                raise
            archive = candidate
            break
        if archive is None:
            raise RuntimeError(f"No exact official source archive for {base} {version}")
        signature = archive.with_name(archive.name + ".sig")
        if not signature.exists():
            fetch(url + ".sig", signature)
        verification = verifier(archive, signature)
        expected_hashes = {packages[owner]["pkgbuild_sha256"] for owner in group_owners
                           if packages[owner].get("pkgbuild_sha256")}
        if len(expected_hashes) > 1:
            raise RuntimeError(f"Conflicting binary recipe hashes for {base} {version}")
        contents = inspector(archive, base, version, next(iter(expected_hashes), None))
        verification_file = directory / "signature-verification.txt"
        verification_file.write_text(verification, encoding="utf-8")
        files = [{"path": file.relative_to(output).as_posix(), "sha256": digest(file)}
                 for file in (archive, signature, verification_file)]
        record = {"package_base": base, "version": version, "archive_url": url,
                  "signature_url": url + ".sig", "files": files, **contents}
        print(f"Collected {base} {version} ({len(group_owners)} bundled package owners)", flush=True)
        return group_owners, record

    records = {}
    with ThreadPoolExecutor(max_workers=jobs) as executor:
        for group_owners, record in executor.map(one, sorted(groups.items())):
            for owner in group_owners:
                records[owner] = record
    manifest = {"schema_version": 1, "platform": "windows", "source_commit": metadata["source_commit"],
                "package_metadata_sha256": digest(metadata_path), "packages": dict(sorted(records.items())),
                "scope": "Exact MSYS2 base/version source archives, authenticated with the CI pacman "
                         "keyring; bundled DLL hashes remain bound by package-metadata.json. "
                         "Cached binary BUILDINFO recipe hashes are compared when present. "
                         "This is source delivery, not a bit-for-bit rebuild claim.",
                "build": "Extract a source archive, enter its package directory in MSYS2 CLANGARM64, "
                         "install the PKGBUILD build dependencies, and run makepkg-mingw. "
                         "The captured package metadata and optional binary BUILDINFO record versions."}
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 16:
        parser.error("--jobs must be between 1 and 16")
    try:
        collect(args.metadata, args.output_directory, args.jobs)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Windows source collection failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
