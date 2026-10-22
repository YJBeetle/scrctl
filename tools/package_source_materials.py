"""Bind collected sources and build records to one installed scrctl package."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def verify_record(record, root):
    relative = Path(record['path'])
    if relative.is_absolute() or '..' in relative.parts:
        raise RuntimeError('Unsafe source material path')
    path = root / relative
    if not path.is_file() or path.is_symlink() or sha256(path) != record['sha256']:
        raise RuntimeError(f'Source material changed or missing: {path}')


def verify_collectors(metadata, doc, collector, embedded, platform):
    manifest = json.loads((collector / 'source-manifest.json').read_text(encoding='utf-8'))
    if (manifest['source_commit'] != metadata['source_commit'] or
            manifest['package_metadata_sha256'] != sha256(doc / 'package-metadata.json')):
        raise RuntimeError('Collector describes another installed package')
    if platform == 'macos':
        packages = {item['name']: item for item in manifest['packages']}
        expected = metadata['homebrew_packages']
    else:
        packages = manifest['packages']
        expected = metadata['msys2_packages']
    if set(packages) != set(expected):
        raise RuntimeError('Collected package coverage differs from binary metadata')
    for name, item in packages.items():
        if item['version'] != expected[name]['version']:
            raise RuntimeError(f'Collected version differs: {name}')
        for record in item['artifacts' if platform == 'macos' else 'files']:
            verify_record(record, collector)
    manifest = json.loads((embedded / 'source-manifest.json').read_text(encoding='utf-8'))
    dependencies = manifest['dependencies']
    if not {'nlohmann_json', 'cli11', 'xdb'} <= {d['name'] for d in dependencies}:
        raise RuntimeError('Embedded dependency coverage incomplete')
    for item in dependencies:
        if not item['source_material_complete']:
            raise RuntimeError(f'Uncovered embedded dependency: {item["name"]}')
        verify_record({'path': item['source_archive'], 'sha256': item['source_archive_sha256']}, embedded)
        verify_record({'path': item['file_inventory'], 'sha256': item['file_inventory_sha256']}, embedded)


def package(install, build, materials, platform, repository):
    install, build, materials = map(lambda p: Path(p).resolve(strict=True),
                                     (install, build, materials))
    doc = install / 'share/doc/scrctl'
    (doc / 'source-materials.json').unlink(missing_ok=True)
    metadata = json.loads((doc / 'package-metadata.json').read_text(encoding='utf-8'))
    executable = install / ('bin/scrctl' if platform == 'macos' else 'bin/scrctl.exe')
    if sha256(executable) != metadata['executable_sha256']:
        raise RuntimeError('Installed executable changed since provenance capture')
    commit = metadata['source_commit']
    current = subprocess.check_output(['git', '-C', str(repository), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != current:
        raise RuntimeError('Installed package and project source commit differ')
    if subprocess.run(['git', '-C', str(repository), 'diff', '--quiet', 'HEAD']).returncode:
        changes = subprocess.check_output(['git', '-C', str(repository), 'diff', '--name-status',
                                           'HEAD'], text=True)
        raise RuntimeError('Tracked project files differ from recorded source commit:\n' + changes[:4000])
    # Both collectors must finish before anything can be labelled a source bundle.
    collector = materials / ('homebrew' if platform == 'macos' else 'msys2')
    for directory in (collector, materials / 'embedded'):
        if not (directory / 'source-manifest.json').is_file():
            raise RuntimeError(f'Missing completed source collector: {directory}')
    for dependency in metadata['runtime_dependencies']:
        owner = dependency.get('homebrew_package') if platform == 'macos' else dependency.get('msys2_package')
        embedded_runtime = (dependency.get('source_dependency') == 'xdb' and
                            dependency['library'].casefold() in ({'libxdb_client.dylib'} if platform == 'macos' else {'xdb_client.dll', 'libxdb_client.dll'}))
        if not owner and not embedded_runtime:
            raise RuntimeError(f'Uncovered runtime library: {dependency["library"]}')
        library = install / ('bin/lib' if platform == 'macos' else 'bin') / dependency['library']
        if sha256(library) != dependency['bundled_sha256']:
            raise RuntimeError(f'Installed runtime library changed: {dependency["library"]}')
    verify_collectors(metadata, doc, collector, materials / 'embedded', platform)
    project = materials / 'project'
    project.mkdir(exist_ok=True)
    with (project / 'scrctl-source.tar').open('wb') as stream:
        subprocess.run(['git', '-C', str(repository), 'archive', '--format=tar', commit],
                       stdout=stream, check=True)
    records = materials / 'build'
    records.mkdir(exist_ok=True)
    for name in ('package-metadata.json', 'runtime-origins.tsv', 'package-versions.txt',
                 'ffmpeg-build.txt', 'source-commit.txt'):
        shutil.copy2(doc / name, records / name)
    shutil.copy2(build / 'CMakeCache.txt', records / 'CMakeCache.txt')
    shutil.copy2(build / 'compile_commands.json', records / 'compile_commands.json')
    shutil.copy2(repository / '.github/workflows/offline-selftest.yml', records / 'offline-selftest.yml')
    shutil.copy2(repository / 'docs/SOURCE_MATERIALS.md', materials / 'README.md')
    files = []
    for path in sorted(materials.rglob('*')):
        if path.is_symlink():
            raise RuntimeError(f'Source bundle contains an unarchived symlink: {path}')
        if path.is_file() and path != materials / 'source-manifest.json':
            files.append({'path': path.relative_to(materials).as_posix(),
                          'size': path.stat().st_size, 'sha256': sha256(path)})
    manifest = {'schema_version': 1, 'platform': platform, 'source_commit': commit,
                'package_metadata_sha256': sha256(doc / 'package-metadata.json'), 'files': files}
    manifest_path = materials / 'source-manifest.json'
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    archive = materials.parent / f'scrctl-sources-{platform}.tar'
    temporary = archive.with_name(archive.name + '.partial')
    try:
        with tarfile.open(temporary, 'w') as stream:
            stream.add(materials, arcname=f'scrctl-sources-{platform}', recursive=True)
        temporary.replace(archive)
    finally:
        temporary.unlink(missing_ok=True)
    link = {'schema_version': 1, 'source_commit': commit, 'archive': archive.name,
            'archive_sha256': sha256(archive), 'manifest_sha256': sha256(manifest_path)}
    (doc / 'source-materials.json').write_text(json.dumps(link, indent=2) + '\n', encoding='utf-8')
    print(f'Created {archive}: {len(files)} material files; SHA256 {link["archive_sha256"]}')
    return link


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('install_directory', type=Path)
    parser.add_argument('build_directory', type=Path)
    parser.add_argument('materials_directory', type=Path)
    parser.add_argument('--platform', choices=['macos', 'windows'], required=True)
    args = parser.parse_args()
    package(args.install_directory, args.build_directory, args.materials_directory,
            args.platform, Path(__file__).resolve().parents[1])


if __name__ == '__main__':
    main()
