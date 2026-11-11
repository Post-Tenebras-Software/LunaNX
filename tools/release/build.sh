#!/usr/bin/env bash
# Rebuild the bundled Switch adaptations, then the production client.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
release_root="$PWD"
if [[ -f /etc/profile.d/devkit-env.sh ]]; then source /etc/profile.d/devkit-env.sh; fi
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
release_jobs="${LUNANX_BUILD_JOBS:-2}"
release_toolchain="${DEVKITPRO}/cmake/Switch.cmake"

cmake -S external/mbedtls -B build-mbedtls-switch \
  -DCMAKE_TOOLCHAIN_FILE="$release_toolchain" -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DGEN_FILES=OFF \
  -DMBEDTLS_USER_CONFIG_FILE="$release_root/config/mbedtls_switch.h"
cmake --build build-mbedtls-switch --parallel "$release_jobs"

cmake -S external/libdatachannel -B build-libdatachannel-switch \
  -DCMAKE_TOOLCHAIN_FILE="$release_toolchain" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DUSE_MBEDTLS=ON -DNO_WEBSOCKET=ON \
  -DNO_EXAMPLES=ON -DNO_TESTS=ON -DNO_SERVER=ON \
  -DRTC_UPDATE_VERSION_HEADER=OFF \
  -DCMAKE_PROJECT_libdatachannel_INCLUDE="$release_root/config/libdatachannel_switch.cmake"
# BUILD_SHARED_LIBS=OFF makes the main target emit the libdatachannel.a imported
# by LunaNX. The separate datachannel-static target has a different filename.
cmake --build build-libdatachannel-switch --target datachannel --parallel "$release_jobs"

cmake -S . -B build-release \
  -DCMAKE_TOOLCHAIN_FILE="$release_toolchain" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-release --target LunaNX_nro --parallel "$release_jobs"
