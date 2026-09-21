#include "CatalogDb.hpp"
#include "CatalogDbInternal.hpp"

#include "CatalogCompatibility.hpp"
#include "../data/MediaItem.hpp"
#include "MediaItemSql.hpp"
#include "CatalogDbSchema.hpp"
#include "../data/CatalogPrimitives.hpp"
#include "../diagnostics/UiDiagnostics.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryClock.hpp"
#include "../../vendor/sqlite/sqlite3.h"

#include <cassert>
#include <algorithm>
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
#ifdef MIYOOFIN_TEST_BUILD
namespace miyoofin {
using namespace catalog_db_internal;

struct CatalogDb::TestCommand {
    unsigned char operation;
    std::string value;
    std::promise<CatalogDbTestResult> result;
};







CatalogDbEnqueueResult CatalogDb::enqueueNoopForTest(
    CatalogDbPriority priority,
    const CatalogDbJobMetadata &metadata)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            return CatalogDbEnqueueResult::RejectedStopping;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            return CatalogDbEnqueueResult::RejectedCancelled;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            return CatalogDbEnqueueResult::RejectedFull;
        }
        m_queues[priorityIndex(priority)].push_back(
            {priority, metadata, telemetryNowIfEnabled()});
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return CatalogDbEnqueueResult::Accepted;
}

CatalogDbConnectionState CatalogDb::connectionStateForTest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_connectionOpen, m_connectionWorkerOwned,
            m_preparedStatementCount};
}

CatalogDbTestResult CatalogDb::runSqliteDiagnosticsForTest()
{
    return runTestCommand(kDiagnosticsOperation);
}

CatalogDbTestResult CatalogDb::runStatementReuseForTest()
{
    return runTestCommand(kStatementReuseOperation);
}

CatalogDbTestResult CatalogDb::runSqlErrorForTest()
{
    return runTestCommand(kSqlErrorOperation);
}

CatalogDbTestResult CatalogDb::runSchemaDiagnosticsForTest()
{
    return runTestCommand(kSchemaDiagnosticsOperation);
}

CatalogDbTestResult CatalogDb::writeSchemaMarkerForTest(const std::string &value)
{
    return runTestCommand(kWriteSchemaMarkerOperation, value);
}

CatalogDbTestResult CatalogDb::readSchemaMarkerForTest()
{
    return runTestCommand(kReadSchemaMarkerOperation);
}

CatalogDbTestResult CatalogDb::setSchemaMetadataForTest(
    std::int64_t applicationId, std::int64_t userVersion)
{
    return runTestCommand(kSetSchemaMetadataOperation,
                          std::to_string(applicationId) + ":"
                              + std::to_string(userVersion));
}

CatalogDbTestResult CatalogDb::runMigrationRollbackForTest()
{
    return runTestCommand(kMigrationRollbackOperation);
}

CatalogDbTestResult CatalogDb::runMediaItemCodecForTest()
{
    return runTestCommand(kMediaItemCodecOperation);
}

CatalogDbTestResult CatalogDb::runMediaItemCollectionsForTest()
{
    return runTestCommand(kMediaItemCollectionsOperation);
}

CatalogDbTestResult CatalogDb::seedHierarchyQueryFixturesForTest()
{
    return runTestCommand(kSeedHierarchyQueryOperation);
}

CatalogDbTestResult CatalogDb::clearHierarchyQueryFixturesForTest()
{
    return runTestCommand(kClearHierarchyQueryOperation);
}

CatalogDbTestResult CatalogDb::runMediaPageQueryPlanForTest(
    const std::string &type, int alphabetLetter)
{
    if (type != "movie" && type != "show") {
        CatalogDbTestResult result;
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = "invalid media page query-plan type";
        return result;
    }
    return runTestCommand(kMediaPageQueryPlanOperation,
                          type + ":" + std::to_string(alphabetLetter));
}

CatalogDbTestResult CatalogDb::writeSentinelForTest(const std::string &value)
{
    return runTestCommand(kWriteSentinelOperation, value);
}

CatalogDbTestResult CatalogDb::readSentinelForTest()
{
    return runTestCommand(kReadSentinelOperation);
}

CatalogDbEnqueueResult CatalogDb::enqueueScopedNoopForTest(
    CatalogDbPriority priority)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
        performanceTelemetry().addCatalogDbEnqueueRejected();
        return CatalogDbEnqueueResult::RejectedStopping;
    }
    if (!m_scopeConfigured || !m_scopeReady) {
        performanceTelemetry().addCatalogDbEnqueueRejected();
        return CatalogDbEnqueueResult::RejectedScopeNotReady;
    }
    if (m_pendingJobs >= kMaxPendingJobs) {
        performanceTelemetry().addCatalogDbEnqueueRejected();
        return CatalogDbEnqueueResult::RejectedFull;
    }
    CatalogDbJobMetadata metadata;
    metadata.generation = m_generation;
    metadata.scopeEpoch = m_requestedEpoch;
    m_queues[priorityIndex(priority)].push_back(
        {priority, metadata, telemetryNowIfEnabled()});
    ++m_pendingJobs;
    performanceTelemetry().setCatalogDbQueueDepth(
        static_cast<uint32_t>(m_pendingJobs));
    m_wake.notify_one();
    return CatalogDbEnqueueResult::Accepted;
}

bool CatalogDb::canPublishForTest(std::uint64_t scopeEpoch) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return scopeEpoch != 0 && scopeEpoch == m_requestedEpoch
        && m_scopeConfigured && m_scopeReady;
}

void CatalogDb::setGenerationForTest(std::uint64_t generation)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generation = generation;
    }
    m_wake.notify_one();
}

void CatalogDb::setWorkerPausedForTest(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pausedForTest = paused;
    }
    m_wake.notify_one();
}

bool CatalogDb::waitForIdleForTest(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_idle.wait_for(lock, timeout, [this] {
        return m_pendingJobs == 0 && m_scopeCommands.empty() && !m_runningJob;
    });
}

std::vector<CatalogDbJobReport> CatalogDb::jobReportsForTest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_jobReports.begin(), m_jobReports.end()};
}

void CatalogDb::processTestCommand(const std::shared_ptr<TestCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbTestResult result;
    result.workerOwned = std::this_thread::get_id() == m_worker.get_id();
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        command->result.set_value(std::move(result));
        return;
    }

    auto prepareNamed = [&](const char *name, const char *sql,
                            sqlite3_stmt *&statement) {
        auto existing = m_statements.find(name);
        if (existing != m_statements.end()) {
            statement = existing->second;
            return true;
        }
        const int rc = sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr);
        if (rc != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            return false;
        }
        m_statements.emplace(name, statement);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_preparedStatementCount = m_statements.size();
        }
        return true;
    };

    auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };

    switch (command->operation) {
    case kDiagnosticsOperation:
        if (!scalar(m_db, "PRAGMA foreign_keys;", result.foreignKeys,
                    result.message)
            || !scalar(m_db, "PRAGMA trusted_schema;", result.trustedSchema,
                       result.message)
            || !scalar(m_db, "PRAGMA journal_mode;", result.journalMode,
                       result.message)
            || !scalar(m_db, "PRAGMA synchronous;", result.synchronous,
                       result.message)
            || !scalar(m_db, "PRAGMA locking_mode;", result.lockingMode,
                       result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            break;
        }
        result.success = result.foreignKeys == "1"
            && result.trustedSchema == "0"
            && result.journalMode == "delete"
            && result.synchronous == "2"
            && result.lockingMode == "normal";
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "connection pragmas do not match the baseline";
        }
        break;

    case kSchemaDiagnosticsOperation: {
        std::set<std::string> objects;
        if (!collectObjects(m_db, objects, result.message)
            || !scalarInt(m_db, "PRAGMA application_id;",
                          result.applicationId, result.message)
            || !scalarInt(m_db, "PRAGMA user_version;", result.userVersion,
                          result.message)
            || !scalar(m_db,
                       "SELECT singleton_id FROM sync_state "
                       "WHERE singleton_id=1;",
                       result.sentinel, result.message)
            || !schemaConstraints(m_db, result.foreignKeyCascade,
                                  result.checkConstraints, result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            break;
        }
        const std::set<std::string> expectedObjects = {
            "index:idx_media_movie_sort",
            "index:idx_media_show_sort",
            "index:idx_media_season_order",
            "index:idx_media_series_kind_order",
            "table:hierarchy_state",
            "table:item_genres",
            "table:item_image_tags",
            "table:media_items",
            "table:sync_state",
            "index:idx_home_items_row_order",
            "index:idx_library_membership_item",
            "index:idx_library_membership_view_order",
            "index:idx_library_views_ordinal",
            "table:home_items",
            "table:library_membership",
            "table:library_views",
        };
        result.exactSchema = objects == expectedObjects;
        result.singletonSeeded = result.sentinel == "1";
        result.success = result.exactSchema
            && result.foreignKeyCascade && result.checkConstraints
            && result.singletonSeeded
            && result.applicationId == kCatalogApplicationId
            && result.userVersion == 3;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "schema v3 diagnostics did not match the contract";
        }
        break;
    }

    case kStatementReuseOperation: {
        sqlite3_stmt *first = nullptr;
        if (!prepareNamed("test_scalar", "SELECT 1", first)) {
            break;
        }
        int rc = sqlite3_step(first);
        reset(first);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            break;
        }
        sqlite3_stmt *second = nullptr;
        if (!prepareNamed("test_scalar", "SELECT 1", second)) {
            break;
        }
        reset(second);
        result.statementReused = first == second;
        result.success = result.statementReused;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "statement registry did not reuse the statement";
        }
        break;
    }

    case kSqlErrorOperation: {
        sqlite3_stmt *statement = nullptr;
        const int rc = sqlite3_prepare_v2(
            m_db, "SELECT * FROM catalog_task05_missing_table", -1,
            &statement, nullptr);
        int stepRc = rc;
        if (rc == SQLITE_OK) {
            stepRc = sqlite3_step(statement);
        }
        if (statement) {
            sqlite3_finalize(statement);
        }
        const bool sqliteFailed = stepRc != SQLITE_OK && stepRc != SQLITE_ROW
            && stepRc != SQLITE_DONE;
        result.success = false;
        result.error = sqliteFailed ? CatalogDbErrorCategory::SqliteError
                                    : CatalogDbErrorCategory::ConfigurationFailed;
        result.message = sqliteFailed ? sqlite3_errmsg(m_db)
                                      : "expected SQLite error was not returned";
        break;
    }

    case kMediaItemCodecOperation: {
        sqlite3_stmt *insert = nullptr;
        sqlite3_stmt *select = nullptr;
        if (!prepareNamed(
                "media_item_scalar_insert",
                "INSERT OR REPLACE INTO media_items("
                "id, kind, title, overview, production_year, community_rating,"
                "etag, played, progress, playback_position_ticks, index_number,"
                "parent_index_number, runtime_ticks, series_name, series_id,"
                "season_id, art_r, art_g, art_b) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                "?13, ?14, ?15, ?16, ?17, ?18, ?19)",
                insert)
            || !prepareNamed(
                "media_item_scalar_select",
                "SELECT id, kind, title, overview, production_year, "
                "community_rating, etag, played, progress, "
                "playback_position_ticks, index_number, parent_index_number, "
                "runtime_ticks, series_name, series_id, season_id, art_r, "
                "art_g, art_b FROM media_items WHERE id=?1",
                select)) {
            break;
        }

        auto reset = [](sqlite3_stmt *statement) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        };
        auto roundTrip = [&](const MediaItem &expected,
                             bool nullableRelationships) {
            MediaItemSqlError bindError = MediaItemSqlError::None;
            if (!bindMediaItemScalars(insert, expected, bindError)) {
                result.message = "MediaItem scalar bind failed";
                reset(insert);
                return false;
            }
            if (nullableRelationships
                && (sqlite3_bind_null(insert, 15) != SQLITE_OK
                    || sqlite3_bind_null(insert, 16) != SQLITE_OK)) {
                result.message = "MediaItem nullable bind failed";
                reset(insert);
                return false;
            }
            const int insertRc = sqlite3_step(insert);
            reset(insert);
            if (insertRc != SQLITE_DONE) {
                result.message = expected.id + ": " + sqlite3_errmsg(m_db);
                return false;
            }

            if (sqlite3_bind_text(select, 1, expected.id.c_str(), -1,
                                  SQLITE_TRANSIENT) != SQLITE_OK) {
                result.message = "MediaItem scalar lookup bind failed";
                reset(select);
                return false;
            }
            const int selectRc = sqlite3_step(select);
            if (selectRc != SQLITE_ROW) {
                result.message = expected.id + ": " + sqlite3_errmsg(m_db);
                reset(select);
                return false;
            }
            MediaItem actual;
            MediaItemSqlError readError = MediaItemSqlError::None;
            const bool readOk = readMediaItemScalars(select, actual, readError);
            reset(select);
            if (!readOk) {
                result.message = expected.id + ": scalar read failed";
                return false;
            }
            if (!sameMediaItemScalars(expected, actual)) {
                result.message = expected.id + ": scalar comparison failed";
                return false;
            }
            return true;
        };

        MediaItem series;
        series.id = "__task08_series__";
        series.type = "show";
        series.title = "Full Series";
        series.overview = "Series overview";
        series.year = 2024;
        series.rating = 8.25f;
        series.etag = "series-etag";
        series.played = true;
        series.progress = 0.75f;
        series.playbackPositionTicks = 123456789;
        series.indexNumber = 7;
        series.parentIndexNumber = 8;
        series.runTimeTicks = 987654321;
        series.seriesName = "Series name";
        series.placeholderArtwork.red = 10;
        series.placeholderArtwork.green = 20;
        series.placeholderArtwork.blue = 30;

        MediaItem season = series;
        season.id = "__task08_season__";
        season.type = "season";
        season.seriesId = series.id;
        season.title = "Full Season";

        MediaItem episode = season;
        episode.id = "__task08_episode__";
        episode.type = "episode";
        episode.seasonId = season.id;
        episode.title = "Full Episode";

        MediaItem movie = series;
        movie.id = "__task08_movie__";
        movie.type = "movie";
        movie.seriesName.clear();
        movie.seriesId.clear();
        movie.seasonId.clear();
        movie.title = "Full Movie";

        result.codecPopulatedKinds = roundTrip(series, false)
            && roundTrip(season, false) && roundTrip(episode, false)
            && roundTrip(movie, false);

        MediaItem defaults;
        defaults.id = "__task08_defaults__";
        defaults.type = "movie";
        result.codecDefaults = roundTrip(defaults, true);
        result.codecNullableRelationships = result.codecDefaults
            && defaults.seriesId.empty() && defaults.seasonId.empty();

        MediaItem boundaries;
        boundaries.id = "__task08_boundaries__";
        boundaries.type = "episode";
        boundaries.year = std::numeric_limits<int>::min();
        boundaries.rating = 10.0f;
        boundaries.progress = 1.0f;
        boundaries.playbackPositionTicks = std::numeric_limits<long long>::min();
        boundaries.indexNumber = std::numeric_limits<int>::min();
        boundaries.parentIndexNumber = std::numeric_limits<int>::max();
        boundaries.runTimeTicks = std::numeric_limits<long long>::max();
        boundaries.placeholderArtwork.red = 0;
        boundaries.placeholderArtwork.green = 1;
        boundaries.placeholderArtwork.blue = 255;
        result.codecBoundaries = roundTrip(boundaries, false);

        MediaItem invalidKind;
        invalidKind.id = "__task08_invalid_kind__";
        invalidKind.type = "unknown";
        MediaItemSqlError invalidError = MediaItemSqlError::None;
        MediaItemSqlError invalidReadError = MediaItemSqlError::None;
        result.codecInvalidKind = !bindMediaItemScalars(insert, invalidKind,
                                                         invalidError)
            && invalidError == MediaItemSqlError::InvalidKind
            && mediaItemKindFromSql(99, invalidReadError).empty()
            && invalidReadError == MediaItemSqlError::InvalidKind;
        reset(insert);

        MediaItem missingId;
        missingId.type = "movie";
        MediaItemSqlError missingError = MediaItemSqlError::None;
        result.codecMissingId = !bindMediaItemScalars(insert, missingId,
                                                       missingError)
            && missingError == MediaItemSqlError::MissingId;
        reset(insert);

        exec(m_db, "DELETE FROM media_items WHERE id LIKE '__task08_%';",
             result.message);
        result.success = result.codecPopulatedKinds && result.codecDefaults
            && result.codecBoundaries && result.codecInvalidKind
            && result.codecMissingId && result.codecNullableRelationships;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            if (result.message.empty()) {
                result.message = "MediaItem scalar codec checks failed";
            }
        }
        break;
    }

    case kMediaItemCollectionsOperation: {
        sqlite3_stmt *insert = nullptr;
        sqlite3_stmt *select = nullptr;
        sqlite3_stmt *deleteGenres = nullptr;
        sqlite3_stmt *insertGenre = nullptr;
        sqlite3_stmt *deleteImageTags = nullptr;
        sqlite3_stmt *insertImageTag = nullptr;
        sqlite3_stmt *selectGenres = nullptr;
        sqlite3_stmt *selectImageTags = nullptr;
        sqlite3_stmt *deleteItem = nullptr;
        if (!prepareNamed(
                "media_item_scalar_insert",
                "INSERT INTO media_items("
                "id, kind, title, overview, production_year, community_rating,"
                "etag, played, progress, playback_position_ticks, index_number,"
                "parent_index_number, runtime_ticks, series_name, series_id,"
                "season_id, art_r, art_g, art_b) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                "?13, ?14, ?15, ?16, ?17, ?18, ?19) "
                "ON CONFLICT(id) DO UPDATE SET "
                "kind=excluded.kind, title=excluded.title, "
                "overview=excluded.overview, "
                "production_year=excluded.production_year, "
                "community_rating=excluded.community_rating, "
                "etag=excluded.etag, played=excluded.played, "
                "progress=excluded.progress, "
                "playback_position_ticks=excluded.playback_position_ticks, "
                "index_number=excluded.index_number, "
                "parent_index_number=excluded.parent_index_number, "
                "runtime_ticks=excluded.runtime_ticks, "
                "series_name=excluded.series_name, "
                "series_id=excluded.series_id, "
                "season_id=excluded.season_id, "
                "art_r=excluded.art_r, art_g=excluded.art_g, "
                "art_b=excluded.art_b",
                insert)
            || !prepareNamed(
                "media_item_scalar_select",
                "SELECT id, kind, title, overview, production_year, "
                "community_rating, etag, played, progress, "
                "playback_position_ticks, index_number, parent_index_number, "
                "runtime_ticks, series_name, series_id, season_id, art_r, "
                "art_g, art_b FROM media_items WHERE id=?1",
                select)
            || !prepareNamed(
                "media_item_genres_delete",
                "DELETE FROM item_genres WHERE item_id=?1", deleteGenres)
            || !prepareNamed(
                "media_item_genre_insert",
                "INSERT INTO item_genres(item_id, ordinal, genre) "
                "VALUES(?1, ?2, ?3)", insertGenre)
            || !prepareNamed(
                "media_item_image_tags_delete",
                "DELETE FROM item_image_tags WHERE item_id=?1",
                deleteImageTags)
            || !prepareNamed(
                "media_item_image_tag_insert",
                "INSERT INTO item_image_tags(item_id, image_type, tag) "
                "VALUES(?1, ?2, ?3)", insertImageTag)
            || !prepareNamed(
                "media_item_genres_select",
                "SELECT ordinal, genre FROM item_genres WHERE item_id=?1 "
                "ORDER BY ordinal", selectGenres)
            || !prepareNamed(
                "media_item_image_tags_select",
                "SELECT image_type, tag FROM item_image_tags WHERE item_id=?1 "
                "ORDER BY image_type", selectImageTags)
            || !prepareNamed(
                "media_item_delete",
                "DELETE FROM media_items WHERE id=?1", deleteItem)) {
            break;
        }

        const MediaItemCollectionStatements collections{
            deleteGenres, insertGenre, deleteImageTags, insertImageTag,
            selectGenres, selectImageTags};
        auto reset = [](sqlite3_stmt *statement) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        };
        auto rollback = [&] {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
        };
        auto insertAndRead = [&](const MediaItem &expected) {
            if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)) {
                return false;
            }
            MediaItemSqlError error = MediaItemSqlError::None;
            reset(insert);
            if (!bindMediaItemScalars(insert, expected, error)
                || sqlite3_step(insert) != SQLITE_DONE) {
                result.message = sqlite3_errmsg(m_db);
                reset(insert);
                rollback();
                return false;
            }
            reset(insert);
            if (!replaceMediaItemCollections(collections, expected, error)) {
                result.message = "MediaItem collection replacement failed";
                rollback();
                return false;
            }
            if (!exec(m_db, "COMMIT;", result.message)) {
                rollback();
                return false;
            }

            reset(select);
            if (sqlite3_bind_text(select, 1, expected.id.c_str(), -1,
                                  SQLITE_TRANSIENT) != SQLITE_OK
                || sqlite3_step(select) != SQLITE_ROW) {
                result.message = sqlite3_errmsg(m_db);
                reset(select);
                return false;
            }
            MediaItem actual;
            MediaItemSqlError readError = MediaItemSqlError::None;
            const bool readOk = readMediaItemScalars(select, actual, readError);
            reset(select);
            if (!readOk || !readMediaItemCollections(collections, actual,
                                                     readError)) {
                result.message = "MediaItem collection read failed";
                return false;
            }
            return mediaItemsEquivalentForCatalog(expected, actual);
        };

        MediaItem item;
        item.id = "__task09_item__";
        item.type = "movie";
        item.title = "Collection Fixture";

        result.collectionsZero = insertAndRead(item);

        item.genres = {"Drama", "Science Fiction", "Thriller"};
        item.genre = item.genres.front();
        item.imageTags = {{"Backdrop", "backdrop-tag"},
                          {"Primary", "primary-tag"},
                          {"Thumb", "thumb-tag"}};
        result.collectionsMultiple = insertAndRead(item);

        MediaItem updated = item;
        updated.genres = {"Updated"};
        updated.genre = updated.genres.front();
        updated.imageTags = {{"Logo", "logo-new"},
                             {"Primary", "primary-new"}};
        result.collectionsUpdateRemoval = insertAndRead(updated);
        if (result.collectionsUpdateRemoval) {
            result.collectionsUpdateRemoval = updated.genres.size() == 1
                && updated.genre == updated.genres.front();
        }

        // Empty imageTags must preserve previously stored tags rather than
        // wiping them.  A transient server payload returning "ImageTags": {}
        // must not destroy artwork metadata.
        MediaItem emptyTags = updated;
        emptyTags.imageTags.clear();
        if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)) {
            result.collectionsEmptyTagPreserve = false;
        } else {
            MediaItemSqlError error = MediaItemSqlError::None;
            reset(insert);
            if (!bindMediaItemScalars(insert, emptyTags, error)
                || sqlite3_step(insert) != SQLITE_DONE) {
                result.message = sqlite3_errmsg(m_db);
                reset(insert);
                rollback();
                result.collectionsEmptyTagPreserve = false;
            } else {
                reset(insert);
                if (!replaceMediaItemCollections(collections, emptyTags,
                                                 error)) {
                    result.message =
                        "MediaItem collection replacement failed (empty)";
                    rollback();
                    result.collectionsEmptyTagPreserve = false;
                } else if (!exec(m_db, "COMMIT;", result.message)) {
                    rollback();
                    result.collectionsEmptyTagPreserve = false;
                } else {
                    reset(select);
                    const bool bound = sqlite3_bind_text(
                        select, 1, emptyTags.id.c_str(), -1,
                        SQLITE_TRANSIENT) == SQLITE_OK;
                    const bool row = bound
                        && sqlite3_step(select) == SQLITE_ROW;
                    MediaItem readBack;
                    MediaItemSqlError readErr = MediaItemSqlError::None;
                    const bool readOk = row
                        && readMediaItemScalars(select, readBack, readErr);
                    const bool collOk = readOk
                        && readMediaItemCollections(collections, readBack,
                                                    readErr);
                    reset(select);
                    // After the fix, empty imageTags must leave the existing
                    // tags intact: the two tags from the previous write above.
                    result.collectionsEmptyTagPreserve = collOk
                        && readBack.imageTags.size() == 2
                        && readBack.imageTags.count("Logo")
                        && readBack.imageTags.at("Logo") == "logo-new"
                        && readBack.imageTags.count("Primary")
                        && readBack.imageTags.at("Primary") == "primary-new"
                        && readBack.genres.size() == 1
                        && readBack.genres[0] == "Updated";
                }
            }
        }

        result.collectionsParity = result.collectionsZero
            && result.collectionsMultiple && result.collectionsUpdateRemoval
            && result.collectionsEmptyTagPreserve;

        if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)) {
            result.collectionsDeleteCascade = false;
        } else {
            reset(deleteItem);
            const bool deleteOk = sqlite3_bind_text(
                                    deleteItem, 1, item.id.c_str(), -1,
                                    SQLITE_TRANSIENT) == SQLITE_OK
                && sqlite3_step(deleteItem) == SQLITE_DONE;
            reset(deleteItem);
            if (!deleteOk) {
                result.message = sqlite3_errmsg(m_db);
                rollback();
                result.collectionsDeleteCascade = false;
            } else if (!exec(m_db, "COMMIT;", result.message)) {
                rollback();
                result.collectionsDeleteCascade = false;
            } else {
                MediaItem deleted;
                deleted.id = item.id;
                MediaItemSqlError readError = MediaItemSqlError::None;
                const bool childrenEmpty = readMediaItemCollections(
                    collections, deleted, readError)
                    && deleted.genres.empty() && deleted.genre.empty()
                    && deleted.imageTags.empty();
                std::int64_t remaining = 1;
                const bool rowGone = scalarInt(
                    m_db,
                    "SELECT COUNT(*) FROM media_items "
                    "WHERE id='__task09_item__';",
                    remaining, result.message) && remaining == 0;
                result.collectionsDeleteCascade = childrenEmpty && rowGone;
            }
        }

        result.success = result.collectionsParity
            && result.collectionsDeleteCascade;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            if (result.message.empty()) {
                result.message = "MediaItem collection checks failed";
            }
        }
        break;
    }

    case kSeedHierarchyQueryOperation: {
        if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)
            || !exec(m_db,
                     "DELETE FROM media_items WHERE id LIKE '__task10_%';",
                     result.message)
            || !exec(m_db,
                     "INSERT INTO media_items(id, kind, title) VALUES"
                     "('__task10_series__', 2, 'Target Series'),"
                     "('__task10_other_series__', 2, 'Other Series');",
                     result.message)
            || !exec(m_db,
                     "INSERT INTO media_items(id, kind, title, series_id, "
                     "index_number) VALUES"
                     "('__task10_season_z__', 3, 'Zed', "
                     "'__task10_series__', 1),"
                     "('__task10_season_b__', 3, 'Beta', "
                     "'__task10_series__', 2),"
                     "('__task10_season_a__', 3, 'Alpha', "
                     "'__task10_series__', 1);",
                     result.message)
            || !exec(m_db,
                     "INSERT INTO media_items(id, kind, title, season_id, "
                     "series_id, index_number) VALUES"
                     "('__task10_episode_z__', 4, 'Zed Episode', "
                     "'__task10_season_a__', '__task10_series__', 1),"
                     "('__task10_episode_a__', 4, 'Alpha Episode', "
                     "'__task10_season_a__', '__task10_series__', 1),"
                     "('__task10_episode_b__', 4, 'Beta Episode', "
                     "'__task10_season_a__', '__task10_series__', 2);",
                     result.message)) {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
            break;
        }
        for (int index = 0; index < 200; ++index) {
            const std::string statement =
                "INSERT INTO media_items(id, kind, title, series_id, "
                "index_number) VALUES('__task10_other_season_"
                + std::to_string(index) + "', 3, 'Other', "
                "'__task10_other_series__', " + std::to_string(index) + ");";
            if (!exec(m_db, statement.c_str(), result.message)) {
                std::string ignored;
                exec(m_db, "ROLLBACK;", ignored);
                break;
            }
            if (index == 199) {
                result.hierarchyFixture = true;
            }
        }
        if (!result.hierarchyFixture) {
            break;
        }
        if (!exec(m_db,
                  "INSERT INTO item_genres(item_id, ordinal, genre) "
                  "VALUES('__task10_season_a__', 0, 'Drama'),"
                  "('__task10_season_a__', 1, 'Mystery');",
                  result.message)
            || !exec(m_db,
                     "INSERT INTO item_image_tags(item_id, image_type, tag) "
                     "VALUES('__task10_season_a__', 'Primary', 'season-tag'),"
                     "('__task10_episode_a__', 'Primary', 'episode-tag');",
                     result.message)) {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
            result.hierarchyFixture = false;
            break;
        }

        auto planUsesIndex = [&](const char *sql, const char *indexName) {
            sqlite3_stmt *statement = nullptr;
            if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr)
                != SQLITE_OK) {
                return false;
            }
            bool found = false;
            while (sqlite3_step(statement) == SQLITE_ROW) {
                const unsigned char *detail = sqlite3_column_text(statement, 3);
                if (detail && std::string(reinterpret_cast<const char *>(detail))
                                  .find(indexName) != std::string::npos) {
                    found = true;
                }
            }
            sqlite3_finalize(statement);
            return found;
        };
        result.hierarchyIndexes = planUsesIndex(
            "EXPLAIN QUERY PLAN SELECT id FROM media_items "
            "WHERE series_id='__task10_series__' AND kind=3 "
            "ORDER BY index_number, title, id;",
            "idx_media_series_kind_order")
            && planUsesIndex(
                "EXPLAIN QUERY PLAN SELECT id FROM media_items "
                "WHERE season_id='__task10_season_a__' AND kind=4 "
                "ORDER BY index_number, title, id;",
                "idx_media_season_order");
        if (!exec(m_db, "COMMIT;", result.message)) {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
            break;
        }
        result.success = result.hierarchyFixture && result.hierarchyIndexes;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "hierarchy query index plan check failed";
        }
        break;
    }

    case kClearHierarchyQueryOperation:
        result.success = exec(
            m_db, "DELETE FROM media_items WHERE id LIKE '__task10_%';",
            result.message);
        break;

    case kMediaPageQueryPlanOperation: {
        const std::size_t separator = command->value.find(':');
        if (separator == std::string::npos) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "invalid media page query-plan arguments";
            break;
        }
        const std::string type = command->value.substr(0, separator);
        int letter = -1;
        try {
            letter = std::stoi(command->value.substr(separator + 1));
        } catch (...) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "invalid media page query-plan alphabet";
            break;
        }
        if ((type != "movie" && type != "show") || letter < -1 || letter > 25) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "invalid media page query-plan arguments";
            break;
        }
        const int kind = type == "movie" ? 1 : 2;
        std::string sql =
            "EXPLAIN QUERY PLAN SELECT id FROM media_items INDEXED BY "
            + ("idx_media_" + type + "_sort")
            + " WHERE kind=" + std::to_string(kind)
            + " AND EXISTS (SELECT 1 FROM library_membership "
              "JOIN library_views ON library_views.id=library_membership.view_id "
              "WHERE library_membership.item_id=media_items.id "
              "AND library_views.collection_type='"
            + (type == "movie" ? "movies" : "tvshows") + "')";
        if (letter >= 0) {
            sql += " AND organizational_sort_key >= '";
            sql += static_cast<char>('a' + letter);
            sql += "' AND organizational_sort_key < '";
            sql += static_cast<char>('a' + letter + 1);
            sql += "'";
        }
        sql += " ORDER BY organizational_sort_key, title, id LIMIT 25;";
        sqlite3_stmt *plan = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &plan, nullptr)
            != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            break;
        }
        bool sawTempSort = false;
        while (sqlite3_step(plan) == SQLITE_ROW) {
            const unsigned char *detail = sqlite3_column_text(plan, 3);
            if (!detail) continue;
            const std::string text(reinterpret_cast<const char *>(detail));
            if (text.find("idx_media_" + type + "_sort") != std::string::npos)
                result.mediaPageUsesSortIndex = true;
            if (text.find("USE TEMP B-TREE") != std::string::npos)
                sawTempSort = true;
        }
        sqlite3_finalize(plan);
        result.mediaPageAvoidsTempSort = !sawTempSort;
        result.success = result.mediaPageUsesSortIndex
            && result.mediaPageAvoidsTempSort;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "bounded media page query plan is not indexed";
        }
        break;
    }

    case kWriteSentinelOperation: {
        if (!exec(m_db,
                  "CREATE TABLE IF NOT EXISTS catalog_task05_sentinel(value TEXT NOT NULL)",
                  result.message)
            || !exec(m_db, "DELETE FROM catalog_task05_sentinel",
                     result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            break;
        }
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed("sentinel_insert",
                          "INSERT INTO catalog_task05_sentinel(value) VALUES(?1)",
                          statement)) {
            break;
        }
        sqlite3_bind_text(statement, 1, command->value.c_str(), -1,
                          SQLITE_TRANSIENT);
        const int rc = sqlite3_step(statement);
        reset(statement);
        result.success = rc == SQLITE_DONE;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        break;
    }

    case kWriteSchemaMarkerOperation: {
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed(
                "schema_marker_write",
                "INSERT OR REPLACE INTO media_items(id, kind, title) "
                "VALUES('__task06_marker__', 1, ?1)",
                statement)) {
            break;
        }
        const int bindRc = sqlite3_bind_text(statement, 1,
                                             command->value.c_str(), -1,
                                             SQLITE_TRANSIENT);
        const int stepRc = bindRc == SQLITE_OK ? sqlite3_step(statement)
                                                : bindRc;
        reset(statement);
        result.success = stepRc == SQLITE_DONE;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        break;
    }

    case kReadSchemaMarkerOperation: {
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed(
                "schema_marker_read",
                "SELECT title FROM media_items "
                "WHERE id='__task06_marker__'",
                statement)) {
            break;
        }
        const int rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(statement, 0);
            result.sentinel = value ? reinterpret_cast<const char *>(value) : "";
            result.success = true;
        } else {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(statement);
        break;
    }

    case kSetSchemaMetadataOperation: {
        const std::size_t separator = command->value.find(':');
        if (separator == std::string::npos) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "invalid schema metadata fixture";
            break;
        }
        const std::string applicationId = command->value.substr(0, separator);
        const std::string userVersion = command->value.substr(separator + 1);
        const std::string applicationIdPragma =
            "PRAGMA application_id = " + applicationId + ";";
        const std::string userVersionPragma =
            "PRAGMA user_version = " + userVersion + ";";
        result.success = exec(m_db, applicationIdPragma.c_str(), result.message)
            && exec(m_db, userVersionPragma.c_str(), result.message);
        if (!result.success) {
            result.error = CatalogDbErrorCategory::SqliteError;
        }
        break;
    }

    case kMigrationRollbackOperation: {
        finalizeStatements();
        std::string migrationError;
        const bool began = exec(m_db, "BEGIN IMMEDIATE;", migrationError);
        const int failedMigration = began
            ? execResult(m_db,
                         "CREATE TABLE catalog_task07_rollback_probe(value TEXT);"
                         "INSERT INTO catalog_task07_missing(value) VALUES(1);",
                         migrationError)
            : SQLITE_ERROR;
        std::string rollbackError;
        const bool rolledBack = began
            && exec(m_db, "ROLLBACK;", rollbackError);
        std::int64_t probeCount = 1;
        const bool inspected = scalarInt(
            m_db,
            "SELECT COUNT(*) FROM sqlite_master WHERE name="
            "'catalog_task07_rollback_probe';",
            probeCount, result.message);
        sqlite3_stmt *probe = nullptr;
        const int prepareRc = sqlite3_prepare_v2(m_db, "SELECT 1", -1,
                                                 &probe, nullptr);
        if (prepareRc == SQLITE_OK && probe) {
            sqlite3_step(probe);
            sqlite3_reset(probe);
            sqlite3_clear_bindings(probe);
            m_statements.emplace("migration_probe", probe);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_preparedStatementCount = m_statements.size();
        }
        result.statementReused = prepareRc == SQLITE_OK && probe != nullptr;
        result.success = failedMigration != SQLITE_OK && rolledBack
            && inspected && probeCount == 0 && result.statementReused;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            if (result.message.empty()) {
                result.message = migrationError.empty() ? rollbackError
                                                        : migrationError;
            }
        }
        break;
    }

    case kReadSentinelOperation: {
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed("sentinel_read",
                          "SELECT value FROM catalog_task05_sentinel LIMIT 1",
                          statement)) {
            break;
        }
        const int rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(statement, 0);
            result.sentinel = value ? reinterpret_cast<const char *>(value) : "";
            result.success = true;
        } else {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(statement);
        break;
    }

    default:
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = "unknown CatalogDb test operation";
        break;
    }

    command->result.set_value(std::move(result));
}

CatalogDbTestResult CatalogDb::runTestCommand(unsigned char operation,
                                              const std::string &value)
{
    auto command = std::make_shared<TestCommand>();
    command->operation = operation;
    command->value = value;
    std::future<CatalogDbTestResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            CatalogDbTestResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            return stopped;
        }
        if (m_testCommands.size() >= kMaxPendingJobs) {
            CatalogDbTestResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb test command queue is full";
            return full;
        }
        m_testCommands.push_back(command);
    }
    m_wake.notify_one();
    return result.get();
}

std::future<CatalogDbHierarchyWriteResult>
CatalogDb::upsertSeriesHierarchyForTest(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::uint64_t generation, std::int64_t refreshMs, int failAfterRows,
    int cancelAfterRows)
{
    return enqueueHierarchyWrite(series, seasons, episodesBySeason, generation,
                                 refreshMs, true, false, {},
                                 CatalogDbFailureSpec{failAfterRows,
                                                      cancelAfterRows, -1});
}

std::future<CatalogDbReconcileResult> CatalogDb::reconcileSeriesForTest(
    const std::vector<MediaItem> &series, bool authoritative,
    int failAfterRows)
{
    return enqueueReconcile(series, authoritative, {},
                            CatalogDbFailureSpec{failAfterRows, -1, -1});
}

std::future<CatalogDbMediaPageUpsertResult> CatalogDb::upsertMediaPageForTest(
    const CatalogDbMediaPageWrite &page, int failAfterRows,
    const CatalogDbJobMetadata &metadata)
{
    return enqueueMediaPageUpsert(page, metadata,
                                  CatalogDbFailureSpec{failAfterRows, -1, -1});
}

} // namespace miyoofin

#endif // MIYOOFIN_TEST_BUILD
