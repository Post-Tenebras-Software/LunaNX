# Native launcher preview

This uses the same C++ renderer and bundled Inter fonts as the Switch launcher.
It writes sixteen 1280x720 PNGs to `build-launcher/previews/` using synthetic
offline display fixtures. No login, HTTP, account data or game allocation occurs.
The sample title is presentation data, not evidence of availability/entitlement.
Artwork is intentionally absent so the neutral loading/failure covers are shown.
No sample game list, credentials or demo code is compiled into the Switch app.

From the repository root, with desktop C++/pkg-config/FreeType/libpng headers:

```sh
python3 tools/launcher-preview/run.py
```

The runner also checks empty/partial/final-row navigation, horizontal notice
choices, loaded screenshot pixels, one-shot Play continuation/cancellation,
bad artwork origins, JPEG CDN transforms, long UTF-8 display content, output
buffer dimensions and bounded disk cache behavior (including corruption and
unavailable paths). Cache tests use a temporary directory, not an SD card.
Shortcut checks cover hold thresholds, cancelled holds, partial release across
the return-to-launcher boundary, recording/cancellation, disconnects and saved
settings validation. Settings tests also use temporary files. Previews include
remembered-account status and the explicit forget/sign-out confirmation.
The shared renderer also shows the bright controller badges and footer bar;
inspect the home, notice and settings PNGs to review prompt spacing and contrast.
These are rendering
and offline regression checks; they do not establish Switch framebuffer
handoff, artwork-server compatibility or live gameplay through the GUI.
