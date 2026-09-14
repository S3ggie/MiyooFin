# Task 10 — Extract the top-level LibrarySync owner

**Phase:** B — ownership/modularity

## Objective

Move Jellyfin → SQLite top-level synchronization out of HomeScreen into one lightweight app-scoped `LibrarySync` component.

## Why

HomeScreen currently owns network, generation lifecycle, page population, cancellation, and presentation.

## Preconditions

- CP-A is explicitly approved.
- Tasks 01-09 are committed/passing.

## Allowed Files

- Create `src/library/LibrarySync.hpp`
- Create `src/library/LibrarySync.cpp`
- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/screens/HomeScreen.cpp` and `.hpp`
- `src/app/App.cpp` and `.hpp`
- `Makefile`
- `Makefile.cross`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_ui_foundation.inc`

## Forbidden Scope

- Do not change Phase-A SQL/schema semantics.
- Do not create abstract repositories/DI framework.
- Do not move artwork/download UI workers.
- Do not unify hierarchy consumers yet.
- App remains scope owner.

## Architecture invariants

- Schema v3 remains the baseline. Do not introduce schema v4 or redesign the schema unless this task explicitly requires it; no task in this roadmap currently does.
- Exactly one `CatalogDb` SQLite worker owns exactly one SQLite connection for the active scope.
- No SQLite operation, HTTP request, long filesystem operation, retry sleep, or blocking worker join may run on the SDL/UI thread.
- Keep indexed/keyset bounded reads and bounded result windows; do not reintroduce whole-library RAM materialization.
- Keep the atomic fresh-database temporary-file bootstrap/promotion path unchanged unless a task explicitly targets it.
- `DownloadStore` remains authoritative for physical offline availability. Catalog metadata may enrich downloads but must not decide whether bytes exist.
- `ImageCache` remains the artwork-byte cache and is not replaced by SQLite.
- Do not add production dual-write to legacy whole-file catalog/cache persistence.
- The intermittent audio-only FFplay/mmiyoo display bug is out of scope. Do not modify playback repair code in this roadmap.

## Implementation requirements

1. App owns one concrete LibrarySync for active session/scope; it owns top-level refresh worker, cancellation, and monotonic sync generation.
2. Move Views + bounded page iteration + begin/stage/finalize/abort logic into LibrarySync.
3. Move CW/RA request execution into LibrarySync as optional rail results.
4. Expose a small thread-safe immutable status/result snapshot: in-flight, latest generation outcome, optional rails/status.
5. LibrarySync never touches SDL/tab/card/ImageCache state.
6. Home can request refresh and poll/take results but no longer invokes CatalogDb page-write/generation APIs.
7. Cancel/join safely on session teardown using existing cancellable HTTP; no detached threads.

## Tests

- Optional rail failure still commits catalog generation.
- Required page failure aborts stage and reports failure.
- Newer refresh supersedes older unpublished work.
- Home no longer begins/stages/finalizes top-level DB generations.
- LibrarySync destruction/cancel cannot publish stale results.

## Validation

Run the following from the repository root. Do not report a command as passed unless it actually completed successfully.

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
make onionos
make verify-arm
git diff --check
```

## Hardware gate

None. This task is host/ARM-build validation only; do not deploy to a Miyoo Mini Plus.

## STOP conditions

- The change needs a production file outside **Allowed Files**.
- The task appears to require schema v4, a second SQLite connection/worker, full-library materialization, or SQLite/network work on the SDL thread.
- A prerequisite task/checkpoint is missing or the current repository state contradicts the task assumptions.
- Required validation fails for a reason outside this task's narrow scope.

## Commit boundary

This task is exactly one commit. Commit only files allowed above after all required validation passes.

```text
refactor(library): extract top-level sync owner
```

Do not include any part of Task 11 in this commit.
