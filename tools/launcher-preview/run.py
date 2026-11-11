#!/usr/bin/env python3
"""Render the native UI on desktop using synthetic, offline fixtures."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="lunanx-launcher-preview-") as temporary:
    binary = str(Path(temporary) / "preview")
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "freetype2", "libpng"], text=True))
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    '-DLUNANX_BUILD_VERSION="1.0.1"', '-DLUNANX_BUILD_CHANNEL="1.0.1"', "-Isource", "source/launcher_ui.cpp", "source/launcher_image_cache.cpp", "source/launcher_shortcut.cpp",
                    "source/performance_overlay.cpp",
                    "tools/launcher-preview/main.cpp", "-o", binary, *flags], cwd=ROOT, check=True)
    subprocess.run([binary, str(ROOT / "build-tests/previews"), str(Path(temporary) / "image-cache")], cwd=ROOT, check=True)
