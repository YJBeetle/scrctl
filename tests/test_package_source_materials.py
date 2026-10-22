"""Publication gates: incomplete or mismatched materials must not produce a link."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('source_package', Path(__file__).parents[1] / 'tools/package_source_materials.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SourcePackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / 'repo'
        self.repo.mkdir()
        subprocess.run(['git', 'init', '-q', str(self.repo)], check=True)
        (self.repo / 'docs').mkdir()
        (self.repo / 'docs/SOURCE_MATERIALS.md').write_text('Build instructions')
        (self.repo / '.github/workflows').mkdir(parents=True)
        (self.repo / '.github/workflows/offline-selftest.yml').write_text('jobs: {}')
        subprocess.run(['git', '-C', str(self.repo), 'add', '.'], check=True)
        subprocess.run(['git', '-C', str(self.repo), '-c', 'user.name=Test', '-c',
                        'user.email=test@example.invalid', 'commit', '-qm', 'test'], check=True)
        self.commit = subprocess.check_output(['git', '-C', str(self.repo), 'rev-parse', 'HEAD'], text=True).strip()
        self.install = self.root / 'install'
        self.doc = self.install / 'share/doc/scrctl'
        self.doc.mkdir(parents=True)
        (self.install / 'bin').mkdir()
        (self.install / 'bin/test.dll').write_bytes(b'controlled library')
        (self.install / 'bin/scrctl.exe').write_bytes(b'controlled executable')
        self.metadata = {'source_commit': self.commit, 'executable_sha256': module.sha256(self.install / 'bin/scrctl.exe'), 'runtime_dependencies': [{'library': 'test.dll', 'msys2_package': 'test', 'bundled_sha256': module.sha256(self.install / 'bin/test.dll')}], 'msys2_packages': {'test': {'version': '1.0-1'}}}
        self.write_metadata()
        for name in ('runtime-origins.tsv', 'package-versions.txt', 'ffmpeg-build.txt', 'source-commit.txt'):
            (self.doc / name).write_text(name)
        self.build = self.root / 'build'
        self.build.mkdir()
        (self.build / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Release')
        (self.build / 'compile_commands.json').write_text('[]')
        self.materials = self.root / 'materials'
        for name in ('msys2', 'embedded'):
            directory = self.materials / name
            directory.mkdir(parents=True)
            (directory / 'source-manifest.json').write_text('{}')
        runtime = self.materials / 'msys2'
        (runtime / 'sources.tar').write_bytes(b'controlled source archive')
        manifest = {'source_commit': self.commit,
                    'package_metadata_sha256': module.sha256(self.doc / 'package-metadata.json'),
                    'packages': {'test': {'version': '1.0-1', 'files': [
                        {'path': 'sources.tar', 'sha256': module.sha256(runtime / 'sources.tar')} ]}}}
        (runtime / 'source-manifest.json').write_text(json.dumps(manifest))
        embedded = self.materials / 'embedded'
        dependencies = []
        for name in ('nlohmann_json', 'cli11', 'xdb'):
            (embedded / (name + '.tar')).write_bytes(b'controlled embedded source')
            (embedded / (name + '.json')).write_text('[]')
            dependencies.append({'name': name, 'source_material_complete': True,
                'source_archive': name + '.tar', 'source_archive_sha256': module.sha256(embedded / (name + '.tar')),
                'file_inventory': name + '.json', 'file_inventory_sha256': module.sha256(embedded / (name + '.json'))})
        (embedded / 'source-manifest.json').write_text(json.dumps({'dependencies': dependencies}))

    def write_metadata(self):
        (self.doc / 'package-metadata.json').write_text(json.dumps(self.metadata))

    def run_package(self):
        return module.package(self.install, self.build, self.materials, 'windows', self.repo)

    def test_archive_link_and_hashes(self):
        result = self.run_package()
        archive = self.root / result['archive']
        self.assertEqual(result['archive_sha256'], module.sha256(archive))
        manifest = json.loads((self.materials / 'source-manifest.json').read_text())
        for item in manifest['files']:
            self.assertEqual(item['sha256'], module.sha256(self.materials / item['path']))
        self.assertEqual(manifest['source_commit'], self.commit)

    def test_wrong_commit_rejected(self):
        self.metadata['source_commit'] = '0' * 40
        self.write_metadata()
        with self.assertRaisesRegex(RuntimeError, 'commit differ'):
            self.run_package()
        self.assertFalse((self.doc / 'source-materials.json').exists())

    def test_dirty_tracked_source_rejected(self):
        (self.repo / 'docs/SOURCE_MATERIALS.md').write_text('uncommitted source change')
        with self.assertRaisesRegex(RuntimeError, 'Tracked project files differ'):
            self.run_package()
        self.assertFalse((self.doc / 'source-materials.json').exists())

    def test_incomplete_collector_rejected(self):
        (self.materials / 'msys2/source-manifest.json').unlink()
        with self.assertRaisesRegex(RuntimeError, 'Missing completed'):
            self.run_package()

    def test_unknown_runtime_owner_rejected(self):
        self.metadata['runtime_dependencies'][0]['msys2_package'] = None
        self.write_metadata()
        with self.assertRaisesRegex(RuntimeError, 'Uncovered runtime'):
            self.run_package()

    def test_changed_installed_executable_rejected(self):
        (self.install / 'bin/scrctl.exe').write_bytes(b'changed after provenance')
        with self.assertRaisesRegex(RuntimeError, 'executable changed'):
            self.run_package()

    def test_changed_installed_library_rejected(self):
        (self.install / 'bin/test.dll').write_bytes(b'changed after collection')
        with self.assertRaisesRegex(RuntimeError, 'runtime library changed'):
            self.run_package()

    def test_missing_embedded_source_rejected(self):
        (self.materials / 'embedded/xdb.tar').unlink()
        with self.assertRaisesRegex(RuntimeError, 'changed or missing'):
            self.run_package()

    def test_wrong_collected_version_rejected(self):
        path = self.materials / 'msys2/source-manifest.json'
        manifest = json.loads(path.read_text())
        manifest['packages']['test']['version'] = '2.0-1'
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(RuntimeError, 'version differs'):
            self.run_package()

    def test_external_symlink_rejected(self):
        (self.materials / 'outside').symlink_to(self.repo)
        with self.assertRaisesRegex(RuntimeError, 'symlink'):
            self.run_package()


if __name__ == '__main__':
    unittest.main()
