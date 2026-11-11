#!/usr/bin/env python3
"""Offline video request and conversion-bound checks, without Switch or network."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="lunanx-video-mode-") as temporary:
    binary = str(Path(temporary) / "unit")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Isource", "source/luna_control.cpp", "source/luna_time_sync.cpp",
                    "source/local_control_session.cpp", "source/launcher_preferences.cpp",
                    "tests/video-mode/unit.cpp", "-o", binary], cwd=root, check=True)
    subprocess.run([binary, str(Path(temporary) / "settings")], check=True)
