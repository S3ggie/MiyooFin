# Catalog Schema V1 — Hierarchy-First

Schema v1 intentionally covers the **OfflineCatalog replacement** only. LibraryCache/Home tables are introduced later as schema v2 after the hierarchy-only hardware gate. This is sequencing, not a change to the approved final architecture.

## Design rules

- one normalized `media_items` table;
- Jellyfin item IDs are primary keys;
- no integer surrogate media IDs;
- no downloaded-byte state;
- no artwork bytes;
- genres and image tags remain structured, not JSON blobs;
- nullable relationship columns are indexed;
- schema must support series, season, episode, and movie kinds even though phase-A population is hierarchy-first;
- FK enforcement is enabled;
- DB is scoped per server/user via the existing scope key.

## Kind values

Stable application enum stored as INTEGER:

```text
1 = movie
2 = series
3 = season
4 = episode
```

Do not persist current display strings (`movie`, `show`, `season`, `episode`) as the schema discriminator.

## Tables

```sql
CREATE TABLE media_items (
    id                       TEXT PRIMARY KEY NOT NULL,
    kind                     INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 4),
    title                    TEXT NOT NULL DEFAULT '',
    overview                 TEXT NOT NULL DEFAULT '',
    production_year          INTEGER NOT NULL DEFAULT 0,
    community_rating         REAL NOT NULL DEFAULT 0.0,
    etag                     TEXT NOT NULL DEFAULT '',
    played                   INTEGER NOT NULL DEFAULT 0 CHECK(played IN (0,1)),
    progress                 REAL NOT NULL DEFAULT 0.0,
    playback_position_ticks  INTEGER NOT NULL DEFAULT 0,
    index_number             INTEGER NOT NULL DEFAULT 0,
    parent_index_number      INTEGER NOT NULL DEFAULT 0,
    runtime_ticks            INTEGER NOT NULL DEFAULT 0,
    series_name              TEXT NOT NULL DEFAULT '',
    series_id                TEXT,
    season_id                TEXT,
    art_r                    INTEGER NOT NULL DEFAULT 128 CHECK(art_r BETWEEN 0 AND 255),
    art_g                    INTEGER NOT NULL DEFAULT 128 CHECK(art_g BETWEEN 0 AND 255),
    art_b                    INTEGER NOT NULL DEFAULT 128 CHECK(art_b BETWEEN 0 AND 255),
    FOREIGN KEY(series_id) REFERENCES media_items(id) ON DELETE CASCADE,
    FOREIGN KEY(season_id) REFERENCES media_items(id) ON DELETE CASCADE
);
```

Insertion order for complete hierarchy writes is:

```text
series -> seasons -> episodes
```

If real legacy data reveals a valid relationship shape incompatible with these FKs, STOP at Task 08/15 and review the schema. Do not disable FKs to force migration through.

```sql
CREATE TABLE item_genres (
    item_id   TEXT NOT NULL,
    ordinal   INTEGER NOT NULL CHECK(ordinal >= 0),
    genre     TEXT NOT NULL,
    PRIMARY KEY(item_id, ordinal),
    FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE
);
```

`MediaItem::genre` is reconstructed from `ordinal=0`; do not duplicate it.

```sql
CREATE TABLE item_image_tags (
    item_id     TEXT NOT NULL,
    image_type  TEXT NOT NULL,
    tag         TEXT NOT NULL,
    PRIMARY KEY(item_id, image_type),
    FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE
);
```

```sql
CREATE TABLE hierarchy_state (
    series_id          TEXT PRIMARY KEY NOT NULL,
    complete           INTEGER NOT NULL CHECK(complete IN (0,1)),
    last_refresh_ms    INTEGER NOT NULL DEFAULT 0,
    last_generation    INTEGER NOT NULL DEFAULT 0,
    FOREIGN KEY(series_id) REFERENCES media_items(id) ON DELETE CASCADE
);
```

`complete=1` means every discovered level for that series was fetched and atomically committed. It distinguishes an empty-but-complete series from undiscovered data.

```sql
CREATE TABLE sync_state (
    singleton_id           INTEGER PRIMARY KEY CHECK(singleton_id = 1),
    last_successful_ms      INTEGER NOT NULL DEFAULT 0,
    last_reconcile_ms       INTEGER NOT NULL DEFAULT 0,
    committed_generation    INTEGER NOT NULL DEFAULT 0
);
```

Create singleton row `1` when the schema is initialized. It remains unused as authoritative state until Task 25.

## Indexes

```sql
CREATE INDEX idx_media_series_kind_order
ON media_items(series_id, kind, index_number, id);

CREATE INDEX idx_media_season_order
ON media_items(season_id, index_number, id);
```

These cover the core phase-A query shapes:

```sql
-- seasons
SELECT ...
FROM media_items
WHERE series_id=?1 AND kind=3
ORDER BY index_number, title, id;

-- episodes
SELECT ...
FROM media_items
WHERE season_id=?1 AND kind=4
ORDER BY index_number, title, id;
```

Do not add indexes for rating, overview, runtime, played, or other columns without an actual query-plan need.

## Metadata

Use:

```sql
PRAGMA application_id = <approved-noncolliding-ID>;
PRAGMA user_version = 1;
```

`application_id` is a file identity.  
`user_version` is the schema migration version.

## Authoritative subtree replacement

A complete series write must:

1. UPSERT series;
2. UPSERT every returned season;
3. UPSERT every returned episode;
4. replace genre/tag child rows for each updated item;
5. delete episodes absent from each authoritative returned season;
6. delete seasons absent from the authoritative series response;
7. let FK cascades remove children;
8. mark hierarchy state complete;
9. commit.

Never infer deletion from an incomplete network fetch.

## Future schema v2

Only after CP-F, add:

```text
library_views
library_membership
home_items
```

and any exact organizational-sort support needed to reproduce Home semantics.

Do not preemptively put LibraryCache/Home ownership into schema v1.
