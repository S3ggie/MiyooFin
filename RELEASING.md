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
