# Third-party notices

LunaNX's original code is GPL-3.0-or-later; see COPYRIGHT and LICENSE. This
release statically links a GPL-enabled FFmpeg build. Its distribution includes
source and build instructions, rather than treating FFmpeg as LGPL-only.

| Component | License / selected terms | Source in the accompanying source archive |
| --- | --- | --- |
| libdatachannel and libjuice | MPL-2.0; original file notices retained | `external/libdatachannel`, including local Switch adaptations |
| usrsctp | BSD-3-Clause | `external/libdatachannel/deps/usrsctp` |
| libSRTP | BSD-3-Clause | `external/libdatachannel/deps/libsrtp` |
| plog | MIT | `external/libdatachannel/deps/plog` |
| nlohmann/json | MIT | `external/libdatachannel/deps/json` |
| Mbed TLS | Apache-2.0 (dual-license text retained) | `external/mbedtls`; Switch configuration in `config/mbedtls_switch.h` |
| FFmpeg 7.1, devkitPro package 7.1-5 | GPL-2.0-or-later; used under GPLv3 in this combined work | `third_party/distfiles/switch-ffmpeg`, including both Switch patches |
| curl 7.69.1 | curl license (not the mislabeled package metadata) | `third_party/distfiles/switch-curl`, including Switch patch |
| FreeType 2.13.3 | GPL-2.0-or-later; used under GPLv3 | `third_party/distfiles/switch-freetype` |
| HarfBuzz 10.0.1 | MIT-style notices in COPYING | `third_party/distfiles/switch-harfbuzz` |
| libpng 1.6.48 | PNG Reference Library license | `third_party/distfiles/switch-libpng` |
| zlib 1.3.1 | zlib | `third_party/distfiles/switch-zlib` |
| bzip2 1.0.8 | bzip2 license | `third_party/distfiles/switch-bzip2` |
| dav1d 1.5.0 | BSD-2-Clause, additional file notices retained | `third_party/distfiles/switch-dav1d`, including Switch patch |
| libnx 4.12.0 | ISC and file-specific notices | `third_party/distfiles/libnx` |
| Inter fonts | SIL Open Font License 1.1 | `assets/launcher/fonts`; also embedded in RomFS |
| GCC/libstdc++ runtime | GPLv3 with GCC Runtime Library Exception | SDK prerequisite; notices in `third_party/licenses/toolchain-runtime` |
| newlib/libsysbase/libm runtime | Permissive, file-specific notices | SDK prerequisite; consolidated notices in `third_party/licenses/toolchain-runtime` |

Full license texts are in `third_party/licenses`. Local vendor base revisions
and changed paths are recorded in `third_party/vendors.json`; the source
archive contains the actual modified files. Library archive URLs and SHA256
values are in `third_party/sources.json`. The corresponding official devkitPro
recipes are included under `third_party/recipes`; downloads are not executed
by the packager.

The combined binary is distributed under GPLv3-or-later subject to retained
dependency terms. MPL-covered source files remain available under MPL-2.0.
Keep the matching source archive available alongside the binary when sharing
this release. The archive includes LunaNX, patched RTC/crypto sources and
matching portlib/libnx sources and recipes. The devkitPro compiler/SDK and its
runtime exception remain build prerequisites; see `tools/release/BUILDING.md`.

Official references: [FFmpeg license](https://ffmpeg.org/legal.html),
[MPL-2.0](https://www.mozilla.org/en-US/MPL/2.0/),
[devkitPro package recipes](https://github.com/devkitPro/pacman-packages).

Amazon's client bundles, account data, credentials and cached game artwork are
excluded from the distributed source. Game images and metadata are downloaded
from Luna for the signed-in user; no games or game licenses are bundled.
