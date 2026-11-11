#!/usr/bin/env python3
"""Validate the production release; optionally run synthetic regression suites."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build-release'
AUDIT = BUILD / 'release-audit'
VERSION = '1.0.1'
TESTS = [
    ['python3', 'tests/luna-auth/run.py'], ['python3', 'tests/luna-auth/memory.py'],
    ['python3', 'tests/recent-games/run.py'], ['python3', 'tests/frame-pacer/run.py'],
    ['python3', 'tests/h264-decoder/run.py'], ['python3', 'tests/video-view/run.py'],
    ['python3', 'tests/video-mode/run.py'], ['python3', 'tests/ice-address/run.py'],
    ['python3', 'tests/trailers/run.py'],
    ['python3', 'tools/launcher-preview/run.py'], ['node', 'tests/luna-session/run.cjs'],
    ['node', 'tests/luna-control/run.cjs'], ['node', 'tests/luna-input/run.cjs'],
]


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run_tests():
    AUDIT.mkdir(parents=True, exist_ok=True)
    def run(command):
        start = time.monotonic()
        result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        log = command[1].replace('/', '_') + '.log'
        (AUDIT / log).write_text(result.stdout)
        record = {'command': command, 'exit_code': result.returncode,
                  'seconds': round(time.monotonic() - start, 2), 'log': log}
        print(('PASS ' if result.returncode == 0 else 'FAIL ') + ' '.join(command), flush=True)
        return record
    with ThreadPoolExecutor(max_workers=3) as pool:
        records = list(pool.map(run, TESTS))
    (AUDIT / 'tests.json').write_text(json.dumps(records, indent=2) + '\n')
    require(all(r['exit_code'] == 0 for r in records), 'Regression failure; inspect release-audit logs')


def validate():
    cache = (BUILD / 'CMakeCache.txt').read_text()
    require('CMAKE_BUILD_TYPE:STRING=Release\n' in cache, 'Expected Release build')
    commands = json.loads((BUILD / 'compile_commands.json').read_text())
    launcher = [c for c in commands if 'LunaNX.dir' in c['command']]
    require(bool(launcher), 'Launcher compile commands missing')
    require(all('-O2' in c['command'] and '-DNDEBUG' in c['command'] and
                VERSION in c['command'] for c in launcher), 'Wrong optimization or identity')
    nacp = (BUILD / 'LunaNX.nacp').read_bytes()
    string = lambda offset, length: nacp[offset:offset + length].split(b'\0', 1)[0].decode()
    require(len(nacp) == 0x4000, 'Invalid NACP size')
    require(string(0, 0x200) == 'LunaNX', 'Wrong application name')
    require(string(0x200, 0x100) == 'PostTenebrasSoftware', 'Wrong author')
    require(string(0x3060, 16) == VERSION, 'Wrong embedded version')
    nro = (BUILD / 'LunaNX.nro').read_bytes()
    require(nro[0x10:0x14] == b'NRO0', 'Invalid NRO header')
    asset = struct.unpack_from('<I', nro, 0x18)[0]
    require(nro[asset:asset + 4] == b'ASET', 'Missing NRO assets')
    icon_offset, icon_size = struct.unpack_from('<QQ', nro, asset + 0x8)
    require(nro[asset + icon_offset:asset + icon_offset + icon_size] ==
            (ROOT / 'assets/branding/icon.jpg').read_bytes(), 'Missing/custom icon mismatch')
    offset, size = struct.unpack_from('<QQ', nro, asset + 0x18)
    require(nro[asset + offset:asset + offset + size] == nacp, 'Embedded NACP mismatch')
    romfs, romfs_size = struct.unpack_from('<QQ', nro, asset + 0x28)
    require(romfs_size > 0 and asset + romfs + romfs_size <= len(nro), 'Missing/truncated RomFS')
    require(VERSION.encode() in nro and b'1.0.1\0' in nro, 'Missing runtime identity')
    require(b'1.0.0-rc3' not in nro, 'Old candidate identity remains in NRO')
    require((BUILD / 'LunaNX.map').stat().st_size > 0, 'Missing link map')
    for source in json.loads((ROOT / 'third_party/sources.json').read_text()):
        require(source['verified'] and digest(ROOT / source['file']) == source['sha256'],
                'Dependency source missing or modified: ' + source['file'])
    for recipe in json.loads((ROOT / 'third_party/recipes.json').read_text()):
        require(digest(ROOT / recipe['file']) == recipe['sha256'], 'Dependency recipe modified: ' + recipe['file'])
    records = json.loads((AUDIT / 'tests.json').read_text())
    require(sorted(r['command'] for r in records) == sorted(TESTS), 'Test inventory mismatch')
    require(all(r['exit_code'] == 0 and (AUDIT / r['log']).is_file() for r in records), 'Tests did not all pass')
    inputs = [ROOT / 'CMakeLists.txt']
    for directory in ('source', 'config', 'tests', 'tools/launcher-preview'):
        inputs.extend(p for p in (ROOT / directory).rglob('*') if p.is_file() and
                      p.suffix in {'.cpp', '.hpp', '.h', '.c', '.py', '.js', '.cjs', '.mjs', '.json'})
    require(min((AUDIT / r['log']).stat().st_mtime for r in records) >=
            max(p.stat().st_mtime for p in inputs), 'Source/test inputs changed after regression checks; run --tests')
    nro_hash = digest(BUILD / 'LunaNX.nro')
    hardware = json.loads((ROOT / 'tools/release/hardware-tests.json').read_text())
    tested = any(r['nro_sha256'] == nro_hash for r in hardware)
    result = {'version': VERSION, 'build_type': 'Release', 'test_suites': len(records),
              'nro_sha256': nro_hash, 'nro_bytes': len(nro),
              'nacp': 'verified, including embedded NRO copy', 'romfs': 'present',
              'dependency_sources': 'SHA256 verified', 'release_status': 'production',
              'hardware_validation': 'User acceptance recorded for this binary' if tested else
                  'Production runtime accepted on Switch before the catalog-cover correction; corrected artwork awaits device confirmation',
              'docked_1080p': 'untested'}
    (AUDIT / 'validation.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tests', action='store_true')
    parser.add_argument('--tests-only', action='store_true', help='Run offline suites without a Switch build')
    args = parser.parse_args()
    if args.tests or args.tests_only:
        run_tests()
    if not args.tests_only:
        print(json.dumps(validate(), indent=2))


if __name__ == '__main__':
    main()
