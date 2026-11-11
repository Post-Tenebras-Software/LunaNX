#!/usr/bin/env python3
"""Offline trailers: real FFmpeg decoding of generated media, no account or network."""
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK_FFMPEG = ROOT / 'third_party/distfiles/switch-ffmpeg/ffmpeg-7.1.tar.xz'
ffmpeg_root = Path(os.environ.get('LUNANX_TEST_FFMPEG_ROOT', ROOT / 'build-tests/ffmpeg-7.1'))
if not (ffmpeg_root / 'libavformat/libavformat.a').is_file():
    ffmpeg_root.parent.mkdir(parents=True, exist_ok=True)
    if not (ffmpeg_root / 'configure').exists():
        with tarfile.open(SDK_FFMPEG) as archive:
            archive.extractall(ffmpeg_root.parent, filter='data')
    with (ffmpeg_root.parent / 'ffmpeg-build.log').open('w') as log:
        subprocess.run(['bash', './configure', '--disable-everything', '--disable-doc', '--disable-programs',
                        '--disable-network', '--disable-autodetect', '--disable-x86asm',
                        '--enable-decoder=h264,aac,mp3,opus', '--enable-demuxer=mov', '--enable-parser=h264,aac',
                        '--enable-swscale', '--enable-swresample', '--disable-avfilter', '--disable-avdevice'],
                       cwd=ffmpeg_root, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run(['make', '-j2', *[f'{lib}/{lib}.a' for lib in
                        ('libavformat', 'libavcodec', 'libswscale', 'libswresample', 'libavutil')]],
                       cwd=ffmpeg_root, stdout=log, stderr=subprocess.STDOUT, check=True)
with tempfile.TemporaryDirectory(prefix='lunanx-trailer-test-') as directory:
    d = Path(directory)
    # HTTP is simulated by player.cpp. Use the matching bundled public curl
    # headers, rather than leaking the Switch SDK into a host-only test.
    with tarfile.open(ROOT / 'third_party/distfiles/switch-curl/curl-7.69.1.tar.xz') as archive:
        headers = [m for m in archive.getmembers() if m.isfile() and
                   m.name.startswith('curl-7.69.1/include/curl/') and m.name.endswith('.h')]
        archive.extractall(d, members=headers, filter='data')
    curl_headers = d / 'curl-7.69.1/include'
    common = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=30:duration=1']
    subprocess.run([*common, '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=1',
                    '-c:v', 'libx264', '-bf', '2', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-movflags', '+faststart',
                    '-shortest', str(d / 'av.mp4')], check=True)
    subprocess.run([*common, '-c:v', 'libx264', '-bf', '2', '-an', '-movflags', '+faststart', str(d / 'silent.mp4')], check=True)
    subprocess.run([*common, '-c:v', 'mpeg4', '-an', str(d / 'unsupported.mp4')], check=True)
    libs = [str(ffmpeg_root / lib / (lib + '.a')) for lib in
            ('libavformat', 'libavcodec', 'libswscale', 'libswresample', 'libavutil')]
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    '-Isource', '-isystem', str(ffmpeg_root), 'source/trailer_decoder.cpp',
                    'tests/trailers/decoder.cpp', '-o', str(d / 'decoder'), '-Wl,--start-group', *libs,
                    '-Wl,--end-group', '-pthread', '-lm'], cwd=ROOT, check=True)
    subprocess.run([str(d / 'decoder'), str(d / 'av.mp4'), str(d / 'silent.mp4'), str(d / 'unsupported.mp4')], check=True)
    subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    '-Isource', '-Itests/trailers/stubs', '-isystem', str(ffmpeg_root),
                    '-isystem', str(curl_headers),
                    'source/trailer_decoder.cpp', 'source/launcher_trailer.cpp', 'tests/trailers/player.cpp',
                    '-o', str(d / 'player'), '-Wl,--start-group', *libs, '-Wl,--end-group', '-pthread', '-lm'], cwd=ROOT, check=True)
    subprocess.run([str(d / 'player'), str(d / 'av.mp4')], check=True)
