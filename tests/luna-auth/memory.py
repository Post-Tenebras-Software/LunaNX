#!/usr/bin/env python3
"""Offline synthetic credentials only. Builds the vendored AES/GCM implementation."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="lunanx-login-memory-") as directory:
    temporary = Path(directory)
    flags = ['-DMBEDTLS_CONFIG_FILE="login_store_config.h"', "-Itests/luna-auth",
             "-Iexternal/mbedtls/include", "-Iexternal/mbedtls/library"]
    objects = []
    for name in ("aes", "gcm", "cipher", "cipher_wrap", "platform_util", "constant_time"):
        output = temporary / (name + ".o")
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-O2", *flags,
                        "-c", f"external/mbedtls/library/{name}.c", "-o", str(output)], cwd=ROOT, check=True)
        objects.append(str(output))
    binary = str(temporary / "memory")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    *flags, "-Isource", "-Iexternal/libdatachannel/deps/json/include", "source/native_auth.cpp",
                    "source/remembered_login.cpp", "tests/luna-auth/memory.cpp", *objects, "-o", binary],
                   cwd=ROOT, check=True)
    subprocess.run([binary, str(temporary / "storage")], cwd=ROOT, check=True)
    key_binary = str(temporary / "key-switch")
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Itests/luna-auth/switch-stubs", "-Isource", "source/login_key_switch.cpp",
                    "tests/luna-auth/key_switch.cpp", "-o", key_binary], cwd=ROOT, check=True)
    subprocess.run([key_binary], cwd=ROOT, check=True)
