"""包来源离线回归：临时 DLL 字节与受控本地数据库，无设备或网络。"""
import csv
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "record_windows_package", Path(__file__).resolve().parents[1] / "tools/record_windows_package.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
COMMIT = "a" * 40
PACKAGE_INFO = ("Name            : mingw-test\n"
                "Version         : 2:1.2.3-4\n"
                "Description     : Test package\n"
                "URL             : https://example.invalid/source\n"
                "Licenses        : BSD-3-Clause\n"
                "Depends On      : dependency-one\n"
                "                  dependency-two\n")


class PackageDatabaseTest(unittest.TestCase):
    def test_metadata_continuations_and_exact_version(self):
        fields = module.parse_package_info(PACKAGE_INFO)
        self.assertEqual(fields["Name"], "mingw-test")
        self.assertEqual(fields["Version"], "2:1.2.3-4")
        self.assertEqual(fields["Depends On"], "dependency-one\ndependency-two")
        self.assertEqual(fields["Licenses"], "BSD-3-Clause")

    def test_malformed_metadata_is_rejected(self):
        for text in ["Name : pkg\n", "  continuation\n", "Name : pkg\nName : other\n",
                     "invalid line\n"]:
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                module.parse_package_info(text)

    def test_native_path_boundary_and_local_metadata_cache(self):
        calls = []

        def run(command):
            calls.append(command)
            if command[0] == "cygpath":
                return "/clangarm64/bin/Example.dll\n"
            if command[1] == "-Qqo":
                self.assertEqual(command[2], "/clangarm64/bin/Example.dll")
                return "mingw-test\n"
            return PACKAGE_INFO if command[1] == "-Qi" else "mingw-test 2:1.2.3-4\n"

        database = module.MsysPackages(run=run, native_windows_paths=True)
        self.assertEqual(database.owner(Path("C:/toolchain/bin/Example.dll")), "mingw-test")
        self.assertEqual(database.metadata("mingw-test")["version"], "2:1.2.3-4")
        self.assertEqual(database.metadata("mingw-test")["pacman_qi_raw"], PACKAGE_INFO)
        self.assertEqual(sum(command[1] == "-Qi" for command in calls), 1)

    def test_unknown_or_ambiguous_owner_is_rejected(self):
        for owner in ["", "first\nsecond\n"]:
            database = module.MsysPackages(run=lambda command: owner, native_windows_paths=False)
            with self.subTest(owner=owner), self.assertRaises(RuntimeError):
                database.owner(Path("Example.dll"))

    def test_version_disagreement_is_rejected(self):
        database = module.MsysPackages(
            run=lambda command: PACKAGE_INFO if command[1] == "-Qi" else "mingw-test 1.0-1\n")
        with self.assertRaisesRegex(RuntimeError, "mismatched"):
            database.metadata("mingw-test")


class DirectoryMappingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.install, self.runtime = self.base / "install with spaces", self.base / "toolchain"
        self.bin = self.install / "bin"
        self.bin.mkdir(parents=True)
        self.runtime.mkdir()
        (self.bin / "scrctl.exe").write_bytes(b"owned test executable")
        for name in ["Example.dll", "Other.DLL"]:
            (self.bin / name).write_bytes(name.encode())
            (self.runtime / name.lower()).write_bytes(name.encode())
        self.calls = []

        def run(command):
            self.calls.append(command)
            if command[1] == "-Qqo":
                return "mingw-test\n"
            return PACKAGE_INFO if command[1] == "-Qi" else "mingw-test 2:1.2.3-4\n"

        self.database = module.MsysPackages(run=run, native_windows_paths=False)

    def record(self):
        return module.record_package(self.install, self.runtime, COMMIT, self.database)

    def test_actual_directory_mapping_and_generated_reports(self):
        (self.bin / "unrelated.txt").write_text("not a dependency")
        result = self.record()
        self.assertEqual(len(result["runtime_dependencies"]), 2)
        self.assertEqual(list(result["msys2_packages"]), ["mingw-test"])
        self.assertEqual(result["source_commit"], COMMIT)
        self.assertIn("not a complete corresponding-source", result["scope"])
        for dependency in result["runtime_dependencies"]:
            self.assertEqual(Path(dependency["original_path"]).parent, self.runtime.resolve())
            self.assertEqual(dependency["package_version"], "2:1.2.3-4")
            self.assertEqual(dependency["original_sha256"], dependency["bundled_sha256"])
        doc = self.install / "share/doc/scrctl"
        self.assertEqual(json.loads((doc / "package-metadata.json").read_text()), result)
        with (doc / "runtime-origins.tsv").open(newline="") as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        self.assertEqual(rows, result["runtime_dependencies"])
        self.assertEqual(sum(command[1] == "-Qi" for command in self.calls), 1)

    def test_hash_mismatch_fails_before_database_or_reports(self):
        (self.runtime / "example.dll").write_bytes(b"wrong original")
        with self.assertRaisesRegex(RuntimeError, "SHA256 differ"):
            self.record()
        self.assertEqual(self.calls, [])
        self.assertFalse((self.install / "share/doc/scrctl/package-metadata.json").exists())

    def test_missing_source_fails(self):
        (self.runtime / "example.dll").unlink()
        with self.assertRaisesRegex(RuntimeError, "No toolchain source"):
            self.record()

    def test_unknown_owner_does_not_write_partial_reports(self):
        self.database.run = lambda command: ""
        with self.assertRaisesRegex(RuntimeError, "Unknown or ambiguous"):
            self.record()
        self.assertFalse((self.install / "share/doc/scrctl/runtime-origins.tsv").exists())

    def test_mismatched_source_commit_fails(self):
        doc = self.install / "share/doc/scrctl"
        doc.mkdir(parents=True)
        (doc / "source-commit.txt").write_text("b" * 40 + "\n")
        with self.assertRaisesRegex(RuntimeError, "source-commit.txt differs"):
            self.record()


if __name__ == "__main__":
    unittest.main()
