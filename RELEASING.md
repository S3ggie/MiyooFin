# Release Packaging

Public binary releases of MiyooFin must be produced with `tools/build-release.sh`.
The wrapper uses the existing `make package` staging process, then adds the
project GPL license and third-party notices before creating the distributable
ZIP under `output/release/MiyooFin.zip`.

Do not upload the raw `output/package/` staging directory as a public release
without first adding `LICENSE` and `THIRD_PARTY_NOTICES.md`.

Before publishing a release, run the focused legal-packaging check:

`sh tests/test_release_legal.sh`

The source archive for the same GitHub release tag is the corresponding
MiyooFin source for that binary release. Keep the binary asset and its matching
source tag available together.

## OTA release assets

Each release must publish three assets for the on-device OTA updater:

- **MiyooFin.zip** — manual install archive (existing).
- **MiyooFin.tar.gz** — OTA artifact (preserves paths and permissions).
- **manifest.json** — OTA discovery file with version, checksums, and download URLs.

The manifest is served at:

`https://github.com/S3ggie/MiyooFin/releases/latest/download/manifest.json`

### Build and publish steps

```sh
# 1. Build the release assets (ZIP, tarball, manifest).
#    RELEASE=1 enables the slim release profile (-Os, no debug info, stripped).
sh tools/build-release.sh

# 2. Run the legal/packaging checks.
sh tests/test_release_legal.sh

# 3. Publish to GitHub (creates the release and uploads all three assets).
sh tools/publish-release.sh v<X.Y.Z>
```

The publish script refuses to run if the tag already exists. The manifest's
`tag` field must match the argument.
