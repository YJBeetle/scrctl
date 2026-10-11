"""Offline source-delivery gates: exact versions, coverage, checksums, signatures."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import urllib.error


spec = importlib.util.spec_from_file_location("collect_windows_sources",
    Path(__file__).resolve().parents[1] / "tools/collect_windows_sources.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def source_archive(path, version="1.0", data=b"upstream", omit_source=False):
    checksum = hashlib.sha256(data).hexdigest()
    info = (f"pkgbase = mingw-w64-test\n\tpkgver = {version}\n\tpkgrel = 1\n"
            f"\tsource = test.tar.gz::https://example.invalid/source\n\tsha256sums = {checksum}\n"
            "pkgname = mingw-w64-ucrt-x86_64-test\n")
    entries = {".SRCINFO": info.encode(), "PKGBUILD": b"original recipe"}
    if not omit_source:
        entries["test.tar.gz"] = data
    with tarfile.open(path, "w:gz") as archive:
        for name, contents in entries.items():
            member = tarfile.TarInfo("mingw-w64-test/" + name)
            member.size = len(contents)
            archive.addfile(member, io.BytesIO(contents))


class ArchiveTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "source.tar.gz"
        source_archive(self.path)

    def test_complete_archive_has_exact_recipe_and_upstream_checksum(self):
        result = module.inspect_archive(self.path, "mingw-w64-test", "1.0-1",
                                       hashlib.sha256(b"original recipe").hexdigest())
        self.assertEqual(result["recipe_binding"], "binary-buildinfo-sha256")
        self.assertEqual(result["inputs"][0]["checksums"]["sha256"],
                         hashlib.sha256(b"upstream").hexdigest())

    def test_wrong_version_and_wrong_binary_recipe_fail(self):
        with self.assertRaisesRegex(RuntimeError, "base/version differs"):
            module.inspect_archive(self.path, "mingw-w64-test", "2.0-1")
        with self.assertRaisesRegex(RuntimeError, "BUILDINFO"):
            module.inspect_archive(self.path, "mingw-w64-test", "1.0-1", "0" * 64)

    def test_msys2_epoch_separator_keeps_exact_version(self):
        def reader(archive, member):
            data = module.archive_read(archive, member)
            if member.endswith(".SRCINFO"):
                data = data.replace(b"\tpkgver = 1.0", b"\tepoch = 2\n\tpkgver = 1.0")
            return data
        module.inspect_archive(self.path, "mingw-w64-test", "2~1.0-1", reader=reader)
        with self.assertRaisesRegex(RuntimeError, "base/version differs"):
            module.inspect_archive(self.path, "mingw-w64-test", "3~1.0-1", reader=reader)

    def test_source_omission_and_checksum_corruption_fail(self):
        source_archive(self.path, omit_source=True)
        with self.assertRaisesRegex(RuntimeError, "omits declared input"):
            module.inspect_archive(self.path, "mingw-w64-test", "1.0-1")
        source_archive(self.path)
        with self.assertRaisesRegex(RuntimeError, "checksum differs"):
            module.inspect_archive(self.path, "mingw-w64-test", "1.0-1",
                                   hash_reader=lambda *args: "0" * 64)

    def test_path_traversal_and_duplicate_members_fail(self):
        for name in ["../escape", "/absolute", "mingw-w64-test/PKGBUILD"]:
            source_archive(self.path)
            with tarfile.open(self.path, "r:gz") as archive:
                entries = [(member.name, archive.extractfile(member).read())
                           for member in archive.getmembers()]
            entries.append((name, b"bad"))
            with tarfile.open(self.path, "w:gz") as archive:
                for member_name, data in entries:
                    member = tarfile.TarInfo(member_name)
                    member.size = len(data)
                    archive.addfile(member, io.BytesIO(data))
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                module.archive_members(self.path)

    def test_native_windows_signature_verifier_requires_pacman_keyring_trust(self):
        calls = []
        valid = b"[GNUPG:] GOODSIG key signer\n[GNUPG:] VALIDSIG fingerprint\n[GNUPG:] TRUST_FULLY 0 pgp\n"
        def run(command):
            calls.append(command)
            if command[:2] == ["cygpath", "-w"]:
                return b"C:/msys64/usr/bin/gpg.exe\n"
            if command[0] == "cygpath":
                return ("/c/path with spaces/" + command[-1].split("/")[-1] + "\n").encode()
            if command[0] == "pacman-conf":
                return b"/etc/pacman.d/gnupg\n"
            return valid
        with patch.object(module.os, "name", "nt"), patch.object(module, "command_output", run):
            self.assertEqual(module.verify_signature("C:/source archive", "C:/source sig"), valid.decode())
        self.assertEqual(calls[-1][:3], ["C:/msys64/usr/bin/gpg.exe", "--homedir", "/etc/pacman.d/gnupg"])
        self.assertEqual(calls[-1][-3:], ["--verify", "/c/path with spaces/source sig", "/c/path with spaces/source archive"])
        for flag in ["--batch", "--lock-never", "--no-auto-check-trustdb", "--no-auto-key-retrieve"]:
            self.assertIn(flag, calls[-1])
        for status in [b"", valid.replace(b"TRUST_FULLY", b"TRUST_UNDEFINED"),
                       valid.replace(b"VALIDSIG", b"EXPSIG"), valid + b"[GNUPG:] REVKEYSIG key\n"]:
            with self.subTest(status=status), patch.object(module.os, "name", "nt"), patch.object(module, "command_output", run):
                valid = status
                with self.assertRaisesRegex(RuntimeError, "not fully trusted"):
                    module.verify_signature("C:/source archive", "C:/source sig")


class CollectionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.metadata = self.root / "metadata.json"
        packages = {name: {"package_base": "mingw-w64-test", "version": "1.0-1"}
                    for name in ("owner-one", "owner-two")}
        self.data = {"source_commit": "a" * 40, "msys2_packages": packages,
                     "runtime_dependencies": [{"msys2_package": owner} for owner in packages]}
        self.write_metadata()
        self.output = self.root / "sources"
        self.fetches = []

    def write_metadata(self):
        self.metadata.write_text(json.dumps(self.data))

    def fetch(self, url, path):
        self.fetches.append(url)
        if url.endswith(".sig"):
            path.write_bytes(b"detached signature")
        elif url.endswith(".gz"):
            source_archive(path)
        else:
            raise urllib.error.HTTPError(url, 404, "old compression", {}, None)

    def collect(self, verifier=lambda *args: "trusted test signer\n"):
        return module.collect(self.metadata, self.output, fetch=self.fetch, verifier=verifier)

    def test_every_dll_owner_covered_and_shared_archive_only_downloaded_once(self):
        result = self.collect()
        self.assertEqual(set(result["packages"]), {"owner-one", "owner-two"})
        self.assertEqual(sum(url.endswith(".gz") for url in self.fetches), 1)
        self.assertEqual(result["package_metadata_sha256"], module.digest(self.metadata))
        self.assertEqual(result["packages"]["owner-one"]["recipe_binding"], "exact-base-and-version")
        for material in result["packages"]["owner-one"]["files"]:
            self.assertEqual(material["sha256"], module.digest(self.output / material["path"]))

    def test_invalid_signature_does_not_leave_success_manifest(self):
        self.collect()
        def reject(*args):
            raise RuntimeError("untrusted signer")
        with self.assertRaisesRegex(RuntimeError, "untrusted"):
            self.collect(verifier=reject)
        self.assertFalse((self.output / "source-manifest.json").exists())

    def test_unknown_source_base_or_incomplete_owner_coverage_fail(self):
        del self.data["msys2_packages"]["owner-one"]["package_base"]
        self.write_metadata()
        with self.assertRaisesRegex(RuntimeError, "source package base"):
            self.collect()
        del self.data["msys2_packages"]["owner-one"]
        self.write_metadata()
        with self.assertRaisesRegex(RuntimeError, "coverage"):
            self.collect()

    def test_missing_historical_archive_never_falls_back_to_latest(self):
        def unavailable(url, path):
            raise urllib.error.HTTPError(url, 404, "gone", {}, None)
        with self.assertRaisesRegex(RuntimeError, "No exact official"):
            module.collect(self.metadata, self.output, fetch=unavailable)
        self.assertFalse((self.output / "source-manifest.json").exists())


if __name__ == "__main__":
    unittest.main()
