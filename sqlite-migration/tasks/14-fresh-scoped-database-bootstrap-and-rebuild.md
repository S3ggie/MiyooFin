# Task 14 — Fresh scoped database bootstrap and rebuild

## Execution mode — roadmap override

Implement directly with the selected GPT-5.6 Luna High. Do not spawn subagents
or create orchestration artifacts. One task equals one narrow commit; preserve
all other repository safety, threading, and validation rules.

## Goal

Implement current-scope worker-side fresh SQLite bootstrap and disposable rebuild
staging. A supported final database is opened normally. When no final database
exists, create an empty schema database through `catalog.sqlite3.migrating`,
validate it, and promote it atomically. This task must not read or import
`cache/offline/<scope>/catalog.v1`.

## Depends On

- Task 13

## Allowed Files

- CatalogDb/bootstrap-specific files under `src/catalog/`
- Catalog schema/path helpers
- Focused tests

## Forbidden Scope

- No parsing, importing, rewriting, or deleting `catalog.v1`.
- No Jellyfin network population yet.
- No DownloadStore offline reconstruction yet.
- No production consumer read switch.
- No deletion or replacement of a valid final DB.

## Exact implementation requirements

1. Use only the current CatalogDb scope/epoch and existing scope derivation.
2. If a supported final DB exists, open it and let SQLite recover its full DB family.
3. If final DB is absent, treat an incomplete `.migrating` family as disposable,
   create a fresh empty schema DB, validate it, close it, fsync it, and promote it.
4. If both final and temp exist, final wins; clean temp only after final validation.
5. Never open `.migrating` as the production database.
6. Validate application ID, user version, required pragmas, and foreign keys.
7. Keep bootstrap bounded to the worker and preserve scope-epoch publication rules.
8. Expose safe state decisions to tests without exposing private media metadata.

## Invariants

- No legacy catalog input is needed for bootstrap or recovery.
- A valid final DB is never silently replaced.
- Temp state is never authoritative.
- DownloadStore, playback, and ImageCache data are untouched.
- No UI-thread SQLite or synchronous waiting.

## Focused tests

- No final DB creates an empty valid DB.
- Existing supported final DB opens without consulting `catalog.v1`.
- Stale temp-only state is rebuilt safely.
- Final plus temp leaves final authoritative.
- Corrupt/unsupported final DB follows safe failure/rebuild policy.
- Scope supersession suppresses stale bootstrap readiness.
- A test seam proves the bootstrap path does not parse the legacy catalog.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

## Commit message

```text
feat(catalog): bootstrap fresh scoped SQLite database
```

This task file authorizes exactly one commit for this task after successful validation.

## STOP conditions

Stop if bootstrap can overwrite a valid final DB, crosses scope boundaries, or
requires reading or mutating `catalog.v1`.
