# Contributing

Discuss larger changes in an issue first. Keep changes focused and preserve
working streaming, authentication and controller behavior.

Use synthetic data in tests. Never commit credentials, authentication captures,
SDP, downloaded Amazon client bundles, game artwork or device-specific state.
Clearly distinguish offline tests, hardware observations and assumptions.

Run the relevant tests, and the full release checks before release packaging:

```sh
python3 tools/release/check.py --tests
```

For UI changes, run `python3 tools/launcher-preview/run.py`; previews are written
under `build-tests/previews/`. Build with devkitPro as described in
`tools/release/BUILDING.md`. Hardware checks should include audio/input, return
to Home, session control and clean exit. Docked output needs separate testing.

New code must be compatible with GPL-3.0-or-later. Preserve dependency notices.
Disclose material AI assistance in contributions and describe what was reviewed
and tested; generated output is held to the same standard as other code.
