# Refactor2 Task 11 — Modularize App external playback lifecycle

## Objective

Move playback-result ingestion, offline journal synchronization, suspend/resume handling, external playback orchestration, and playback-start overlay rendering into focused App translation units.

## Preconditions

- Task 10 commit exists.
- Baseline: playback group, playback reporter tests, bridge tests, and runner shell test pass.

## Allowed Files

- `src/app/App.cpp`, `App.hpp`
- New `src/app/AppPlayback.cpp`
- New `src/app/AppPlatform.cpp` only if suspend/resume helpers form a clean independent unit
- Playback test files
- `tools/playback_*` and `distributions/onionos/playback_runner.sh` only for compilation/include fallout; behavior edits are forbidden
- `Makefile`, `Makefile.cross`, `Makefile.desktop`

## Required result

- Move `ingestPlaybackResult`, result recovery, journal scheduling/loop/sync, external playback handling, and playback overlay definitions with their exclusive helpers.
- If created, `AppPlatform.cpp` contains only existing platform suspend/resume definitions and exclusive helpers.
- Preserve request/result files, reporter and journal semantics, localhost bridge routing, complete-download-first resolution, process invocation, SDL/platform teardown/reinitialization, overlay timing, cancellation, and thread joins.
- Do not modify playback repair behavior, scripts, routes, media profiles, or authenticated URL handling.

## Verification

Run:

```sh
make output/test/test_playback -j2
make bridge bridge-test reporter reporter-test -j2
sh tests/test_playback_runner.sh
sh -n distributions/onionos/playback_runner.sh
make test -j2
make -j2
git diff --check
```

Do not run OnionOS or deploy. STOP if platform behavior must change.

## Commit

```text
refactor(app): isolate external playback lifecycle
```
