# Task 15 — Jellyfin initial population and reconciliation

## Execution mode — roadmap override

Implement directly with the selected GPT-5.6 Luna High. Do not spawn subagents
or create orchestration artifacts. One task equals one narrow commit; preserve
all other repository safety, threading, and validation rules.

## Goal

Populate a fresh or existing scoped CatalogDb from current Jellyfin metadata and
make authoritative hierarchy reconciliation safe, bounded, and retryable. The
database starts as an empty/unverified catalog; it never depends on the contents
or shape of `catalog.v1`.

## Depends On

- Task 14

## Allowed Files

- CatalogDb hierarchy/reconciliation files under `src/catalog/`
- Narrow Jellyfin-to-CatalogDb worker/API seams required for initial population
- Focused tests

## Forbidden Scope

- No `catalog.v1` parsing/import/parity requirement.
- No DownloadStore offline reconstruction yet.
- No production screen/consumer switch.
- No permanent dual-write.

## Exact implementation requirements

1. Accept current Jellyfin series/season/episode metadata only through bounded
   worker-side operations.
2. Persist complete returned subtrees transactionally using the existing DAL.
3. Replace stale children only when the Jellyfin response is authoritative and
   complete for that scope.
4. Keep `hierarchy_state.complete=0` for fresh/unverified/partial state.
5. Set it to `1` only after the complete Jellyfin generation commits successfully.
6. Preserve ordered genres, image tags, playback fields, and canonical IDs from
   the current Jellyfin response.
7. On network failure or cancellation, preserve the last committed DB state and
   leave the checkpoint conservative.
8. Reconciliation must be safe to repeat after a process interruption.

## Invariants

- Jellyfin is authoritative for server metadata/hierarchy.
- No network call or SQLite operation runs on the SDL/UI thread.
- Incomplete responses never cause authoritative deletion or completion.
- Downloaded bytes and durable download metadata remain separate.
- No old-vs-new catalog parity is required.

## Focused tests

- Fresh empty DB followed by complete Jellyfin population.
- Repeatable idempotent reconciliation.
- Partial/network-failed response leaves state incomplete.
- Authoritative deletion removes stale hierarchy without touching downloads.
- Generation failure leaves the old checkpoint.
- Process interruption retry preserves the last committed subtree.
- Scope supersession prevents stale publication.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

## Commit message

```text
feat(catalog): populate hierarchy from Jellyfin
```

This task file authorizes exactly one commit for this task after successful validation.

## STOP conditions

Stop if current Jellyfin responses cannot identify authoritative completeness,
if partial data would be deleted/marked complete, or if DownloadStore ownership
would need to change.
