#!/usr/bin/env python3
"""Exercise the real decoder wrapper against simulated FFmpeg calls, no GPU/network."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="lunanx-decoder-") as temporary:
    binary = str(Path(temporary) / "decoder")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Isource", "-isystem", "/opt/devkitpro/portlibs/switch/include",
                    "source/h264_decoder.cpp", "tests/h264-decoder/unit.cpp", "-o", binary], cwd=root, check=True)
    subprocess.run([binary], cwd=root, check=True)
