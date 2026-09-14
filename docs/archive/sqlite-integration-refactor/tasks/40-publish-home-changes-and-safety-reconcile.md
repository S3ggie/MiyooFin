# Task 40 — Publish relevant Home changes and retain safety reconciliation

**Phase:** E — incremental synchronization

## Objective

Publish only Home-relevant catalog changes to the visible Home presentation,
show `HOME SYNCING...` only while such a publication is active, and retain a
low-frequency authoritative reconciliation as the missed-event safety net.

## Why

The catalog can update in the background without disturbing the user’s
current screen.  Home should redraw when one of its rails is affected, while
unrelated library events remain invisible to Home.  A daily/reconnect
reconciliation guarantees eventual correctness after downtime or event loss.

## Preconditions

- Tasks 35–39 pass and are committed.
- Existing local-first Home publication, bounded paging, artwork scheduling,
  selection preservation, and cancellation behavior remain intact.

## Allowed Files

- `src/ui/screens/HomeScreen.*`
- `src/ui/HomeSyncState.*`
- `src/library/LibrarySync.*`
- `src/app/App.*` only for session-scope lifecycle wiring required by the
  existing background sync owner
- `tests/cases/test_misc_regressions.inc`
- `tests/cases/test_ui_foundation.inc`
- `tests/cases/test_cache_offline.inc`
- This task file

## Forbidden Scope

- Do not block SDL/UI on event processing, HTTP, SQLite, filesystem work,
  retry sleeps, or worker joins.
- Do not replace cached Home content with an empty result after a transient
  failure.
- Do not rebuild Home for unrelated library events.
- Do not download every artwork image during a reconciliation.
- Do not alter playback, downloads, schema, CatalogCompatibility, or
  DownloadStore authority.

## Required behavior

1. Cached Home content renders immediately when available.
2. A new/updated/deleted item causes Home publication only when it can affect
   Recently Added, Continue Watching, or a currently visible catalog rail.
3. Relevant updates display `HOME SYNCING...` in the existing bottom-right
   status area only while the update is pending/applied.
4. Unrelated events do not move focus, reset scroll position, or show the
   Home status.
5. Existing cached artwork remains visible; ImageCache downloads only missing
   or changed image keys.
6. On startup after a disconnected period, or after event-stream loss, the
   app performs bounded catch-up and then an authoritative reconciliation when
   required.
7. A periodic full reconciliation runs at a low cadence, approximately once
   per day, rather than every few minutes.  Reconnect-triggered reconciliation
   is allowed when the event stream was unavailable.
8. Continue Watching remains a user-state refresh, not an assumption that a
   `LibraryChanged` event contains playback progress.

## Method

1. Add focused tests for relevant Home insertion/update/removal, unrelated
   event suppression, status lifetime, cached-content preservation, focus and
   scroll stability, artwork-cache reuse, reconnect catch-up, and daily
   reconciliation scheduling.
2. Verify the tests fail for the missing relevance/publishing policy.
3. Implement the minimum presentation/state change while retaining existing
   worker and supersession patterns.
4. Run desktop tests with a clean remakeable runtime data set while preserving
   `output/desktop-runtime/session.txt` so the saved login remains available.
5. If a mandatory physical Miyoo run is added by the implementation evidence,
   use the established developer SSH/deploy workflow and record it separately;
   do not claim hardware validation from the desktop run.

## Validation

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
make onionos
make verify-arm
make refactor-check
git diff --check
```

## STOP conditions

- Home must wait on network/SQLite before displaying valid cached content.
- A relevant update can reset valid selection/focus or expose partial data.
- The safety reconciliation requires frequent full artwork downloads.
- The change requires work outside **Allowed Files**, schema v4, another
  worker/connection, or a compatibility deletion.

## Commit boundary

```text
feat(home): publish incremental library updates
```

## Checkpoint

**CP-E — Incremental synchronization. STOP after Task 40. Do not begin
another task until the event, catch-up, deletion, Home, and safety-sync
evidence has been reviewed. Original SQLite Task 34 remains PAUSED.**
