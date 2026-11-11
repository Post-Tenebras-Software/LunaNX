# Distribution preparation

Target repository: **https://github.com/Post-Tenebras-Software/LunaNX**.
Release: **v1.0.1**. These are prepared destinations, not a claim that the
repository, release or any store listing has been published.

## GitHub

The local repository includes production source, dependency archives/notices,
offline tests, issue templates, CI and a complete README. Build outputs and
private state are ignored. The source archive must accompany every binary
redistribution. GitHub's automatically generated source ZIP alone is not a
substitute for the separately audited release source archive.

After reviewing the local repository and final binary, create the empty public
GitHub repository, push `main` and tag `v1.0.1`, and create a release. The install
asset is `LunaNX-v1.0.1-switch.zip`. Also attach the matching source and symbols,
NRO, checksum inventory, release notes and store ZIP. A draft is suitable until
the final binary passes a device smoke test. Commands are in
`tools/release/PUBLISHING.md`; they are never run by the build scripts.

The CI workflow builds and uploads workflow artifacts only. It does not create
public releases or submit to stores. Its remote execution has not been tested
until the repository is published and a workflow runs.

## HB App Store / 4TU

The [official submission form](https://submit.fortheusers.org/) is manually
reviewed. On 2026-11-10 it required Switch/title/author/category/contact,
version/license/repository URL, a 10–40 character short description, a detailed
description plus AI disclosure within 1024 characters, a **256×150** icon and
an **848×208** banner. It states GitHub releases are picked up for future review.

Prepared fields are in `distribution/hb-appstore/submission.json`; the contact
and AI-use category were supplied/confirmed by the author. The form's license
choice is `GPLv3`; the text and project license specify **GPL-3.0-or-later**.
AI disclosure is **Mostly AI-generated**, plus artwork and description text.

Run `python3 tools/release/store.py` after normal release packaging. It creates:

- `releases/v1.0.1/LunaNX-v1.0.1-hb-appstore.zip`, containing the same NRO and
  documentation plus `info.json` and `manifest.install`.
- `dist/store/submission-fields.json` and the exact-form prepared payload.
- A static `dist/store/repo/` layout for a custom compatible repository.

The manifest updates app/documentation files only. It never includes saved
login, settings, history or caches. A store uninstall may remove files listed
in that manifest; the user's existing app data is outside it.

Publish the GitHub repository/release first so reviewers can inspect source and
download the binary, then fill the form with the prepared fields and images.
Do not describe the app as listed or approved until maintainers confirm it.
No form POST is performed by this repository.

## Sphaira

[Sphaira](https://github.com/NaGaa95/sphaira) uses the 4TU Switch store by default,
so approval there is the normal route into its App Store view. Sphaira also has
a GitHub download browser and custom store support.

For GitHub installation, add `Post-Tenebras-Software/LunaNX` through Sphaira's
GitHub repository manager and choose `LunaNX-v1.0.1-switch.zip`. Its SD-root
layout installs `/switch/LunaNX/LunaNX.nro`. Use title takeover/full application
memory to run it. Real Sphaira installation still requires a device check.

A minimal catalog entry and prepared PR description are in
`distribution/sphaira/`. The proposed upstream file is
`assets/romfs/github/LunaNX.json`, following Sphaira's current URL-entry examples.
This is prepared for a maintainer PR after the release URLs are live; no PR has
been submitted and inclusion is not guaranteed.

For a private/local custom store test, serve `dist/store/repo/` as static files
and add its base URL through Sphaira's store selector. Publishing that directory
to HTTPS hosting is a separate action. Its `repo.json`, `packages/LunaNX/` and
`zips/LunaNX.zip` follow the observed libget/Sphaira resource paths. Test an
install/update/uninstall on hardware before relying on this alternative route.

Sources checked on 2026-11-10:
[submission form](https://submit.fortheusers.org/),
[libget layout](https://github.com/fortheusers/libget),
[Sphaira store implementation](https://github.com/NaGaa95/sphaira/blob/master/sphaira/source/ui/menus/appstore.cpp),
[Sphaira GitHub example](https://github.com/NaGaa95/sphaira/blob/master/assets/romfs/github/sphaira.json).
