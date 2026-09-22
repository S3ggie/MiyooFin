#include "CatalogDbSchemaOps.hpp"

#include "../data/CatalogPrimitives.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace miyoofin {
namespace catalog_db_internal {

std::string scopeDirectory(const std::string& scopeKey)
{
    return std::string("cache/library/") + scopeKey;
}

bool makeDirectories(const std::string& path)
{
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string directory = path.substr(0, i);
            if (!directory.empty() && ::mkdir(directory.c_str(), 0755) && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

std::string catalogPath(const std::string& scopeKey)
{
    return catalog::catalogPath("cache", scopeKey);
}

std::string migratingPath(const std::string& scopeKey)
{
    return catalog::migratingCatalogPath("cache", scopeKey);
}

bool validScopeIdentity(const std::string& serverUrl, const std::string& userId)
{
    const std::string normalizedUrl = catalog::normalizeIdentityUrl(serverUrl);
    const std::size_t schemeEnd = normalizedUrl.find("://");
    return schemeEnd != std::string::npos && schemeEnd + 3 < normalizedUrl.size() &&
           userId.find_first_not_of(" \t\r\n") != std::string::npos;
}

MigrationPathPresence inspectMigrationPath(const std::string& path)
{
    struct stat information
    {};
    if (::stat(path.c_str(), &information) == 0) {
        return {true, false};
    }
    return {false, errno != ENOENT};
}

CatalogDbMigrationState inspectMigrationState(const std::string& scopeKey)
{
    const MigrationPathPresence final = inspectMigrationPath(catalogPath(scopeKey));
    const MigrationPathPresence migrating = inspectMigrationPath(migratingPath(scopeKey));

    CatalogDbMigrationState state;
    state.finalPresent = final.present;
    state.migratingPresent = migrating.present;
    state.pathError = final.error || migrating.error;
    if (state.pathError) {
        state.files = CatalogDbMigrationFileState::PathError;
        state.decision = CatalogDbMigrationDecision::PathError;
        return state;
    }

    const unsigned mask = (state.finalPresent ? 2u : 0u) | (state.migratingPresent ? 4u : 0u);
    switch (mask) {
    case 0:
        state.files = CatalogDbMigrationFileState::NoFiles;
        state.decision = CatalogDbMigrationDecision::CreateEmptyFinal;
        break;
    case 2:
        state.files = CatalogDbMigrationFileState::FinalOnly;
        state.decision = CatalogDbMigrationDecision::FinalDatabaseWins;
        break;
    case 4:
        state.files = CatalogDbMigrationFileState::MigratingOnly;
        state.decision = CatalogDbMigrationDecision::RebuildMigratingAtMigrationStart;
        break;
    case 6:
        state.files = CatalogDbMigrationFileState::FinalAndMigrating;
        state.decision = CatalogDbMigrationDecision::FinalDatabaseWinsCleanupCandidate;
        break;
    }
    state.finalWins = state.finalPresent;
    state.migratingCleanupCandidate = state.finalPresent && state.migratingPresent;
    return state;
}

int scalarCallback(void* context, int columnCount, char** values, char**)
{
    if (columnCount > 0 && values[0]) {
        static_cast<ScalarValue*>(context)->value = values[0];
    }
    return 0;
}

bool scalar(sqlite3* db, const char* sql, std::string& value, std::string& error)
{
    ScalarValue result;
    char* sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, scalarCallback, &result, &sqliteError);
    if (rc != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(db);
        sqlite3_free(sqliteError);
        return false;
    }
    value = result.value;
    return true;
}

bool exec(sqlite3* db, const char* sql, std::string& error)
{
    char* sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqliteError);
    if (rc != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(db);
        sqlite3_free(sqliteError);
        return false;
    }
    return true;
}

bool scalarInt(sqlite3* db, const char* sql, std::int64_t& value, std::string& error)
{
    std::string text;
    if (!scalar(db, sql, text, error)) {
        return false;
    }
    char* end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        error = "SQLite returned a non-integer scalar";
        return false;
    }
    value = parsed;
    return true;
}

int execResult(sqlite3* db, const char* sql, std::string& error)
{
    char* sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqliteError);
    if (rc != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(db);
        sqlite3_free(sqliteError);
    }
    return rc;
}

bool ensureSchema(sqlite3* db, CatalogDbOpenState& openState, bool& needsBackfill,
                  std::string& error)
{
    needsBackfill = false;
    std::int64_t applicationId = 0;
    std::int64_t userVersion = 0;
    if (!scalarInt(db, "PRAGMA application_id;", applicationId, error) ||
        !scalarInt(db, "PRAGMA user_version;", userVersion, error)) {
        openState = CatalogDbOpenState::CorruptOrIo;
        return false;
    }
    if (applicationId == kCatalogApplicationId && userVersion == 3) {
        openState = CatalogDbOpenState::SupportedV3;
        return true;
    }
    if (applicationId == kCatalogApplicationId && userVersion == 2) {
        needsBackfill = true;
        if (!exec(db, "BEGIN IMMEDIATE;", error))
            return false;
        const char* const migration[] = {
            "ALTER TABLE media_items ADD COLUMN organizational_sort_key TEXT NOT NULL DEFAULT '';",
            "CREATE INDEX idx_media_movie_sort ON media_items(kind, organizational_sort_key, "
            "title, id);",
            "CREATE INDEX idx_media_show_sort ON media_items(kind, organizational_sort_key, title, "
            "id);",
        };
        for (const char* statement : migration) {
            if (!exec(db, statement, error)) {
                std::string ignored;
                exec(db, "ROLLBACK;", ignored);
                return false;
            }
        }
        if (!exec(db, "PRAGMA user_version = 3;", error) || !exec(db, "COMMIT;", error)) {
            std::string ignored;
            exec(db, "ROLLBACK;", ignored);
            return false;
        }
        openState = CatalogDbOpenState::SupportedV3;
        return true;
    }
    if (applicationId == kCatalogApplicationId && userVersion == 1) {
        needsBackfill = true;
        if (!exec(db, "BEGIN IMMEDIATE;", error))
            return false;
        const char* const migration[] = {
            "CREATE TABLE library_views ("
            "id TEXT PRIMARY KEY NOT NULL,"
            "name TEXT NOT NULL DEFAULT '',"
            "collection_type TEXT NOT NULL DEFAULT '',"
            "ordinal INTEGER NOT NULL CHECK(ordinal >= 0)"
            ");",
            "CREATE TABLE library_membership ("
            "view_id TEXT NOT NULL, item_id TEXT NOT NULL,"
            "ordinal INTEGER NOT NULL CHECK(ordinal >= 0),"
            "PRIMARY KEY(view_id, item_id),"
            "FOREIGN KEY(view_id) REFERENCES library_views(id) ON DELETE CASCADE,"
            "FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE"
            ");",
            "CREATE TABLE home_items ("
            "row_kind TEXT NOT NULL, item_id TEXT NOT NULL,"
            "ordinal INTEGER NOT NULL CHECK(ordinal >= 0),"
            "PRIMARY KEY(row_kind, item_id),"
            "FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE"
            ");",
            "CREATE INDEX idx_library_views_ordinal ON library_views(ordinal, id);",
            "CREATE INDEX idx_library_membership_view_order ON library_membership(view_id, "
            "ordinal, item_id);",
            "CREATE INDEX idx_library_membership_item ON library_membership(item_id, view_id);",
            "CREATE INDEX idx_home_items_row_order ON home_items(row_kind, ordinal, item_id);",
            "ALTER TABLE media_items ADD COLUMN organizational_sort_key TEXT NOT NULL DEFAULT '';",
            "CREATE INDEX idx_media_movie_sort ON media_items(kind, organizational_sort_key, "
            "title, id);",
            "CREATE INDEX idx_media_show_sort ON media_items(kind, organizational_sort_key, title, "
            "id);",
        };
        for (const char* statement : migration) {
            if (!exec(db, statement, error)) {
                std::string ignored;
                exec(db, "ROLLBACK;", ignored);
                return false;
            }
        }
        if (!exec(db, "PRAGMA user_version = 3;", error) || !exec(db, "COMMIT;", error)) {
            std::string ignored;
            exec(db, "ROLLBACK;", ignored);
            return false;
        }
        openState = CatalogDbOpenState::SupportedV2;
        return true;
    }
    if (applicationId != 0 && applicationId != kCatalogApplicationId) {
        openState = CatalogDbOpenState::WrongApplicationId;
        error = "catalog database application ID is not recognized";
        return false;
    }
    if (applicationId != 0 || userVersion != 0) {
        openState = CatalogDbOpenState::UnsupportedVersion;
        error = "catalog database schema version is not supported";
        return false;
    }

    if (!exec(db, "BEGIN IMMEDIATE;", error)) {
        return false;
    }
    // A newly-created schema has no pre-existing rows to canonicalize.
    for (const char* statement : kCatalogSchemaStatements) {
        if (!exec(db, statement, error)) {
            std::string ignored;
            exec(db, "ROLLBACK;", ignored);
            return false;
        }
    }
    const std::string applicationIdPragma =
        "PRAGMA application_id = " + std::to_string(kCatalogApplicationId) + ";";
    if (!exec(db, applicationIdPragma.c_str(), error) ||
        !exec(db, "PRAGMA user_version = 3;", error) || !exec(db, "COMMIT;", error)) {
        std::string ignored;
        exec(db, "ROLLBACK;", ignored);
        return false;
    }
    openState = CatalogDbOpenState::CreatedV3;
    return true;
}

bool backfillOrganizationalSortKeys(sqlite3* db, std::string& error)
{
    sqlite3_stmt* read = nullptr;
    sqlite3_stmt* write = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, title FROM media_items", -1, &read, nullptr) !=
            SQLITE_OK ||
        sqlite3_prepare_v2(db, "UPDATE media_items SET organizational_sort_key=?1 WHERE id=?2", -1,
                           &write, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        sqlite3_finalize(read);
        sqlite3_finalize(write);
        return false;
    }
    while (sqlite3_step(read) == SQLITE_ROW) {
        const char* id = reinterpret_cast<const char*>(sqlite3_column_text(read, 0));
        const char* title = reinterpret_cast<const char*>(sqlite3_column_text(read, 1));
        sqlite3_reset(write);
        sqlite3_clear_bindings(write);
        if (!id ||
            sqlite3_bind_text(write, 1, catalog::organizationalSortKey(title ? title : "").c_str(),
                              -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_bind_text(write, 2, id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(write) != SQLITE_DONE) {
            error = sqlite3_errmsg(db);
            sqlite3_finalize(read);
            sqlite3_finalize(write);
            return false;
        }
    }
    sqlite3_finalize(read);
    sqlite3_finalize(write);
    return true;
}

bool maintainOrganizationalSortKey(sqlite3* db, const MediaItem& item, std::string& error)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE media_items SET organizational_sort_key=?1 WHERE id=?2", -1,
                           &statement, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    const std::string key = catalog::organizationalSortKey(item.title);
    const bool ok =
        sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(statement, 2, item.id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_DONE;
    if (!ok)
        error = sqlite3_errmsg(db);
    sqlite3_finalize(statement);
    return ok;
}

} // namespace catalog_db_internal
} // namespace miyoofin
