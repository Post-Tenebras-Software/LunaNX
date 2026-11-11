#!/usr/bin/env python3
"""Exercise pacing deadlines with a simulated clock, without wall-clock sleeps."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="lunanx-pacing-") as temporary:
    binary = str(Path(temporary) / "pacer")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Isource", "tests/frame-pacer/unit.cpp", "-o", binary], cwd=root, check=True)
    subprocess.run([binary], check=True)
