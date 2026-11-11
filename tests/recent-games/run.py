#!/usr/bin/env python3
"""Offline history storage and profile/MRU checks; no account data or network."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="lunanx-recent-") as temporary:
    binary = str(Path(temporary) / "test")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Isource", "-Iexternal/libdatachannel/deps/json/include", "source/launcher_recent.cpp",
                    "tests/recent-games/main.cpp", "-o", binary], cwd=root, check=True)
    subprocess.run([binary, temporary], cwd=root, check=True)
