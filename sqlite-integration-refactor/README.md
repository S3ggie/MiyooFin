# SQLite Integration Refactor Roadmap

This roadmap corrects the SQLite integration architecture audited on branch `audit/sqlite-integration-current` at snapshot `e96d685405c195a170dbc09ff2db0e897662e5b9`.

It is a **pre-Task-34** roadmap. The original `sqlite-migration/tasks/34-retire-legacy-catalog-cache-sync-persistence.md` is explicitly **PAUSED**. Do not execute, amend, or partially implement original Task 34 while this roadmap is in progress. After Task 33 below, Task 34 is still paused until a separate human review explicitly authorizes legacy retirement.

## Purpose

Keep the good SQLite foundation, but fix ownership and source-of-truth problems before removing legacy persistence.

The target direction is:

```text
App / Session
      |
      v
LibrarySync
   /       \
JellyfinApi  CatalogDb
                  |
                  v
               SQLite
                  |
                  v
             LibraryQuery
          /       |       \
       Home   Series/Episodes   Download planner

DownloadStore
      |
      v
OfflineLibraryQuery
      |
      v
UI
```

Names may change only when the current code supports a clearly smaller equivalent. The ownership rules do not change:

- one component owns Jellyfin -> SQLite synchronization;
- `CatalogDb` owns SQLite persistence/query execution on its existing single worker;
- screens consume domain/query results instead of owning database synchronization;
- `DownloadStore` remains authoritative for physical offline availability;
- App/session lifecycle owns CatalogDb scope configure/deconfigure;
- HomeScreen must not own top-level database synchronization.

## Audit findings this roadmap resolves

The roadmap is ordered around the concrete audit failures:

1. Continue Watching / Recently Added failures currently share control flow with catalog failures and can truncate later library views.
2. Bounded top-level page writes are not an authoritative generation: stale views/memberships can survive on warm databases.
3. Top-level reads select historical media rows by kind before membership, allowing stale unreferenced items to remain browseable.
4. Warm SQLite data exists but Home still waits for network requests and a new page commit before becoming useful.
5. HomeScreen has a CatalogDb scope fallback even though App already owns scope lifecycle.
6. Offline root presentation still depends on legacy `LibrarySnapshot`/LibraryCache behavior and can disagree with DownloadStore availability.
7. `home_items` and live network Home rails form competing half-authorities.
8. HomeScreen owns too much synchronization/persistence orchestration.
9. CatalogDb has upward dependencies on cache/network/download/UI concerns.
10. Series, EpisodeBrowser, and DownloadManager duplicate DB -> network -> DB hierarchy policy.

## Architecture that must be preserved

This is a correction of integration boundaries, **not** a database rewrite.

- schema v3 stays the baseline;
- one CatalogDb worker and one SQLite connection stay the baseline;
- indexed/keyset bounded reads stay;
- the atomic fresh-database temporary-file bootstrap/promotion stays;
- DownloadStore and its download architecture stay;
- ImageCache stays;
- blocking SQLite/network/large filesystem work stays off the SDL thread;
- existing scope epoch/cancellation safety is preserved and centralized rather than discarded.

A task must STOP if it discovers that one of these needs redesign. Do not casually broaden a task to replace an architecture that already passed hardware validation.

## Deliberate Phase-A reconciliation design

Top-level authoritative reconciliation uses **worker-owned TEMP staging tables** under schema v3:

```text
begin generation N
    |
    +--> stage bounded view/member pages
    |       canonical media metadata may be upserted page-by-page
    |       live membership remains generation N-1
    |
    +--> success: one transaction publishes staged views/memberships
    |
    +--> failure/cancel: discard TEMP stage
                         live generation N-1 remains valid
```

TEMP staging is intentionally process-local. An interrupted process cannot leave a partially published membership generation, and no schema-v4 migration is needed merely to repair top-level reconciliation.

## Home rail authority

Continue Watching and Recently Added are intentionally **ephemeral network presentation state** in this roadmap. They are not required to make a warm Movies/Shows catalog usable. The schema-v3 `home_items` table is retained only for compatibility/test bridges until a later explicit retirement decision; normal runtime must not treat it as a competing authority.

## Offline authority

Offline availability comes from DownloadStore/DownloadManager state. CatalogDb supplies canonical metadata when available. `DownloadItem` metadata supplies the minimum fallback when canonical metadata is absent. A legacy LibraryCache snapshot is not required for offline root browsing.

## Playback exclusion

The intermittent audio-only FFplay/mmiyoo display handoff defect is a separate subsystem issue. This roadmap must not repair or redesign playback. Validation tasks may avoid playback entirely unless an existing unrelated regression test runs as part of `make test`.

## Phases and checkpoints

| Phase | Tasks | Goal | Checkpoint |
|---|---:|---|---|
| A — correctness | 01-09 | Repair failure isolation, authoritative membership generations, warm reads, scope ownership, offline roots | **CP-A after Task 09** |
| B — ownership/modularity | 10-17 | Extract LibrarySync/LibraryQuery ownership and narrow CatalogDb/screens | **CP-B after Task 17** |
| C — validation | 18-31 | Lock every required cold/warm/membership/offline/cancellation scenario and validate real Miyoo correctness | **CP-C after Task 31** |
| D — performance rebaseline | 32-33 | Fill measurement gaps and rebaseline on real Miyoo without speculative optimization | **CP-D after Task 33** |

Ordinary tasks inside a phase proceed one at a time without asking for confirmation after every successful commit. Stop only at the checkpoint at the end of each phase, a task-specific hardware gate, or a genuine STOP condition.

## Task index

- [01 — Isolate optional Home rail failures](tasks/01-isolate-optional-home-rail-failures.md)
- [02 — Add begin/abort top-level sync staging](tasks/02-begin-abort-top-level-sync-generation.md)
- [03 — Stage top-level media pages without live membership mutation](tasks/03-stage-media-pages-without-live-membership-mutation.md)
- [04 — Finalize authoritative top-level generations atomically](tasks/04-finalize-authoritative-top-level-generation.md)
- [05 — Make top-level media reads membership-authoritative](tasks/05-membership-authoritative-media-page-reads.md)
- [06 — Make Home rails explicitly ephemeral](tasks/06-make-home-rails-explicitly-ephemeral.md)
- [07 — Publish warm SQLite Home before network refresh](tasks/07-publish-warm-sqlite-home-before-network-refresh.md)
- [08 — Enforce one CatalogDb scope owner](tasks/08-enforce-single-catalog-scope-owner.md)
- [09 — Build offline root from DownloadStore plus catalog metadata](tasks/09-offline-root-downloadstore-plus-catalog-metadata.md)
- [10 — Extract the top-level LibrarySync owner](tasks/10-extract-librarysync-top-level-owner.md)
- [11 — Introduce the LibraryQuery domain owner](tasks/11-introduce-libraryquery-domain-owner.md)
- [12 — Make Home consume LibrarySync and LibraryQuery](tasks/12-home-consumes-librarysync-and-libraryquery.md)
- [13 — Isolate LibrarySnapshot compatibility bridge](tasks/13-isolate-librarysnapshot-compatibility-bridge.md)
- [14 — Remove CachedLibraryView reconstruction from SQLite membership handling](tasks/14-remove-cachedlibraryview-membership-reconstruction.md)
- [15 — Narrow CatalogDb to SQLite persistence/query execution](tasks/15-narrow-catalogdb-dependencies.md)
- [16 — Unify Series and EpisodeBrowser refresh policy](tasks/16-unify-series-episode-refresh-policy.md)
- [17 — Unify DownloadManager hierarchy policy](tasks/17-unify-download-planner-hierarchy-policy.md)
- [18 — Validate fresh database behavior](tasks/18-validate-fresh-database.md)
- [19 — Validate existing warm database behavior](tasks/19-validate-existing-warm-database.md)
- [20 — Validate item removal from a view](tasks/20-validate-item-removed-from-view.md)
- [21 — Validate deleted library view](tasks/21-validate-view-deleted.md)
- [22 — Validate item moved between libraries](tasks/22-validate-item-moved-between-libraries.md)
- [23 — Validate multiple TV libraries](tasks/23-validate-multiple-tv-libraries.md)
- [24 — Validate Anime membership and artwork metadata](tasks/24-validate-anime-membership-and-artwork.md)
- [25 — Validate Playlist mixed with supported views](tasks/25-validate-playlist-mixed-with-supported-views.md)
- [26 — Validate Continue Watching failure](tasks/26-validate-continue-watching-failure.md)
- [27 — Validate Recently Added failure](tasks/27-validate-recently-added-failure.md)
- [28 — Validate cancellation halfway through population](tasks/28-validate-mid-population-cancellation.md)
- [29 — Validate offline startup without LibraryCache](tasks/29-validate-offline-without-librarycache.md)
- [30 — Validate exit during synchronization](tasks/30-validate-exit-during-synchronization.md)
- [31 — Validate cold/warm correctness on real Miyoo](tasks/31-real-miyoo-cold-warm-correctness-validation.md)
- [32 — Fill performance timeline instrumentation gaps](tasks/32-fill-performance-timeline-instrumentation-gaps.md)
- [33 — Rebaseline SQLite integration performance on real Miyoo](tasks/33-real-miyoo-performance-rebaseline.md)

## Checkpoint acceptance

### CP-A — Correctness foundation

Before Phase B, confirm that optional rails cannot truncate catalog synchronization, successful top-level generations atomically replace membership, failed/cancelled generations retain the prior committed set, top-level reads require current supported membership, warm SQLite data is usable before network refresh, Home cannot reconfigure the DB scope, and offline roots no longer require LibraryCache.

### CP-B — Ownership/modularity

Before Phase C, confirm that Home no longer owns Jellyfin -> SQLite synchronization, LibrarySync is the one synchronization owner, LibraryQuery is the domain read owner, CatalogDb no longer reaches upward into networking/UI/download/cache orchestration, and Series/EpisodeBrowser/DownloadManager use the shared hierarchy policy.

### CP-C — Hardware correctness

Before performance work, a physical Miyoo Mini Plus must pass fresh, warm, multiple-TV/Anime, offline-without-LibraryCache, and exit-during-sync scenarios. ARM compilation is not a substitute for device evidence.

### CP-D — Performance rebaseline

Task 33 records the post-refactor performance evidence. **Passing CP-D does not authorize original SQLite Task 34.** Legacy retirement remains a separate explicit decision.

## Completion definition

This roadmap is complete only when Task 33 and CP-D are complete. At that point, the project should have a coherent SQLite-first runtime architecture, but legacy artifacts are still retained until the separately paused Task 34 is reconsidered.

Do not begin original Task 34 automatically.
