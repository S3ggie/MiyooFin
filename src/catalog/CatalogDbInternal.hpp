#pragma once

#include "CatalogDb.hpp"
#include "CatalogCompatibility.hpp"
#include "CatalogDbFailureInjector.hpp"
#include "../data/MediaItem.hpp"
#include "MediaItemSql.hpp"
#include "CatalogDbSchema.hpp"
#include "CatalogDbSchemaOps.hpp"
#include "../data/CatalogPrimitives.hpp"
#include "../diagnostics/UiDiagnostics.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryClock.hpp"
#include "../../vendor/sqlite/sqlite3.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace miyoofin {

struct CatalogDb::QueryCommand {
    enum class Kind : unsigned char {
        Seasons,
        Episodes,
        MediaItemsByIds,
        DeleteMediaItemsByIds,
    };

    Kind kind;
    std::string parentId;
    std::vector<std::string> itemIds;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbHierarchyResult> result;
};

struct CatalogDb::LibraryReadCommand {
    CatalogDbJobMetadata metadata;
    std::promise<CatalogCompatibilityReadResult> result;
};

struct CatalogDb::MediaPageCommand {
    std::string type; int letter = -1; std::size_t limit = 0;
    CatalogDbMediaPageFilter filter = CatalogDbMediaPageFilter::Supported;
    CatalogDbPageCursor after; CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbMediaPageResult> result;
};

struct CatalogDb::ReconcileCommand {
    std::vector<MediaItem> series;
    bool authoritative = false;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
#ifdef MIYOOFIN_TEST_BUILD
    CatalogDbFailureSpec injection;
#endif
    CatalogDbFailureSpec failureSpec() const {
#ifdef MIYOOFIN_TEST_BUILD
        return injection;
#else
        return CatalogDbFailureSpec::disabled();
#endif
    }
    std::promise<CatalogDbReconcileResult> result;
};

struct CatalogDb::SyncStateCommand {
    bool write = false;
    bool legacyAvailable = false;
    std::int64_t legacyLastSuccessfulMs = 0;
    std::int64_t legacyLastReconcileMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
    std::uint64_t committedGeneration = 0;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbSyncState> result;
};

struct CatalogDb::LibrarySeedCommand {
    CatalogCompatibilitySeedRequest request;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
#ifdef MIYOOFIN_TEST_BUILD
    CatalogDbFailureSpec injection;
#endif
    CatalogDbFailureSpec failureSpec() const {
#ifdef MIYOOFIN_TEST_BUILD
        return injection;
#else
        return CatalogDbFailureSpec::disabled();
#endif
    }
    std::promise<CatalogCompatibilitySeedResult> result;
};

struct CatalogDb::HierarchyWriteCommand {
    MediaItem series;
    std::vector<MediaItem> seasons;
    std::map<std::string, std::vector<MediaItem>> episodesBySeason;
    std::uint64_t generation = 0;
    std::int64_t refreshMs = 0;
    bool complete = true;
    bool seasonScoped = false;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
#ifdef MIYOOFIN_TEST_BUILD
    CatalogDbFailureSpec injection;
#endif
    CatalogDbFailureSpec failureSpec() const {
#ifdef MIYOOFIN_TEST_BUILD
        return injection;
#else
        return CatalogDbFailureSpec::disabled();
#endif
    }
    std::promise<CatalogDbHierarchyWriteResult> result;
};

struct CatalogDb::MediaPageUpsertCommand {
    CatalogDbMediaPageWrite page;
    CatalogDbJobMetadata metadata;
#ifdef MIYOOFIN_TEST_BUILD
    CatalogDbFailureSpec injection;
#endif
    CatalogDbFailureSpec failureSpec() const {
#ifdef MIYOOFIN_TEST_BUILD
        return injection;
#else
        return CatalogDbFailureSpec::disabled();
#endif
    }
    std::promise<CatalogDbMediaPageUpsertResult> result;
};

struct CatalogDb::TopLevelSyncCommand {
    bool begin = false;
    bool finalize = false;
    std::uint64_t generation = 0;
    CatalogDbJobMetadata metadata;
    std::promise<CatalogDbTopLevelSyncResult> result;
};

namespace catalog_db_internal {


static inline const char *migrationDecisionName(CatalogDbMigrationDecision value)
{
    switch (value) {
    case CatalogDbMigrationDecision::CreateEmptyFinal: return "create_empty_final";
    case CatalogDbMigrationDecision::RebuildMigratingAtMigrationStart: return "rebuild_migrating";
    case CatalogDbMigrationDecision::FinalDatabaseWins: return "final_database_wins";
    case CatalogDbMigrationDecision::FinalDatabaseWinsCleanupCandidate: return "final_wins_cleanup";
    case CatalogDbMigrationDecision::PathError: return "path_error";
    }
    return "unknown";
}

static inline const char *scopeStatusName(CatalogDbScopeStatus value)
{
    switch (value) {
    case CatalogDbScopeStatus::Unconfigured: return "unconfigured";
    case CatalogDbScopeStatus::Pending: return "pending";
    case CatalogDbScopeStatus::Ready: return "ready";
    case CatalogDbScopeStatus::InvalidIdentity: return "invalid_identity";
    case CatalogDbScopeStatus::OpenFailed: return "open_failed";
    }
    return "unknown";
}

static inline const char *errorCategoryName(CatalogDbErrorCategory value)
{
    switch (value) {
    case CatalogDbErrorCategory::None: return "none";
    case CatalogDbErrorCategory::InvalidIdentity: return "invalid_identity";
    case CatalogDbErrorCategory::ScopeNotReady: return "scope_not_ready";
    case CatalogDbErrorCategory::OpenFailed: return "open_failed";
    case CatalogDbErrorCategory::WrongApplicationId: return "wrong_application_id";
    case CatalogDbErrorCategory::UnsupportedVersion: return "unsupported_version";
    case CatalogDbErrorCategory::CorruptOrIo: return "corrupt_or_io";
    case CatalogDbErrorCategory::ConfigurationFailed: return "configuration_failed";
    case CatalogDbErrorCategory::SqliteError: return "sqlite_error";
    case CatalogDbErrorCategory::Superseded: return "superseded";
    }
    return "unknown";
}

static inline const char *openStateName(CatalogDbOpenState value)
{
    switch (value) {
    case CatalogDbOpenState::NotAttempted: return "not_attempted";
    case CatalogDbOpenState::CreatedV1: return "created_v1";
    case CatalogDbOpenState::SupportedV1: return "supported_v1";
    case CatalogDbOpenState::CreatedV2: return "created_v2";
    case CatalogDbOpenState::SupportedV2: return "supported_v2";
    case CatalogDbOpenState::CreatedV3: return "created_v3";
    case CatalogDbOpenState::SupportedV3: return "supported_v3";
    case CatalogDbOpenState::WrongApplicationId: return "wrong_application_id";
    case CatalogDbOpenState::UnsupportedVersion: return "unsupported_version";
    case CatalogDbOpenState::CorruptOrIo: return "corrupt_or_io";
    }
    return "unknown";
}

static inline void catalogDiagnostic(const std::string &line)
{
    uiDiagnostics().log(std::string("[CatalogDb] ") + line);
}

static inline void catalogFinalDiagnostic(bool ready, CatalogDbScopeStatus status,
                            CatalogDbErrorCategory error,
                            CatalogDbOpenState openState)
{
    char line[256];
    std::snprintf(line, sizeof(line),
                  "final_state ready=%d scope_status=%s(%u) error_category=%s(%u) open_state=%s(%u)",
                  ready ? 1 : 0, scopeStatusName(status),
                  static_cast<unsigned>(status), errorCategoryName(error),
                  static_cast<unsigned>(error), openStateName(openState),
                  static_cast<unsigned>(openState));
    catalogDiagnostic(line);
}

constexpr unsigned char kDiagnosticsOperation = 1;
constexpr unsigned char kStatementReuseOperation = 2;
constexpr unsigned char kSqlErrorOperation = 3;
constexpr unsigned char kWriteSentinelOperation = 4;
constexpr unsigned char kReadSentinelOperation = 5;
constexpr unsigned char kSchemaDiagnosticsOperation = 6;
constexpr unsigned char kWriteSchemaMarkerOperation = 7;
constexpr unsigned char kReadSchemaMarkerOperation = 8;
constexpr unsigned char kSetSchemaMetadataOperation = 9;
constexpr unsigned char kMigrationRollbackOperation = 10;
constexpr unsigned char kMediaItemCodecOperation = 11;
constexpr unsigned char kMediaItemCollectionsOperation = 12;
constexpr unsigned char kSeedHierarchyQueryOperation = 13;
constexpr unsigned char kClearHierarchyQueryOperation = 14;
constexpr unsigned char kMediaPageQueryPlanOperation = 15;
/// Maximum number of attempts for a page transaction that encounters a
/// transient SQLite error (SQLITE_BUSY, SQLITE_LOCKED, SQLITE_FULL).
constexpr std::size_t kPageTransactionMaxAttempts = 3;

/// Pure helper: decide whether a failed page transaction should be retried.
/// Returns true when @p sqliteRc is a transient error and the attempt count
/// has not yet been exhausted.
inline bool pageTransactionShouldRetry(int sqliteRc, std::size_t attempt,
                                       std::size_t maxAttempts)
{
    if (attempt >= maxAttempts)
        return false;
    return sqliteRc == SQLITE_BUSY || sqliteRc == SQLITE_LOCKED
        || sqliteRc == SQLITE_FULL;
}

/// RAII owner for a single `sqlite3_stmt*`. Finalizes the statement in the
/// destructor (null-safe) so multi-exit write paths cannot leak prepared
/// statements. Movable but non-copyable; exactly one guard owns the
/// statement at any time, so double-finalize is impossible by construction.
///
/// Typical use with `sqlite3_prepare_v2`:
/// @code
/// ScopedSqliteStmt statement;
/// if (sqlite3_prepare_v2(db, sql, -1, statement.receive(), nullptr)
///     != SQLITE_OK) { ... }
/// sqlite3_bind_text(statement.get(), 1, ...);
/// @endcode
class ScopedSqliteStmt {
public:
    ScopedSqliteStmt() noexcept = default;
    ~ScopedSqliteStmt() { finalize(); }

    ScopedSqliteStmt(const ScopedSqliteStmt &) = delete;
    ScopedSqliteStmt &operator=(const ScopedSqliteStmt &) = delete;

    ScopedSqliteStmt(ScopedSqliteStmt &&other) noexcept
        : m_statement(other.m_statement)
    {
        other.m_statement = nullptr;
    }
    ScopedSqliteStmt &operator=(ScopedSqliteStmt &&other) noexcept
    {
        if (this != &other) {
            finalize();
            m_statement = other.m_statement;
            other.m_statement = nullptr;
        }
        return *this;
    }

    /// Borrow the raw pointer for the existing `bind*`/`step` helpers.
    sqlite3_stmt *get() const noexcept { return m_statement; }

    /// Slot for `sqlite3_prepare_v2`. Releases any currently held statement
    /// first so a repeated prepare cannot leak or double-finalize.
    sqlite3_stmt **receive() noexcept
    {
        finalize();
        m_statement = nullptr;
        return &m_statement;
    }

private:
    void finalize() noexcept
    {
        if (m_statement != nullptr)
            sqlite3_finalize(m_statement);
    }

    sqlite3_stmt *m_statement = nullptr;
};

constexpr std::size_t kMaxHierarchyQueryRows = 128;
constexpr std::size_t kMaxMetadataByIdRows = 64;
constexpr std::size_t kMaxReconcileSeries = 4096;

static inline uint64_t telemetryNowIfEnabled() noexcept
{
    return performanceTelemetry().enabledFast()
        ? TelemetryClock::monotonicUs() : 0;
}

static inline bool mediaItemRowExists(sqlite3 *db, const std::string &id, bool &exists,
                        std::string &error)
{
    sqlite3_stmt *statement = nullptr;
    const int prepareRc = sqlite3_prepare_v2(
        db, "SELECT 1 FROM media_items WHERE id=?1", -1, &statement, nullptr);
    if (prepareRc != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    const int bindRc = sqlite3_bind_text(statement, 1, id.c_str(), -1,
                                         SQLITE_TRANSIENT);
    const int stepRc = bindRc == SQLITE_OK ? sqlite3_step(statement) : bindRc;
    exists = stepRc == SQLITE_ROW;
    const int finalRc = sqlite3_finalize(statement);
    if ((stepRc != SQLITE_ROW && stepRc != SQLITE_DONE) || finalRc != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

static inline bool collectObjects(sqlite3 *db, std::set<std::string> &objects,
                    std::string &error)
{
    sqlite3_stmt *statement = nullptr;
    const int rc = sqlite3_prepare_v2(
        db,
        "SELECT type, name FROM sqlite_master "
        "WHERE type IN ('table', 'index') AND name NOT LIKE 'sqlite_%' "
        "ORDER BY type, name",
        -1, &statement, nullptr);
    if (rc != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char *type = sqlite3_column_text(statement, 0);
        const unsigned char *name = sqlite3_column_text(statement, 1);
        if (type && name) {
            objects.emplace(std::string(reinterpret_cast<const char *>(type)) +
                            ":" +
                            reinterpret_cast<const char *>(name));
        }
    }
    const int finalRc = sqlite3_finalize(statement);
    if (finalRc != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

static inline bool schemaConstraints(sqlite3 *db, bool &foreignKeyCascade,
                       bool &checkConstraints, std::string &error)
{
    if (!exec(db, "BEGIN;", error)
        || !exec(db,
                 "INSERT INTO media_items(id, kind) "
                 "VALUES('__task06_series__', 2);",
                 error)
        || !exec(db,
                 "INSERT INTO media_items(id, kind, series_id) "
                 "VALUES('__task06_season__', 3, '__task06_series__');",
                 error)
        || !exec(db,
                 "INSERT INTO media_items(id, kind, season_id) "
                 "VALUES('__task06_episode__', 4, '__task06_season__');",
                 error)
        || !exec(db,
                 "INSERT INTO item_genres(item_id, ordinal, genre) "
                 "VALUES('__task06_series__', 0, 'Test');",
                 error)
        || !exec(db,
                 "INSERT INTO item_image_tags(item_id, image_type, tag) "
                 "VALUES('__task06_series__', 'Primary', 'tag');",
                 error)
        || !exec(db,
                 "INSERT INTO hierarchy_state(series_id, complete) "
                 "VALUES('__task06_series__', 1);",
                 error)) {
        std::string ignored;
        exec(db, "ROLLBACK;", ignored);
        return false;
    }

    std::string expectedError;
    const int foreignKeyRc = execResult(
        db,
        "INSERT INTO media_items(id, kind, season_id) "
        "VALUES('__task06_bad_fk__', 4, '__task06_missing__');",
        expectedError);
    const int foreignKeyCode = sqlite3_extended_errcode(db);
    foreignKeyCascade = (foreignKeyRc == SQLITE_CONSTRAINT
                         || foreignKeyCode == SQLITE_CONSTRAINT_FOREIGNKEY)
        && foreignKeyCode == SQLITE_CONSTRAINT_FOREIGNKEY;

    expectedError.clear();
    const int checkRc = execResult(
        db,
        "INSERT INTO media_items(id, kind) "
        "VALUES('__task06_bad_check__', 99);",
        expectedError);
    checkConstraints = checkRc == SQLITE_CONSTRAINT
        || sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_CHECK;

    if (!exec(db, "DELETE FROM media_items WHERE id='__task06_series__';",
              error)) {
        std::string ignored;
        exec(db, "ROLLBACK;", ignored);
        return false;
    }
    std::int64_t remaining = 1;
    if (!scalarInt(db,
                   "SELECT (SELECT COUNT(*) FROM media_items WHERE id IN "
                   "('__task06_series__', '__task06_season__', "
                   "'__task06_episode__')) + "
                   "(SELECT COUNT(*) FROM item_genres WHERE item_id="
                   "'__task06_series__') + "
                   "(SELECT COUNT(*) FROM item_image_tags WHERE item_id="
                   "'__task06_series__') + "
                   "(SELECT COUNT(*) FROM hierarchy_state WHERE series_id="
                   "'__task06_series__');",
                   remaining, error)) {
        std::string ignored;
        exec(db, "ROLLBACK;", ignored);
        return false;
    }
    foreignKeyCascade = foreignKeyCascade && remaining == 0;
    std::string ignored;
    exec(db, "ROLLBACK;", ignored);
    return true;
}

static inline bool sameMediaItemScalars(const MediaItem &expected, const MediaItem &actual)
{
    return expected.id == actual.id && expected.type == actual.type
        && expected.title == actual.title
        && expected.overview == actual.overview && expected.year == actual.year
        && std::fabs(expected.rating - actual.rating) < 0.000001f
        && expected.etag == actual.etag && expected.played == actual.played
        && std::fabs(expected.progress - actual.progress) < 0.000001f
        && expected.playbackPositionTicks == actual.playbackPositionTicks
        && expected.indexNumber == actual.indexNumber
        && expected.parentIndexNumber == actual.parentIndexNumber
        && expected.runTimeTicks == actual.runTimeTicks
        && expected.seriesName == actual.seriesName
        && expected.seriesId == actual.seriesId
        && expected.seasonId == actual.seasonId && expected.artR == actual.artR
        && expected.artG == actual.artG && expected.artB == actual.artB;
}

static inline bool validateHierarchyInput(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::string &error)
{
    if (series.id.empty() || !series.seriesId.empty()
        || !series.seasonId.empty()) {
        error = "invalid series hierarchy root";
        return false;
    }
    if (series.type == "movie") {
        if (!seasons.empty() || !episodesBySeason.empty()) {
            error = "movie hierarchy root cannot contain children";
            return false;
        }
        return true;
    }
    if (series.type != "show") {
        error = "invalid series hierarchy root";
        return false;
    }
    std::set<std::string> seasonIds;
    std::set<std::string> itemIds;
    itemIds.insert(series.id);
    for (const auto &season : seasons) {
        if (season.id.empty() || season.type != "season"
            || season.seriesId != series.id || !season.seasonId.empty()
            || !seasonIds.insert(season.id).second
            || !itemIds.insert(season.id).second) {
            error = "invalid season hierarchy relationship";
            return false;
        }
    }
    if (episodesBySeason.size() != seasonIds.size()) {
        error = "episode map does not match returned seasons";
        return false;
    }
    for (const auto &entry : episodesBySeason) {
        if (!seasonIds.count(entry.first)) {
            error = "episode map contains an unknown season";
            return false;
        }
        for (const auto &episode : entry.second) {
            if (episode.id.empty() || episode.type != "episode"
                || episode.seriesId != series.id
                || episode.seasonId != entry.first
                || !itemIds.insert(episode.id).second) {
                error = "invalid episode hierarchy relationship";
                return false;
            }
        }
    }
    return true;
}

static inline bool validateReconcileInput(const std::vector<MediaItem> &series,
                            std::string &error)
{
    if (series.size() > kMaxReconcileSeries) {
        error = "authoritative series set exceeds bounded limit";
        return false;
    }
    std::set<std::string> ids;
    for (const auto &item : series) {
        if (item.id.empty() || item.type != "show"
            || !item.seriesId.empty() || !item.seasonId.empty()
            || !ids.insert(item.id).second) {
            error = "invalid authoritative series set";
            return false;
        }
    }
    return true;
}


} // namespace catalog_db_internal
} // namespace miyoofin
