# SQLite integration refactor hardware validation

## Current-tree exploratory validation

This record intentionally validates the current working tree rather than the
historical Task 30 commit. The deployed program was built from branch
`audit/sqlite-integration-current` at committed HEAD
`cadf5af43d935ec7036c8ddb8f605a0d0674970f` plus the pre-existing unstaged
source changes present in that working tree. Those source changes are not
included in this evidence-only commit.

Validation date: 2026-09-13.

The deployed ARM package contained:

| artifact | SHA-256 |
| --- | --- |
| `miyoofin` | `c25884e85567226f1e1c0d769bd915aa506af5cef034bb05cc275197954d51d9` |
| `playback_runner.sh` | `9ab4f28e7e5b563a2f3cd0ab854a38128991bb45d0966c9cc86fbb146ef31442` |

The package was transferred through the developer SSH service on port 2222.
The two changed artifacts were SHA-256 verified on the device and atomically
renamed into `/mnt/SDCARD/App/MiyooFin/`. Existing catalog, cache, download,
session, and developer-tool files were preserved.

## Device evidence

- Device: Miyoo Mini Plus, `armv7l`, Linux `4.9.84`.
- Device storage at postflight: 49.7 GiB free of 119.1 GiB.
- Scoped SQLite catalog present at `cache/library/907a3789cc02e9e4/catalog.sqlite3`.
- Download roots remained present for scopes `907a3789cc02e9e4`,
  `919ee3e4169ee6f2`, and `anonymous`.
- Compatibility `OfflineCatalog` artifacts remained present under
  `cache/offline/`; this was not a legacy-retirement test.
- After exit, exactly one `MainUI` process was resident and no `miyoofin`
  process remained.
- Telemetry mode refreshed `telemetry-logs/telemetry.mft`; the trace was not
  copied into this repository.

## Observed scenarios

The current-tree package was launched through the validated Onion-native
handoff with telemetry enabled. During the physical smoke run, the user
reported that Home, Movies, Shows, Anime, artwork, and navigation looked
decent. No new browsing or hierarchy error was reported during that run.

The graceful-exit path was independently exercised while the application was
resident. The remote exit helper returned success, MiyooFin exited, MainUI was
restored, and the Onion-native launcher completed successfully.

This run was an exploratory current-build check, not a destructive fresh-data
matrix. Separate per-screen item counts, a network-disabled interaction, and
an exit initiated at a precisely captured synchronization point were not
instrumented. The evidence therefore records practical hardware confidence
for the current fixes without claiming reproducible historical Task 31
baseline coverage.

## Host validation

- `make test -j2`: PASS
- `make -j2`: PASS (up to date)
- `make onionos`: PASS
- `make verify-arm`: PASS
- `make refactor-check`: PASS
- `git diff --check`: PASS
