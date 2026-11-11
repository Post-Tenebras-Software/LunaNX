#!/usr/bin/env python3
"""Check real composition code against a simulated libnx framebuffer."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="lunanx-view-") as temporary:
    binary = str(Path(temporary) / "view")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Itests/video-view/stubs", "-Isource", "source/video_view.cpp", "tests/video-view/unit.cpp",
                    "-o", binary], cwd=root, check=True)
    subprocess.run([binary], check=True)
