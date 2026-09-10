# Task 16 — DownloadStore offline hierarchy reconstruction

## Execution mode — roadmap override

Implement directly with the selected GPT-5.6 Luna High. Do not spawn subagents
or create orchestration artifacts. One task equals one narrow commit; preserve
all other repository safety, threading, and validation rules.

## Goal

Ensure a first launch with no network remains useful when complete downloads
exist. Use authoritative DownloadStore/DownloadManager durable metadata to insert
only the minimum series, season, episode, or movie metadata needed for offline
browsing. Do not import `catalog.v1`, and do not make the catalog authoritative
for downloaded bytes, manifests, playback state, or download state.

## Depends On

- Task 15

## Allowed Files

- CatalogDb offline reconstruction files under `src/catalog/`
- Narrow read-only DownloadStore/DownloadManager metadata seams under `src/download/`
- Focused tests

## Forbidden Scope

- No `catalog.v1` parsing/import or legacy full-catalog parity.
- No DownloadStore format, media-byte, or playback-state redesign.
- No network requirement for offline reconstruction.
- No production consumer cutover yet.

## Exact implementation requirements

1. Enumerate complete downloaded items from authoritative durable download metadata
   without reading media bytes on the UI thread.
2. Insert the minimum valid hierarchy required to browse complete downloads;
   synthesize only containers whose IDs and parent metadata are present.
3. Preserve downloaded item IDs, names, season/episode ordering, and resume fields
   where DownloadStore metadata provides them.
4. Keep reconstructed hierarchy incomplete/unverified until Jellyfin completes a
   successful authoritative reconciliation.
5. If parent metadata is unavailable, preserve direct DownloadManager local
   visibility/playability rather than inventing an ambiguous hierarchy.
6. Rebuilds and repeated offline launches are deterministic and idempotent.
7. Catalog rebuild/recovery never deletes download records or media bytes.

## Invariants

- DownloadStore/DownloadManager remain authoritative for local content and state.
- Offline complete downloads remain browsable/playable without network where the
  durable metadata supplies a valid hierarchy.
- No SQLite state claims full server synchronization from offline reconstruction.
- No UI-thread SQLite or blocking media scan.

## Focused tests

- Complete downloaded episode with valid series/season metadata is visible offline.
- Complete downloaded movie remains visible without hierarchy parents.
- Missing parent metadata does not invent IDs or break local playback.
- Resume/progress metadata survives reconstruction.
- Repeated reconstruction is idempotent.
- Catalog rebuild leaves DownloadStore fixtures unchanged.
- No-network first launch keeps complete-download visibility.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

## Commit message

```text
feat(catalog): reconstruct offline hierarchy from downloads
```

This task file authorizes exactly one commit for this task after successful validation.

## STOP conditions

Stop if offline hierarchy requires guessing a parent, if download/playback
authority must move into CatalogDb, or if complete downloads disappear offline.
