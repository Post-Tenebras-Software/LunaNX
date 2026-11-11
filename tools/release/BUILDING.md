# Build LunaNX 1.0.1

This repository contains the complete production client, patched vendor trees,
font files, offline tests, matching portlib/libnx source archives and recipes.
It does not require a companion or a recursive submodule checkout.

Install the official [devkitPro toolchain](https://devkitpro.org/wiki/Getting_Started)
for your host, including `switch-dev`, `switch-cmake`, `switch-curl`,
`switch-ffmpeg` and `switch-freetype` and their dependencies. The release was
built with devkitA64 GCC 16.1.0; exact installed versions are in release
`BUILDINFO.json`. Host build tools are Bash, CMake, make and Python 3.11+.

```sh
bash tools/release/build.sh
```

The script builds Mbed TLS, patched libdatachannel/libjuice/usrsctp/libSRTP,
then the sole production app at **`build-release/LunaNX.nro`**. Release flags
are `-O2 -DNDEBUG`. ELF symbols and a link map are retained separately.

For an app-only rebuild once its libraries exist:

```sh
source /etc/profile.d/devkit-env.sh
cmake -S . -B build-release \
  -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-release --target LunaNX_nro --parallel 2
```

On hosts without `/etc/profile.d/devkit-env.sh`, use your devkitPro environment
setup and set `DEVKITPRO` as appropriate. The build script also accepts
`LUNANX_BUILD_JOBS` (default 2). It never invokes sudo or installs packages.

## Offline tests

Tests use synthetic data and never authenticate to Amazon. Install a host C++17
compiler, Git, Node.js, pkg-config, FreeType/PNG development libraries and FFmpeg.
The host tests do not require devkitPro: the hardware decoder mock configures
the bundled FFmpeg sources with their Switch patch for matching headers, while
the trailer HTTP mock uses public headers from the bundled curl archive.
Trailer/decoder tests can build the included FFmpeg source in a test-only
directory if compatible host development libraries are unavailable.

```sh
python3 tools/release/check.py --tests-only
python3 tools/release/check.py --tests
```

The second command also validates the compiled NRO, embedded version/icon/NACP,
RomFS, release flags, dependency hashes and regression record freshness.

`python3 tools/launcher-preview/run.py` renders offline interface previews in
`build-tests/previews/`. To regenerate branding PNG/JPEG from the checked-in
vector/code design, install Pillow and run `python3 tools/branding.py`.

## Package a release

```sh
python3 tools/release/package.py
python3 tools/release/store.py
```

Packaging refuses to overwrite an existing release. It produces a manual SD
install ZIP, matching complete source archive, separate symbols, notices,
validation records and SHA256 checksums. Store preparation adds a manifest-based
HB App Store ZIP, local/custom Sphaira store layout and submission metadata.
Nothing is uploaded or submitted by these scripts.

For dependency changes, inspect `third_party/vendors.json`, `sources.json` and
`recipes.json`. Matching archives and Switch patches are under
`third_party/distfiles/`; official package recipes are under
`third_party/recipes/`. These sources describe the linked release libraries;
the SDK compiler/runtime remains an external build prerequisite. FFmpeg's full
package recipe also needs switch-libass and switch-libfribidi to configure its
complete library, although those are not linked by LunaNX.

This is a tested rebuild procedure, not a promise of bit-identical binaries
across different compilers or paths. CI uses devkitPro's current public image
and packages, so dependency changes there can require a new release audit.
