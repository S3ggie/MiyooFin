#ifndef MIYOOFIN_CATALOG_DB_SCHEMA_HPP
#define MIYOOFIN_CATALOG_DB_SCHEMA_HPP

#include <array>
#include <cstdint>

namespace miyoofin {

// Shared by CatalogDb and the isolated benchmark so schema/workload results
// cannot silently drift from the production database contract.
inline constexpr std::int64_t kCatalogApplicationId = 0x4D59464E;

inline constexpr std::array<const char *, 8> kCatalogSchemaStatements = {{
    "CREATE TABLE media_items ("
    "id TEXT PRIMARY KEY NOT NULL,"
    "kind INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 4),"
    "title TEXT NOT NULL DEFAULT '',"
    "overview TEXT NOT NULL DEFAULT '',"
    "production_year INTEGER NOT NULL DEFAULT 0,"
    "community_rating REAL NOT NULL DEFAULT 0.0,"
    "etag TEXT NOT NULL DEFAULT '',"
    "played INTEGER NOT NULL DEFAULT 0 CHECK(played IN (0,1)),"
    "progress REAL NOT NULL DEFAULT 0.0,"
    "playback_position_ticks INTEGER NOT NULL DEFAULT 0,"
    "index_number INTEGER NOT NULL DEFAULT 0,"
    "parent_index_number INTEGER NOT NULL DEFAULT 0,"
    "runtime_ticks INTEGER NOT NULL DEFAULT 0,"
    "series_name TEXT NOT NULL DEFAULT '',"
    "series_id TEXT,"
    "season_id TEXT,"
    "art_r INTEGER NOT NULL DEFAULT 128 CHECK(art_r BETWEEN 0 AND 255),"
    "art_g INTEGER NOT NULL DEFAULT 128 CHECK(art_g BETWEEN 0 AND 255),"
    "art_b INTEGER NOT NULL DEFAULT 128 CHECK(art_b BETWEEN 0 AND 255),"
    "FOREIGN KEY(series_id) REFERENCES media_items(id) ON DELETE CASCADE,"
    "FOREIGN KEY(season_id) REFERENCES media_items(id) ON DELETE CASCADE"
    ");",
    "CREATE TABLE item_genres ("
    "item_id TEXT NOT NULL,"
    "ordinal INTEGER NOT NULL CHECK(ordinal >= 0),"
    "genre TEXT NOT NULL,"
    "PRIMARY KEY(item_id, ordinal),"
    "FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE"
    ");",
    "CREATE TABLE item_image_tags ("
    "item_id TEXT NOT NULL,"
    "image_type TEXT NOT NULL,"
    "tag TEXT NOT NULL,"
    "PRIMARY KEY(item_id, image_type),"
    "FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE"
    ");",
    "CREATE TABLE hierarchy_state ("
    "series_id TEXT PRIMARY KEY NOT NULL,"
    "complete INTEGER NOT NULL CHECK(complete IN (0,1)),"
    "last_refresh_ms INTEGER NOT NULL DEFAULT 0,"
    "last_generation INTEGER NOT NULL DEFAULT 0,"
    "FOREIGN KEY(series_id) REFERENCES media_items(id) ON DELETE CASCADE"
    ");",
    "CREATE TABLE sync_state ("
    "singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),"
    "last_successful_ms INTEGER NOT NULL DEFAULT 0,"
    "last_reconcile_ms INTEGER NOT NULL DEFAULT 0,"
    "committed_generation INTEGER NOT NULL DEFAULT 0"
    ");",
    "CREATE INDEX idx_media_series_kind_order "
    "ON media_items(series_id, kind, index_number, id);",
    "CREATE INDEX idx_media_season_order "
    "ON media_items(season_id, index_number, id);",
    "INSERT INTO sync_state(singleton_id) VALUES(1);",
}};

} // namespace miyoofin

#endif // MIYOOFIN_CATALOG_DB_SCHEMA_HPP
