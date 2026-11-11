# Release dependency inputs

`sources.json` lists upstream release archives and devkitPro Switch patches.
Every file in `distfiles/` was verified against the SHA256 value in its included
official package recipe. The archives match the installed library versions
recorded in the release BUILDINFO. Recipes are retained verbatim, including
upstream metadata mistakes; the actual notices take precedence over a package
license label. No downloaded recipe is executed by the packager.

`recipes.json` records each recipe's public URL, retrieval date and content hash.
URLs describe provenance; the included files and hashes fix this release's
inputs even if an upstream branch changes later. Zlib and curl archives use
official mirror URLs where the recipe's original URL no longer serves the file;
their hashes still match the recipes.

`vendors.json` identifies the checked-out bases for sources already vendored in
`external/`. It lists local modifications; the source distribution includes
those modified files in full and excludes Git histories. These changes adapt
Switch socket/address handling, random generation, SCTP platform support and
RTC shutdown. Their upstream licenses and file notices are retained. Mbed TLS
uses the separate `config/mbedtls_switch.h` configuration and
`source/mbedtls_switch.c` platform hooks.

`licenses/` contains full notices for the linked libraries, fonts and compiler
runtime. See the root THIRD_PARTY_NOTICES.md for license choices and source paths.
Developer SDK/tools remain prerequisites; follow `tools/release/BUILDING.md`.
