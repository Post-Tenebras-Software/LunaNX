# LunaNX v1.0.1

Unofficial native Amazon Luna client for Nintendo Switch homebrew.
Author: PostTenebrasSoftware. Original code: GPL-3.0-or-later.

## Changes

- Game EULA review, explicit acceptance and automatic launch continuation.
- Recently Played saves correctly across app restarts.
- Recently Played retains catalog covers and repairs older background images
  when matching Home/Catalog tiles load, without changing play order.
- Profile-bound client identity before catalog and launch requests.
- Return to game and separate End Game controls above Recently Played.
- Confirmation before ending one game and launching another; failures stop the
  replacement launch instead of silently leaving conflicting sessions.
- Clearer failed-job diagnostics, production-only source repository, original
  application/store icon, documented installation and release packages.

## Install

Extract `LunaNX-v1.0.1-switch.zip` to the SD root. Launch
`/switch/LunaNX/LunaNX.nro` through hbmenu or Sphaira with full application
memory/title takeover. Album/applet mode is not supported. Keep the existing
LunaNX data files when updating. Initial sign-in uses Amazon's device-code page;
a PC companion is unnecessary.

The two-hour timeout applies only to waiting in Luna's queue. LunaNX does not
impose a two-hour gameplay cutoff after streaming starts; Luna's own session
rules still apply.

## Controls and sessions

Launcher prompts use Switch labels. Games use Xbox **button positions**:
Switch B → Xbox A, A → B, Y → X, X → Y.

Hold Plus, then tap Capture to disconnect locally and return Home. The shortcut
is configurable; when Capture is unavailable, hold Minus + Plus for two seconds.
Return to game reconnects through fresh permission; End Game requires explicit
confirmation and can lose unsaved progress. Switching games also asks first.
These controls retain the session launched in this app run and original profile;
they do not discover games left running by other clients or a previous run.

Hold Minus + R for one second to toggle the overlay when the shortcut does not
conflict. From Home, R opens Catalog, Y opens Search and X opens Settings.
On game pages, Y opens the full description, ZL plays a supported trailer and
ZR opens screenshots. Plus exits from the launcher.

## Validation and limitations

The standalone production runtime was accepted on a real Switch. Handheld
720p60 video, audio/input, sign-in, profiles, descriptions, trailers and history
were tested during development. The final catalog-cover correction has offline
coverage and awaits device confirmation; store installation is also unverified.
See `docs/VALIDATION.md`. **Docked 1080p remains untested.**

Publisher-account linking, PIN/parental-control editing, child/teen management,
profile deletion and arbitrary photo upload are not provided. Unknown or
restricted account flows remain blocked. Luna controls game access, queueing
and actual stream bitrate; its unofficial API may change.

Keep the matching source archive available alongside the binary. See LICENSE,
COPYRIGHT and THIRD_PARTY_NOTICES.md for LunaNX and dependency terms. No games,
Amazon client bundles, credentials or downloaded game artwork are included.
