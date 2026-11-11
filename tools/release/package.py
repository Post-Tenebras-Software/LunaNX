#!/usr/bin/env python3
"""Package an audited local release. Never publishes or overwrites a release."""
import argparse
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

from check import ROOT, BUILD, AUDIT, VERSION, digest, require, validate

ROOTS = ('CMakeLists.txt', 'README.md', 'LICENSE', 'COPYRIGHT', 'THIRD_PARTY_NOTICES.md',
         'source', 'config', 'assets', 'tests', 'tools', 'external', 'third_party',
         'docs', 'distribution', '.github', '.gitignore', '.gitattributes',
         'SECURITY.md', 'CONTRIBUTING.md', 'PRODUCTION_BASELINE.json')
EXCLUDED = {'.git', '.aws', '.agents', '.codex', '__pycache__', 'node_modules', '.pytest_cache'}
SECRET = re.compile(rb'Atza\|[A-Za-z0-9_\-]{100,}|eyJ[A-Za-z0-9_\-]{100,}\.[A-Za-z0-9_\-]{100,}\.[A-Za-z0-9_\-]{100,}')


def allowed(path):
    parts = path.parts
    return (not any(p in EXCLUDED for p in parts)
            and parts[:3] != ('tools', 'luna-inspect', 'bundles')
            and path.suffix not in {'.sdp', '.har', '.pyc', '.o', '.a'}
            and not path.name.startswith('.answer-')
            and path.name not in {'login.bin', 'login.bin.tmp', 'recent-games.json', 'CMakeCache.txt'})


def source_files():
    for name in ROOTS:
        root = ROOT / name
        if root.is_file():
            yield root
            continue
        for directory, dirs, files in os.walk(root):
            dirs[:] = sorted(d for d in dirs if allowed((Path(directory) / d).relative_to(ROOT)))
            for name in sorted(files):
                path = Path(directory) / name
                if allowed(path.relative_to(ROOT)):
                    require(not path.is_symlink(), 'Review source symlink before distribution: ' + str(path))
                    if path.suffix in {'.cpp', '.hpp', '.h', '.c', '.py', '.js', '.cjs', '.mjs', '.json', '.md', '.txt'}:
                        require(not SECRET.search(path.read_bytes()), 'Potential credential in source file: ' + str(path.relative_to(ROOT)))
                    yield path


def add_bytes(archive, name, data):
    member = tarfile.TarInfo(name)
    member.size = len(data)
    member.mode = 0o644
    archive.addfile(member, io.BytesIO(data))


def source_archive(destination, info):
    files = list(source_files())
    manifest = ''.join(f'{digest(p)}  {p.relative_to(ROOT).as_posix()}\n' for p in files)
    with tarfile.open(destination, 'w:gz') as archive:
        for path in files:
            member = archive.gettarinfo(str(path), arcname=path.relative_to(ROOT).as_posix())
            member.uid = member.gid = 0
            member.uname = member.gname = ''
            with path.open('rb') as stream:
                archive.addfile(member, stream)
        add_bytes(archive, 'BUILDINFO.json', (json.dumps(info, indent=2) + '\n').encode())
        add_bytes(archive, 'SOURCE_MANIFEST.sha256', manifest.encode())
    with tarfile.open(destination) as archive:
        members = archive.getmembers()
        require(all(allowed(Path(m.name)) and not m.issym() and not m.islnk() for m in members), 'Source archive contains excluded content')
        require(any(m.name == 'tools/release/build.sh' for m in members), 'Missing rebuild script')
    return len(files)


def build_info(validation):
    packages = subprocess.check_output(['pacman', '-Q'], text=True).splitlines()
    return {**validation, 'release_date': '2026-11-10', 'author': 'PostTenebrasSoftware',
            'original_license': 'GPL-3.0-or-later', 'packages': packages,
            'compiler': subprocess.check_output(['/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++', '--version'], text=True).splitlines()[0],
            'optimization': '-O2 -DNDEBUG; compiler debug symbols retained separately',
            'source_rebuild': 'bash tools/release/build.sh',
            'baseline': 'Switch-tested 1.0.1-id2 runtime; separate production-only source tree',
            'release_device_test': validation['hardware_validation'],
            'clean_source_rebuild': json.loads((AUDIT / 'clean-rebuild.json').read_text())
                if (AUDIT / 'clean-rebuild.json').exists() else {'status': 'not recorded'},
            'hardware_test_history': json.loads((ROOT / 'tools/release/hardware-tests.json').read_text()),
            'build_changes': ['Explicit game EULA retrieval, readable links, acceptance preference and launch continuation',
                              'Switch-compatible Recently Played replacement, rollback and restart recovery',
                              'Preserve catalog covers in Recently Played and repair older artwork without changing play order',
                              'Explicit COMPLETED_FAILED job diagnostics and user guidance',
                              'Production 1.0.1 identity, original icon and store/repository packaging',
                              'Profile-bound client identity and confirmed remote session controls'],
            'tests': json.loads((AUDIT / 'tests.json').read_text())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-only', type=Path, help='Write a source snapshot for a clean rebuild check')
    args = parser.parse_args()
    validation = validate()
    info = build_info(validation)
    if args.source_only:
        require(not args.source_only.exists(), 'Refusing to overwrite source snapshot')
        print('Source files:', source_archive(args.source_only, info))
        return
    release_root = ROOT / 'releases'
    release_root.mkdir(exist_ok=True)
    destination = release_root / ('v' + VERSION)
    require(not destination.exists(), 'Release already exists; refusing to overwrite')
    with tempfile.TemporaryDirectory(prefix='.release-package-', dir=release_root) as temporary:
        staging = Path(temporary) / 'release'
        staging.mkdir()
        shutil.copy2(BUILD / 'LunaNX.nro', staging / 'LunaNX.nro')
        shutil.copy2(ROOT / 'tools/release/RELEASE_NOTES.md', staging / 'RELEASE_NOTES.md')
        (staging / 'BUILDINFO.json').write_text(json.dumps(info, indent=2) + '\n')
        shutil.copytree(AUDIT, staging / 'validation')
        with zipfile.ZipFile(staging / f'LunaNX-v{VERSION}-switch.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            archive.write(staging / 'LunaNX.nro', 'switch/LunaNX/LunaNX.nro')
            for name in ('LICENSE', 'COPYRIGHT', 'THIRD_PARTY_NOTICES.md'):
                archive.write(ROOT / name, 'LunaNX-docs/' + name)
            archive.write(staging / 'RELEASE_NOTES.md', 'LunaNX-docs/README.md')
            archive.write(staging / 'BUILDINFO.json', 'LunaNX-docs/BUILDINFO.json')
            for path in sorted((ROOT / 'third_party/licenses').rglob('*')):
                if path.is_file(): archive.write(path, 'LunaNX-docs/' + path.relative_to(ROOT).as_posix())
        source_archive(staging / f'LunaNX-v{VERSION}-source.tar.gz', info)
        with tarfile.open(staging / f'LunaNX-v{VERSION}-symbols.tar.gz', 'w:gz') as archive:
            archive.add(BUILD / 'LunaNX.elf', arcname='LunaNX.elf')
            archive.add(BUILD / 'LunaNX.map', arcname='LunaNX.map')
            archive.add(staging / 'BUILDINFO.json', arcname='BUILDINFO.json')
        with zipfile.ZipFile(staging / f'LunaNX-v{VERSION}-switch.zip') as archive:
            require(archive.testzip() is None, 'Corrupt install ZIP')
            require(archive.read('switch/LunaNX/LunaNX.nro') == (staging / 'LunaNX.nro').read_bytes(), 'Packaged NRO mismatch')
            require(all(n.startswith(('switch/LunaNX/LunaNX.nro', 'LunaNX-docs/')) and '..' not in Path(n).parts for n in archive.namelist()), 'Unexpected install path')
        files = sorted(p for p in staging.iterdir() if p.is_file())
        (staging / 'SHA256SUMS').write_text(''.join(f'{digest(p)}  {p.relative_to(staging).as_posix()}\n' for p in files))
        staging.rename(destination)
    print('Packaged:', destination)
    print('Production release packaged locally. Nothing was published.')


if __name__ == '__main__':
    main()
