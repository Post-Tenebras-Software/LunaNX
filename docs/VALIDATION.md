# Release validation

The production runtime derives from **1.0.1-id2**, SHA256
`f6ef62ed223e712cfc938dfff16dab0c62141247d1cfa4cda61a21b12ec5ad7b`.
On 2026-11-10 the author reported all session controls working on Switch.
The saved development log corroborated three successful remote termination
acknowledgments, termination before replacement launch and three Ready sessions.
No raw device logs, identifiers or account data are distributed here.

The production export selects those existing launcher/runtime branches and
retains the profile-bound identity lookup. It removes unused console entry
points and Fedora handoff helpers, adds the 1.0.1 identity and original icon,
and provides one build target. Source-export provenance is recorded in
`PRODUCTION_BASELINE.json`.

Offline suites and a fresh source-tree build are release gates. Their results,
toolchain/package versions and binary hashes accompany the release. They do
not prove live Amazon availability or physical-device behavior.

The author also tested the standalone production build on Switch on 2026-11-10,
SHA256 `8a56ba40e9ed6a475da5d44c4e4915c8a5fe8d1d21e62ca5db7a57c6c959701f`,
and reported everything working, with different artwork in Recently Played.
The subsequent correction preserves catalog covers rather than replacing them
with game-page backgrounds and repairs existing entries when matching catalog
tiles load. Offline tests cover persistent repairs, unchanged play order and
profile isolation. The author then confirmed the correction on Switch in the
final production NRO, SHA256
`6f5099658aa4f325ac9515b5735f970d49a39cea20aab541656d7de92d0714f5`.

**Not yet independently verified:** installation through HB App Store/Sphaira,
and docked 1080p. The individual confirmation-cancel paths have
offline coverage; the author's broad acceptance report did not enumerate each
one separately. No store listing or maintainer approval is claimed.
