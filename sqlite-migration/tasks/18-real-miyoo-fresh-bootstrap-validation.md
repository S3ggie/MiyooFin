# Task 18 — Real Miyoo fresh-bootstrap validation

**HARDWARE REQUIRED — this task cannot be completed without actual Miyoo Mini Plus evidence.**

## Execution mode — roadmap override

For this numbered SQLite roadmap task, implement/validate directly with the
selected GPT-5.6 Luna High. Do not spawn subagents or orchestration artifacts.
One task equals one narrow commit. After success, stop for CP-C.

## Goal

Prove fresh SQLite bootstrap and initial population are safe and useful on a real
Miyoo Mini Plus. Validate both a network-available first launch populated from
current Jellyfin metadata and an offline first launch that preserves browsing and
playability of complete downloads through DownloadStore metadata.

## Depends On

- Task 17
- CP-B — Fresh bootstrap/reconciliation/offline-download behavior approved

## Allowed Files

- `docs/sqlite-migration-benchmark.md` — factual evidence for the exact tested commit
- No application/source changes in this hardware-validation task
- If a code fix is needed, stop and create a separate reviewed patch before retrying

## Forbidden Scope

- No `catalog.v1` import, parity requirement, mutation, or deletion.
- No legacy-catalog migration claim.
- No journal-mode decision yet.
- No consumer cutover.
- No hardware completion claim from host/Docker.

## Exact implementation requirements

1. Start with no final SQLite DB; record any `catalog.v1` only as an untouched
   rollback artifact and verify the bootstrap path does not read it.
2. Launch through the normal OnionOS path with a valid saved session.
3. Verify fresh `.migrating` creation/lifecycle, final DB promotion, schema,
   `quick_check`, `foreign_key_check`, and clean reopen.
4. With network available, verify current Jellyfin hierarchy population and that
   completion remains false until the authoritative generation commits.
5. With network unavailable, verify complete DownloadStore items remain browsable
   and locally playable using reconstructed minimum hierarchy/state.
6. Verify normal close/relaunch and process-kill recovery during fresh bootstrap or
   reconciliation; recovery must preserve the last committed DB and downloads.
7. Collect required telemetry, CPU/RSS/I/O, DB size, timing, environment, and
   hardware evidence using `BENCHMARK_PROTOCOL.md`.
8. Record factual evidence in `docs/sqlite-migration-benchmark.md` for this commit.

## Invariants

- Real Miyoo evidence only.
- Jellyfin remains server metadata authority.
- DownloadStore remains local media/download authority.
- `catalog.v1` remains untouched and unused by bootstrap.
- No partial DB becomes authoritative.

## Focused tests

- Fresh online bootstrap/population.
- Fresh offline-with-downloads bootstrap.
- Complete-download browsing and local playback.
- Clean restart/reopen.
- Process-kill fresh rebuild/recovery.
- SQLite integrity and scope/checkpoint invariants after recovery.

## Complete validation commands

```sh
make onionos
make verify-arm
# deploy using the approved normal workflow
# launch via distributions/onionos/launch.sh / normal Onion launcher
# execute BENCHMARK_PROTOCOL fresh-bootstrap scenarios
# collect/decode MFT telemetry and safe CatalogDb metrics
# validate SQLite and downloaded/offline behavior on-device
git diff --check
git status --short
```

## Commit message

```text
test(catalog): validate fresh SQLite bootstrap on Miyoo hardware
```

This task file authorizes exactly one commit for this task after every required validation succeeds.

## STOP conditions

Stop if no physical evidence exists, bootstrap/recovery corrupts the DB, complete
downloads are not browsable/playable offline, stale state publishes, or evidence
cannot identify the tested commit/profile.
