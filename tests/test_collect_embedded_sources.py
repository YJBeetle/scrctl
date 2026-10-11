"""Source export boundary tests; no device, network, or dependency build."""

import importlib.util
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    "collect_embedded_sources", Path(__file__).resolve().parents[1] / "tools/collect_embedded_sources.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SourceExportTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.source = self.root / "actual override"
        self.source.mkdir()
        (self.source / "LICENSE").write_bytes(b"source license\n")
        (self.source / "library.h").write_bytes(b"actual local modified bytes\x00\xff")
        self.output = self.root / "delivery"

    def manifest(self, entries=None):
        if entries is None:
            entries = [{"name": "library", "selection": "source-tree",
                        "source_directory": str(self.source), "source_override": True,
                        "declared_ref": "a default ref that is not the actual override"}]
        (self.build / "source-materials-manifest.json").write_text(
            json.dumps({"schema_version": 1, "dependencies": entries}), encoding="utf-8")

    def test_exports_actual_bytes_and_license_with_inventory(self):
        self.manifest()
        manifest = module.collect(self.build, self.output)
        entry = manifest["dependencies"][0]
        with tarfile.open(self.output / entry["source_archive"]) as archive:
            self.assertEqual(archive.extractfile("library/library.h").read(),
                             (self.source / "library.h").read_bytes())
            self.assertEqual(archive.extractfile("library/LICENSE").read(), b"source license\n")
        inventory = json.loads((self.output / entry["file_inventory"]).read_text())
        item = next(item for item in inventory if item["path"] == "library.h")
        self.assertEqual(item["sha256"], module.sha256(self.source / "library.h"))
        self.assertTrue(entry["source_override"])
        self.assertEqual(entry["source_archive_sha256"],
                         module.sha256(self.output / entry["source_archive"]))

    def test_reproducible_archive_ignores_git_and_timestamps(self):
        self.manifest()
        (self.source / ".git").mkdir()
        (self.source / ".git" / "secret config").write_text("ignored")
        module.collect(self.build, self.output)
        second = self.root / "second"
        (self.source / "library.h").touch()
        module.collect(self.build, second)
        self.assertEqual((self.output / "library-source.tar").read_bytes(),
                         (second / "library-source.tar").read_bytes())
        with tarfile.open(second / "library-source.tar") as archive:
            self.assertFalse(any(".git" in name for name in archive.getnames()))

    def test_internal_links_are_preserved_external_links_are_rejected(self):
        self.manifest()
        link = self.source / "alias.h"
        try:
            link.symlink_to("library.h")
        except OSError:
            self.skipTest("creating symlinks requires platform privilege")
        module.collect(self.build, self.output)
        with tarfile.open(self.output / "library-source.tar") as archive:
            self.assertTrue(archive.getmember("library/alias.h").issym())
        link.unlink()
        external = self.root / "private file"
        external.write_text("not source")
        link.symlink_to(Path("..") / "private file")
        rejected = self.root / "rejected"
        with self.assertRaisesRegex(RuntimeError, "escapes|cannot be resolved safely"):
            module.collect(self.build, rejected)
        self.assertFalse(rejected.exists())

    def test_broken_and_invalid_links_fail_without_delivery(self):
        self.manifest()
        link = self.source / "broken.h"
        try:
            link.symlink_to("missing.h")
        except OSError:
            self.skipTest("creating symlinks requires platform privilege")
        with self.assertRaisesRegex(RuntimeError, "cannot be resolved safely"):
            module.collect(self.build, self.output)
        self.assertFalse(self.output.exists())
        original_resolve = Path.resolve

        def invalid_link_resolve(path, *args, **kwargs):
            if path == link:
                raise OSError(123, "Invalid Windows link syntax")
            return original_resolve(path, *args, **kwargs)

        with mock.patch.object(Path, "resolve", invalid_link_resolve):
            with self.assertRaisesRegex(RuntimeError, "cannot be resolved safely"):
                module.collect(self.build, self.output)
        self.assertFalse(self.output.exists())

    def test_system_packages_are_not_reported_as_exported(self):
        self.manifest([{"name": "system_header", "selection": "system-package",
                        "detected_version": "", "declared_ref": "fixed fallback ref",
                        "INTERFACE_INCLUDE_DIRECTORIES": ["/toolchain/include"]}])
        manifest = module.collect(self.build, self.output)
        entry = manifest["dependencies"][0]
        self.assertFalse(entry["source_material_complete"])
        self.assertNotIn("source_archive", entry)
        self.assertEqual(entry["detected_version"], "")

    def test_missing_source_and_invalid_name_leave_no_delivery(self):
        for name, directory in [("library", self.root / "missing"), ("../escape", self.source)]:
            self.manifest([{"name": name, "selection": "source-tree",
                            "source_directory": str(directory)}])
            with self.subTest(name=name), self.assertRaises((RuntimeError, FileNotFoundError)):
                module.collect(self.build, self.output)
            self.assertFalse(self.output.exists())

    def test_refuses_to_include_output_in_source_or_overwrite_delivery(self):
        self.manifest()
        with self.assertRaisesRegex(RuntimeError, "inside"):
            module.collect(self.build, self.source / "delivery")
        self.output.mkdir()
        (self.output / "important").write_text("preserve")
        with self.assertRaisesRegex(RuntimeError, "not empty"):
            module.collect(self.build, self.output)
        self.assertEqual((self.output / "important").read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
