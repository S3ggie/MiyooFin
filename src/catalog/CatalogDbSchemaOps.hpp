#pragma once

#include "../../vendor/sqlite/sqlite3.h"
#include "CatalogDb.hpp"
#include "CatalogDbSchema.hpp"

namespace miyoofin {
namespace catalog_db_internal {

std::string scopeDirectory(const std::string &scopeKey);
std::string catalogPath(const std::string &scopeKey);
std::string migratingPath(const std::string &scopeKey);
bool makeDirectories(const std::string &path);
bool validScopeIdentity(const std::string &serverUrl, const std::string &userId);

struct MigrationPathPresence {
    bool present = false;
    bool error = false;
};

MigrationPathPresence inspectMigrationPath(const std::string &path);
CatalogDbMigrationState inspectMigrationState(const std::string &scopeKey);

struct ScalarValue {
    std::string value;
};

int scalarCallback(void *context, int columnCount, char **values, char **columns);
bool scalar(sqlite3 *db, const char *sql, std::string &value, std::string &error);
bool exec(sqlite3 *db, const char *sql, std::string &error);
bool scalarInt(sqlite3 *db, const char *sql, std::int64_t &value,
               std::string &error);
int execResult(sqlite3 *db, const char *sql, std::string &error);

bool ensureSchema(sqlite3 *db, CatalogDbOpenState &openState,
                  bool &needsBackfill, std::string &error);
bool backfillOrganizationalSortKeys(sqlite3 *db, std::string &error);
bool maintainOrganizationalSortKey(sqlite3 *db, const MediaItem &item,
                                   std::string &error);

} // namespace catalog_db_internal
} // namespace miyoofin
