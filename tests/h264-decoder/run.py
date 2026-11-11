#!/usr/bin/env python3
"""Exercise the real decoder wrapper against simulated FFmpeg calls, no GPU/network."""
from pathlib import Path
import os
import subprocess
import tarfile
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="lunanx-decoder-") as temporary:
    # Exercise the actual Switch FFmpeg API without requiring an installed SDK.
    # Hardware/library calls are supplied by unit.cpp; only headers are needed.
    with tarfile.open(root / "third_party/distfiles/switch-ffmpeg/ffmpeg-7.1.tar.xz") as archive:
        archive.extractall(temporary, filter="data")
    headers = Path(temporary) / "ffmpeg-7.1"
    subprocess.run(["git", "apply", str(root / "third_party/distfiles/switch-ffmpeg/ffmpeg-7.1.patch")],
                   cwd=headers, check=True)
    subprocess.run(["bash", "./configure", "--disable-everything", "--disable-doc", "--disable-programs",
                    "--disable-network", "--disable-autodetect", "--disable-x86asm"], cwd=headers, check=True)
    binary = str(Path(temporary) / "decoder")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Isource", "-isystem", str(headers),
                    "source/h264_decoder.cpp", "tests/h264-decoder/unit.cpp", "-o", binary], cwd=root, check=True)
    subprocess.run([binary], cwd=root, check=True)
