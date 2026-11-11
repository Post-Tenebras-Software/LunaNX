# Publish after review

These commands require your own authenticated GitHub CLI and explicit intent
to publish. None are run automatically by LunaNX's build/package scripts.
The local repository is prepared with an `origin` URL, an initial production
commit and a `v1.0.1` tag; use `git status` and `git log` to review them first.

Create an **empty** public repository on GitHub (do not initialize another
README/license there), or use:

```sh
gh repo create Post-Tenebras-Software/LunaNX --public \
  --description "Unofficial native Amazon Luna client for Nintendo Switch homebrew"
git push -u origin main
git push origin v1.0.1
gh release create v1.0.1 --repo Post-Tenebras-Software/LunaNX \
  --verify-tag --draft --title "LunaNX v1.0.1" \
  --notes-file tools/release/RELEASE_NOTES.md \
  releases/v1.0.1/LunaNX.nro \
  releases/v1.0.1/LunaNX-v1.0.1-switch.zip \
  releases/v1.0.1/LunaNX-v1.0.1-source.tar.gz \
  releases/v1.0.1/LunaNX-v1.0.1-symbols.tar.gz \
  releases/v1.0.1/LunaNX-v1.0.1-hb-appstore.zip \
  releases/v1.0.1/BUILDINFO.json \
  releases/v1.0.1/RELEASE_NOTES.md \
  releases/v1.0.1/SHA256SUMS
```

Complete the final binary smoke test, then publish the draft from GitHub.
After the public URLs work, use the prepared HB App Store form fields/artwork
and Sphaira PR materials described in `docs/DISTRIBUTION.md`. Store review and
maintainer acceptance are separate from uploading a GitHub release.
