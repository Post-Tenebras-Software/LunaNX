#!/usr/bin/env python3
"""Reproduce ICE numeric→PTR conversion and test the Switch formatting adapter."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
JUICE = ROOT / "external/libdatachannel/deps/libjuice"


def run(args):
    result = subprocess.run([str(arg) for arg in args], cwd=ROOT, text=True,
                            capture_output=True, timeout=90)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    print(result.stdout, end="", flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix="lunanx-ice-address-") as temporary:
        temp = Path(temporary)
        includes = ["-Iexternal/libdatachannel/include", "-Iexternal/libdatachannel/include/rtc",
                    "-Iexternal/libdatachannel/deps/plog/include", "-I" + str(JUICE / "include/juice"),
                    "-I" + str(JUICE / "include"), "-I" + str(JUICE / "src")]
        objects = []
        for source in (JUICE / "src/ice.c", JUICE / "src/addr.c", JUICE / "src/log.c"):
            target = temp / (source.stem + ".o")
            run([os.environ.get("CC", "gcc"), "-std=gnu11", "-ffunction-sections", "-fdata-sections",
                 "-DRELEASE=1", *includes, "-c", source, "-o", target])
            objects.append(target)
        resolver = temp / "resolver.o"
        run([os.environ.get("CC", "gcc"), "-c", "tests/ice-address/resolver.c", "-o", resolver])
        objects.append(resolver)
        adapter = temp / "numeric_address.o"
        run([os.environ.get("CC", "gcc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
             "-c", "source/numeric_address.c", "-o", adapter])
        common = [os.environ.get("CXX", "g++"), "-std=c++17", "-DPLOG_DISABLE_LOGGING", "-DRTC_STATIC",
                  "-ffunction-sections", "-fdata-sections", *includes,
                  "external/libdatachannel/src/candidate.cpp", "tests/ice-address/probe.cpp",
                  *objects, "-pthread", "-Wl,--gc-sections"]
        original = temp / "original"
        run([*common, "-o", original])
        run([original, "negative-control"])
        fixed = temp / "fixed"
        run([*common, adapter, "-Wl,--wrap=getnameinfo", "-o", fixed])
        run([fixed])


if __name__ == "__main__":
    main()
