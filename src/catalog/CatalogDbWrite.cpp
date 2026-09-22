#include "CatalogDb.hpp"
#include "CatalogDbInternal.hpp"

#include <chrono>
#include <thread>

namespace miyoofin {
using namespace catalog_db_internal;

std::future<CatalogDbTopLevelSyncResult>
CatalogDb::beginTopLevelSync(std::uint64_t generation, const CatalogDbJobMetadata& metadata)
{
    auto command = std::make_shared<TopLevelSyncCommand>();
    command->begin = true;
    command->generation = generation;
    command->metadata = metadata;
    auto future = command->result.get_future();
    std::lock_guard<std::mutex> lock(m_mutex);
    command->metadata.scopeEpoch =
        command->metadata.scopeEpoch ? command->metadata.scopeEpoch : m_requestedEpoch;
    m_topLevelSyncCommands.push_back(command);
    ++m_pendingJobs;
    m_wake.notify_one();
    return future;
}
std::future<CatalogDbTopLevelSyncResult>
CatalogDb::abortTopLevelSync(std::uint64_t generation, const CatalogDbJobMetadata& metadata)
{
    auto command = std::make_shared<TopLevelSyncCommand>();
    command->begin = false;
    command->generation = generation;
    command->metadata = metadata;
    auto future = command->result.get_future();
    std::lock_guard<std::mutex> lock(m_mutex);
    command->metadata.scopeEpoch =
        command->metadata.scopeEpoch ? command->metadata.scopeEpoch : m_requestedEpoch;
    m_topLevelSyncCommands.push_back(command);
    ++m_pendingJobs;
    m_wake.notify_one();
    return future;
}
std::future<CatalogDbTopLevelSyncResult>
CatalogDb::finalizeTopLevelSync(std::uint64_t generation, const CatalogDbJobMetadata& metadata)
{
    auto command = std::make_shared<TopLevelSyncCommand>();
    command->finalize = true;
    command->generation = generation;
    command->metadata = metadata;
    auto future = command->result.get_future();
    std::lock_guard<std::mutex> lock(m_mutex);
    command->metadata.scopeEpoch =
        command->metadata.scopeEpoch ? command->metadata.scopeEpoch : m_requestedEpoch;
    m_topLevelSyncCommands.push_back(command);
    ++m_pendingJobs;
    m_wake.notify_one();
    return future;
}
std::future<CatalogDbHierarchyResult>
CatalogDb::deleteMediaItemsByIds(const std::vector<std::string>& itemIds,
                                 const CatalogDbJobMetadata& metadata)
{
    auto command = std::make_shared<QueryCommand>();
    command->kind = QueryCommand::Kind::DeleteMediaItemsByIds;
    command->itemIds = itemIds;
    std::future<CatalogDbHierarchyResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CatalogDbHierarchyResult rejected;
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            rejected.error = CatalogDbErrorCategory::ScopeNotReady;
            rejected.message = "CatalogDb is stopping";
            command->result.set_value(std::move(rejected));
            return result;
        }
        if (itemIds.empty() || itemIds.size() > kMaxMetadataByIdRows) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            rejected.error = CatalogDbErrorCategory::ConfigurationFailed;
            rejected.message = "metadata-by-ID delete exceeds bounded limit";
            command->result.set_value(std::move(rejected));
            return result;
        }
        for (const auto& id : itemIds) {
            if (id.empty()) {
                performanceTelemetry().addCatalogDbEnqueueRejected();
                rejected.error = CatalogDbErrorCategory::ConfigurationFailed;
                rejected.message = "metadata-by-ID delete contains an empty ID";
                command->result.set_value(std::move(rejected));
                return result;
            }
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            rejected.cancelled = true;
            rejected.error = CatalogDbErrorCategory::Superseded;
            rejected.message = "CatalogDb delete cancelled";
            command->result.set_value(std::move(rejected));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            rejected.error = CatalogDbErrorCategory::OpenFailed;
            rejected.message = "CatalogDb query queue is full";
            command->result.set_value(std::move(rejected));
            return result;
        }
        command->metadata = metadata;
        command->enqueuedMonotonicUs = telemetryNowIfEnabled();
        if (command->metadata.generation == 0)
            command->metadata.generation = m_generation;
        if (command->metadata.scopeEpoch == 0)
            command->metadata.scopeEpoch = m_requestedEpoch;
        m_queryCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}
std::future<CatalogDbHierarchyWriteResult> CatalogDb::upsertSeriesHierarchy(
    const MediaItem& series, const std::vector<MediaItem>& seasons,
    const std::map<std::string, std::vector<MediaItem>>& episodesBySeason, std::uint64_t generation,
    std::int64_t refreshMs, const CatalogDbJobMetadata& metadata)
{
    return enqueueHierarchyWrite(series, seasons, episodesBySeason, generation, refreshMs, true,
                                 false, metadata);
}
std::future<CatalogDbHierarchyWriteResult> CatalogDb::stageSeriesHierarchy(
    const MediaItem& series, const std::vector<MediaItem>& seasons,
    const std::map<std::string, std::vector<MediaItem>>& episodesBySeason, std::uint64_t generation,
    std::int64_t refreshMs, bool complete, const CatalogDbJobMetadata& metadata)
{
    return enqueueHierarchyWrite(series, seasons, episodesBySeason, generation, refreshMs, complete,
                                 false, metadata);
}
std::future<CatalogDbHierarchyWriteResult> CatalogDb::reconcileSeasonHierarchy(
    const MediaItem& series, const MediaItem& season, const std::vector<MediaItem>& episodes,
    std::uint64_t generation, std::int64_t refreshMs, const CatalogDbJobMetadata& metadata)
{
    return enqueueHierarchyWrite(series, {season}, {{season.id, episodes}}, generation, refreshMs,
                                 false, true, metadata);
}
std::future<CatalogDbHierarchyWriteResult> CatalogDb::enqueueHierarchyWrite(
    const MediaItem& series, const std::vector<MediaItem>& seasons,
    const std::map<std::string, std::vector<MediaItem>>& episodesBySeason, std::uint64_t generation,
    std::int64_t refreshMs, bool complete, bool seasonScoped, const CatalogDbJobMetadata& metadata,
    CatalogDbFailureSpec injection)
{
    auto command = std::make_shared<HierarchyWriteCommand>();
    command->series = series;
    command->seasons = seasons;
    command->episodesBySeason = episodesBySeason;
    command->generation = generation;
    command->refreshMs = refreshMs;
    command->complete = complete;
    command->seasonScoped = seasonScoped;
    command->metadata = metadata;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
#ifdef MIYOOFIN_TEST_BUILD
    command->injection = injection;
#else
    (void)injection;
#endif // MIYOOFIN_TEST_BUILD
    std::future<CatalogDbHierarchyWriteResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyWriteResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyWriteResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb hierarchy write cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyWriteResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb write queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_writeCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}
std::future<CatalogDbMediaPageUpsertResult>
CatalogDb::upsertMediaPage(const CatalogDbMediaPageWrite& page,
                           const CatalogDbJobMetadata& metadata)
{
    return enqueueMediaPageUpsert(page, metadata);
}
std::future<CatalogDbMediaPageUpsertResult>
CatalogDb::enqueueMediaPageUpsert(const CatalogDbMediaPageWrite& page,
                                  const CatalogDbJobMetadata& metadata,
                                  CatalogDbFailureSpec injection)
{
    auto command = std::make_shared<MediaPageUpsertCommand>();
    command->page = page;
    command->metadata = metadata;
#ifdef MIYOOFIN_TEST_BUILD
    command->injection = injection;
#else
    (void)injection;
#endif // MIYOOFIN_TEST_BUILD
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    auto future = command->result.get_future();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping || m_pendingJobs >= kMaxPendingJobs) {
        CatalogDbMediaPageUpsertResult r;
        r.error = CatalogDbErrorCategory::ScopeNotReady;
        r.message = m_stopping ? "CatalogDb page queue stopped" : "CatalogDb page queue full";
        logMediaPageTransition(command, "page_complete", &r);
        command->result.set_value(std::move(r));
        return future;
    }
    command->metadata.generation =
        command->metadata.generation ? command->metadata.generation : m_generation;
    command->metadata.scopeEpoch =
        command->metadata.scopeEpoch ? command->metadata.scopeEpoch : m_requestedEpoch;
    m_mediaPageUpsertCommands.push_back(command);
    ++m_pendingJobs;
    catalogDiagnostic("page_enqueue request=" + std::to_string(page.request) + " generation=" +
                      std::to_string(command->metadata.generation) + " page_kind=" +
                      (page.collectionType == "movies"    ? "movies"
                       : page.collectionType == "tvshows" ? "tvshows"
                                                          : "unknown") +
                      " page_index=" + std::to_string(page.ordinalStart) +
                      " view_ordinal=" + std::to_string(page.viewOrdinal) +
                      " final=" + std::to_string(page.finalPage ? 1 : 0) +
                      " queue_depth=" + std::to_string(m_pendingJobs));
    catalogDiagnostic("page_submit_enqueued");
    m_wake.notify_one();
    return future;
}
void CatalogDb::logMediaPageTransition(const std::shared_ptr<MediaPageUpsertCommand>& command,
                                       const char* event,
                                       const CatalogDbMediaPageUpsertResult* result,
                                       std::size_t attempt)
{
    const auto& page = command->page;
    std::string line = std::string(event) + " request=" + std::to_string(page.request) +
                       " generation=" + std::to_string(command->metadata.generation) +
                       " page_kind=" +
                       (page.collectionType == "movies"    ? "movies"
                        : page.collectionType == "tvshows" ? "tvshows"
                                                           : "unknown") +
                       " page_index=" + std::to_string(page.ordinalStart) +
                       " view_ordinal=" + std::to_string(page.viewOrdinal) +
                       " final=" + std::to_string(page.finalPage ? 1 : 0);
    if (attempt != 0)
        line += " attempt=" + std::to_string(attempt);
    if (command->enqueuedMonotonicUs != 0) {
        const std::uint64_t now = telemetryNowIfEnabled();
        if (now >= command->enqueuedMonotonicUs)
            line += " elapsed_us=" + std::to_string(now - command->enqueuedMonotonicUs);
    }
    if (result) {
        line += " success=" + std::to_string(result->success ? 1 : 0) +
                " cancelled=" + std::to_string(result->cancelled ? 1 : 0) +
                " superseded=" + std::to_string(result->superseded ? 1 : 0) +
                " error=" + std::to_string(static_cast<unsigned>(result->error));
    }
    catalogDiagnostic(line);
}
void CatalogDb::processHierarchyWrite(const std::shared_ptr<HierarchyWriteCommand>& command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbHierarchyWriteResult result;
    result.workerOwned = true;
    uint64_t transactionStartUs = 0;
    bool transactionActive = false;
    auto finish = [&] {
        PerformanceTelemetry& telemetry = performanceTelemetry();
        const uint64_t endUs = telemetryNowIfEnabled();
        if (command->enqueuedMonotonicUs != 0 && endUs >= command->enqueuedMonotonicUs)
            telemetry.recordCatalogDbQueueWait(endUs - command->enqueuedMonotonicUs);
        if (transactionActive && endUs >= transactionStartUs) {
            telemetry.recordCatalogDbTransaction(endUs - transactionStartUs);
            transactionActive = false;
        }
        if (result.cancelled || result.superseded)
            telemetry.addCatalogDbCancelled();
        else if (result.success)
            telemetry.addCatalogDbCompleted();
        else
            telemetry.addCatalogDbFailed();
        command->result.set_value(std::move(result));
    };
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        finish();
        return;
    }
    if (!validateHierarchyInput(command->series, command->seasons, command->episodesBySeason,
                                result.message)) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        finish();
        return;
    }
    if (command->seasonScoped && command->seasons.size() != 1) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        result.message = "season reconciliation must contain one season";
        finish();
        return;
    }

    enum class WriteState : unsigned char
    {
        Valid,
        Cancelled,
        Superseded,
    };
    auto state = [&] {
        if (command->failureSpec().cancelAfterRows >= 0 &&
            result.rowsWritten >=
                static_cast<std::size_t>(command->failureSpec().cancelAfterRows)) {
            return WriteState::Cancelled;
        }
        if (command->metadata.cancellation && command->metadata.cancellation->load()) {
            return WriteState::Cancelled;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation ||
            command->metadata.scopeEpoch != m_requestedEpoch || !m_scopeConfigured ||
            !m_scopeReady) {
            return WriteState::Superseded;
        }
        return WriteState::Valid;
    };
    auto rejectState = [&](WriteState writeState) {
        result.error = CatalogDbErrorCategory::Superseded;
        if (writeState == WriteState::Cancelled) {
            result.cancelled = true;
            result.message = "CatalogDb hierarchy write cancelled";
        } else {
            result.superseded = true;
            result.message = "CatalogDb hierarchy write superseded";
        }
    };
    auto validState = [&] {
        const WriteState writeState = state();
        if (writeState != WriteState::Valid) {
            rejectState(writeState);
            return false;
        }
        return true;
    };
    if (!validState()) {
        finish();
        return;
    }

    auto prepareCached = [&](const char* name, const char* sql, sqlite3_stmt*& statement) {
        auto existing = m_statements.find(name);
        if (existing != m_statements.end()) {
            statement = existing->second;
            return true;
        }
        if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
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
    sqlite3_stmt* upsert = nullptr;
    sqlite3_stmt* deleteSeasons = nullptr;
    sqlite3_stmt* deleteEpisodes = nullptr;
    sqlite3_stmt* markComplete = nullptr;
    sqlite3_stmt* deleteGenres = nullptr;
    sqlite3_stmt* insertGenre = nullptr;
    sqlite3_stmt* deleteImageTags = nullptr;
    sqlite3_stmt* insertImageTag = nullptr;
    sqlite3_stmt* selectGenres = nullptr;
    sqlite3_stmt* selectImageTags = nullptr;
    if (!prepareCached("hierarchy_upsert_item",
                       "INSERT INTO media_items("
                       "id, kind, title, overview, production_year, community_rating,"
                       "etag, played, progress, playback_position_ticks, index_number,"
                       "parent_index_number, runtime_ticks, series_name, series_id,"
                       "season_id) "
                       "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                       "?13, ?14, ?15, ?16) "
                       "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, "
                       "title=excluded.title, overview=excluded.overview, "
                       "production_year=excluded.production_year, "
                       "community_rating=excluded.community_rating, etag=excluded.etag, "
                       "played=excluded.played, progress=excluded.progress, "
                       "playback_position_ticks=excluded.playback_position_ticks, "
                       "index_number=excluded.index_number, "
                       "parent_index_number=excluded.parent_index_number, "
                       "runtime_ticks=excluded.runtime_ticks, "
                       "series_name=excluded.series_name, series_id=excluded.series_id, "
                       "season_id=excluded.season_id",
                       upsert) ||
        !prepareCached("hierarchy_delete_seasons",
                       "DELETE FROM media_items WHERE kind=3 AND series_id=?1", deleteSeasons) ||
        !prepareCached("hierarchy_delete_episodes",
                       "DELETE FROM media_items WHERE kind=4 AND season_id=?1", deleteEpisodes) ||
        !prepareCached("hierarchy_mark_complete",
                       "INSERT INTO hierarchy_state(series_id, complete, "
                       "last_refresh_ms, last_generation) VALUES(?1, ?2, ?3, ?4) "
                       "ON CONFLICT(series_id) DO UPDATE SET complete=excluded.complete, "
                       "last_refresh_ms=excluded.last_refresh_ms, "
                       "last_generation=excluded.last_generation",
                       markComplete) ||
        !prepareCached("media_item_genres_delete", "DELETE FROM item_genres WHERE item_id=?1",
                       deleteGenres) ||
        !prepareCached("media_item_genre_insert",
                       "INSERT INTO item_genres(item_id, ordinal, genre) "
                       "VALUES(?1, ?2, ?3)",
                       insertGenre) ||
        !prepareCached("media_item_image_tags_delete",
                       "DELETE FROM item_image_tags WHERE item_id=?1", deleteImageTags) ||
        !prepareCached("media_item_image_tag_insert",
                       "INSERT INTO item_image_tags(item_id, image_type, tag) "
                       "VALUES(?1, ?2, ?3)",
                       insertImageTag) ||
        !prepareCached("media_item_genres_select",
                       "SELECT ordinal, genre FROM item_genres WHERE item_id=?1 "
                       "ORDER BY ordinal",
                       selectGenres) ||
        !prepareCached("media_item_image_tags_select",
                       "SELECT image_type, tag FROM item_image_tags WHERE item_id=?1 "
                       "ORDER BY image_type",
                       selectImageTags)) {
        finish();
        return;
    }

    auto reset = [](sqlite3_stmt* statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };
    const MediaItemCollectionStatements collections{deleteGenres,   insertGenre,  deleteImageTags,
                                                    insertImageTag, selectGenres, selectImageTags};
    auto rollback = [&] {
        std::string ignored;
        exec(m_db, "ROLLBACK;", ignored);
    };
    auto deleteById = [&](sqlite3_stmt* statement, const std::string& id) {
        reset(statement);
        const bool ok =
            sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_step(statement) == SQLITE_DONE;
        if (!ok) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            performanceTelemetry().recordCatalogDbSqliteError(sqlite3_extended_errcode(m_db));
        } else {
            performanceTelemetry().addCatalogDbRows(0, 0,
                                                    static_cast<uint32_t>(sqlite3_changes(m_db)));
        }
        reset(statement);
        return ok;
    };
    auto writeItem = [&](const MediaItem& item) {
        if (!validState()) {
            return false;
        }
        MediaItemSqlError error = MediaItemSqlError::None;
        bool existed = false;
        if (performanceTelemetry().enabledFast() &&
            !mediaItemRowExists(m_db, item.id, existed, result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            performanceTelemetry().recordCatalogDbSqliteError(sqlite3_extended_errcode(m_db));
            return false;
        }
        reset(upsert);
        const bool bound = bindMediaItemScalars(upsert, item, error);
        const bool stepped = bound && sqlite3_step(upsert) == SQLITE_DONE &&
                             maintainOrganizationalSortKey(m_db, item, result.message);
        if (!stepped) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            performanceTelemetry().recordCatalogDbSqliteError(sqlite3_extended_errcode(m_db));
            reset(upsert);
            return false;
        }
        performanceTelemetry().addCatalogDbRows(existed ? 0u : 1u, existed ? 1u : 0u, 0u);
        reset(upsert);
        if (!replaceMediaItemCollections(collections, item, error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "hierarchy collection write failed";
            return false;
        }
        ++result.rowsWritten;
        if (command->failureSpec().failAfterRows >= 0 &&
            result.rowsWritten >= static_cast<std::size_t>(command->failureSpec().failAfterRows)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "injected hierarchy write failure";
            return false;
        }
        return true;
    };

    const uint64_t beginUs = telemetryNowIfEnabled();
    const bool began = exec(m_db, "BEGIN IMMEDIATE;", result.message);
    if (began) {
        transactionStartUs = beginUs;
        transactionActive = true;
    }
    if (!began || !writeItem(command->series) ||
        (command->complete && !deleteById(deleteSeasons, command->series.id))) {
        rollback();
        finish();
        return;
    }
    for (const auto& season : command->seasons) {
        if (((command->complete || command->seasonScoped) &&
             !deleteById(deleteEpisodes, season.id)) ||
            !writeItem(season)) {
            rollback();
            finish();
            return;
        }
        for (const auto& episode : command->episodesBySeason.at(season.id)) {
            if (!writeItem(episode)) {
                rollback();
                finish();
                return;
            }
        }
    }
    if (!validState()) {
        rollback();
        finish();
        return;
    }
    if (command->series.type != "movie") {
        reset(markComplete);
        if (sqlite3_bind_text(markComplete, 1, command->series.id.c_str(), -1, SQLITE_TRANSIENT) !=
                SQLITE_OK ||
            sqlite3_bind_int(markComplete, 2,
                             command->complete && !command->seasonScoped ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int64(markComplete, 3, command->refreshMs) != SQLITE_OK ||
            sqlite3_bind_int64(markComplete, 4, static_cast<sqlite3_int64>(command->generation)) !=
                SQLITE_OK ||
            sqlite3_step(markComplete) != SQLITE_DONE) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            reset(markComplete);
            rollback();
            finish();
            return;
        }
        reset(markComplete);
    }
    const uint64_t commitStartUs = telemetryNowIfEnabled();
    if (!exec(m_db, "COMMIT;", result.message)) {
        rollback();
        finish();
        return;
    }
    transactionActive = false;
    if (commitStartUs != 0) {
        const uint64_t commitEndUs = telemetryNowIfEnabled();
        if (commitEndUs >= commitStartUs)
            performanceTelemetry().recordCatalogDbCommit(commitEndUs - commitStartUs);
    }
    if (transactionStartUs != 0) {
        const uint64_t transactionEndUs = telemetryNowIfEnabled();
        if (transactionEndUs >= transactionStartUs)
            performanceTelemetry().recordCatalogDbTransaction(transactionEndUs -
                                                              transactionStartUs);
    }
    result.success = true;
    finish();
}
bool CatalogDb::mediaPageUpsertStillValid(const std::shared_ptr<MediaPageUpsertCommand>& command,
                                          CatalogDbMediaPageUpsertResult& result)
{
    if (command->metadata.cancellation && command->metadata.cancellation->load()) {
        result.cancelled = true;
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (command->metadata.generation != m_generation ||
        command->metadata.scopeEpoch != m_requestedEpoch || !m_scopeReady) {
        result.superseded = true;
        return false;
    }
    return true;
}
void CatalogDb::logMediaPageRollback(const std::shared_ptr<MediaPageUpsertCommand>& command,
                                     const char* stage, std::size_t itemOrdinal,
                                     const MediaItem* item, int sqliteRc = SQLITE_OK)
{
    std::string line =
        "page_transaction_rollback page_start=" + std::to_string(command->page.ordinalStart) +
        " page_count=" + std::to_string(command->page.items.size()) +
        " item_ordinal=" + std::to_string(itemOrdinal) +
        " media_type=" + (item ? item->type : "unknown") + " stage=" + stage;
    if (sqliteRc != SQLITE_OK) {
        line += " sqlite_rc=" + std::to_string(sqliteRc) +
                " sqlite_extended_rc=" + std::to_string(sqlite3_extended_errcode(m_db));
    }
    catalogDiagnostic(line);
}
void CatalogDb::markMediaPagePopulationFailed(const CatalogDbMediaPageUpsertResult& result)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!result.cancelled && !result.superseded)
        m_populationStatus.state = CatalogDbPopulationState::Failed;
}
bool CatalogDb::prepareMediaPageItemStatements(sqlite3_stmt** upsert, sqlite3_stmt** deleteGenres,
                                               sqlite3_stmt** insertGenre,
                                               sqlite3_stmt** deleteImageTags,
                                               sqlite3_stmt** insertImageTag,
                                               CatalogDbMediaPageUpsertResult& result)
{
    const char* sql =
        "INSERT INTO "
        "media_items(id,kind,title,overview,production_year,community_rating,etag,played,progress,"
        "playback_position_ticks,index_number,parent_index_number,runtime_ticks,series_name,series_"
        "id,season_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
        "kind=excluded.kind,title=excluded.title,overview=excluded.overview,production_year="
        "excluded.production_year,community_rating=excluded.community_rating,etag=excluded.etag,"
        "played=excluded.played,progress=excluded.progress,playback_position_ticks=excluded."
        "playback_position_ticks,index_number=excluded.index_number,parent_index_number=excluded."
        "parent_index_number,runtime_ticks=excluded.runtime_ticks,series_name=excluded.series_name,"
        "series_id=excluded.series_id,season_id=excluded.season_id";
    const bool prepared =
        sqlite3_prepare_v2(m_db, sql, -1, upsert, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(m_db, "DELETE FROM item_genres WHERE item_id=?", -1, deleteGenres,
                           nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(m_db, "INSERT INTO item_genres(item_id,ordinal,genre) VALUES(?,?,?)", -1,
                           insertGenre, nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(m_db, "DELETE FROM item_image_tags WHERE item_id=?", -1, deleteImageTags,
                           nullptr) == SQLITE_OK &&
        sqlite3_prepare_v2(m_db,
                           "INSERT INTO item_image_tags(item_id,image_type,tag) VALUES(?,?,?)", -1,
                           insertImageTag, nullptr) == SQLITE_OK;
    if (!prepared) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        return false;
    }
    return true;
}
bool CatalogDb::prepareMediaPageViewRow(const std::shared_ptr<MediaPageUpsertCommand>& command,
                                        bool staged, sqlite3_stmt** view, sqlite3_stmt** membership)
{
    const char* viewSql =
        staged
            ? "INSERT INTO top_level_sync_views(generation,id,name,collection_type,ordinal) "
              "VALUES(?,?,?,?,?) ON CONFLICT(generation,id) DO UPDATE SET "
              "name=excluded.name,collection_type=excluded.collection_type,ordinal=excluded.ordinal"
            : "INSERT INTO library_views(id,name,collection_type,ordinal) VALUES(?,?,?,?) ON "
              "CONFLICT(id) DO UPDATE SET "
              "name=excluded.name,collection_type=excluded.collection_type";
    const char* membershipSql =
        staged ? "INSERT INTO top_level_sync_membership(generation,view_id,item_id,ordinal) "
                 "VALUES(?,?,?,?) ON CONFLICT(generation,view_id,item_id) DO UPDATE SET "
                 "ordinal=excluded.ordinal"
               : "INSERT INTO library_membership(view_id,item_id,ordinal) VALUES(?,?,?) ON "
                 "CONFLICT(view_id,item_id) DO UPDATE SET ordinal=excluded.ordinal";
    return command->page.viewId.empty() ||
           (sqlite3_prepare_v2(m_db, viewSql, -1, view, nullptr) == SQLITE_OK &&
            (!staged || sqlite3_bind_int64(
                            *view, 1, static_cast<sqlite3_int64>(command->page.syncGeneration)) ==
                            SQLITE_OK) &&
            sqlite3_bind_text(*view, staged ? 2 : 1, command->page.viewId.c_str(), -1,
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_text(*view, staged ? 3 : 2, command->page.viewName.c_str(), -1,
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_text(*view, staged ? 4 : 3, command->page.collectionType.c_str(), -1,
                              SQLITE_TRANSIENT) == SQLITE_OK &&
            sqlite3_bind_int(*view, staged ? 5 : 4, command->page.viewOrdinal) == SQLITE_OK &&
            sqlite3_step(*view) == SQLITE_DONE &&
            sqlite3_prepare_v2(m_db, membershipSql, -1, membership, nullptr) == SQLITE_OK);
}
CatalogDb::MediaPageItemLoopOutcome CatalogDb::writeMediaPageItemRows(
    const std::shared_ptr<MediaPageUpsertCommand>& command, bool staged, std::size_t attempt,
    bool viewReady, sqlite3_stmt* upsert, const MediaItemCollectionStatements& collections,
    sqlite3_stmt* membership, CatalogDbMediaPageUpsertResult& result)
{
    for (std::size_t n = 0; viewReady && n < command->page.items.size(); ++n) {
        const auto& item = command->page.items[n];
        MediaItemSqlError e = MediaItemSqlError::None;
        if (command->failureSpec().failAfterRows >= 0 &&
            static_cast<int>(result.rowsWritten) >= command->failureSpec().failAfterRows) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "injected page failure";
            logMediaPageRollback(command, "injected_test_failure", n, &item);
            return MediaPageItemLoopOutcome::Fatal;
        }
        sqlite3_reset(upsert);
        sqlite3_clear_bindings(upsert);
        if (!bindMediaItemScalars(upsert, item, e)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            logMediaPageRollback(
                command, e == MediaItemSqlError::InvalidKind ? "ID/type" : "scalar validation", n,
                &item);
            return MediaPageItemLoopOutcome::Fatal;
        }
        const int mediaRc = sqlite3_step(upsert);
        if (mediaRc != SQLITE_DONE) {
            result.error = CatalogDbErrorCategory::SqliteError;
            logMediaPageRollback(command, "media bind/step", n, &item, mediaRc);
            if (pageTransactionShouldRetry(mediaRc, attempt, kPageTransactionMaxAttempts)) {
                sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                return MediaPageItemLoopOutcome::Retryable;
            }
            return MediaPageItemLoopOutcome::Fatal;
        }
        if (!maintainOrganizationalSortKey(m_db, item, result.message)) {
            const int sortRc = sqlite3_errcode(m_db);
            result.error = CatalogDbErrorCategory::SqliteError;
            logMediaPageRollback(command, "sort key", n, &item, sortRc);
            if (pageTransactionShouldRetry(sortRc, attempt, kPageTransactionMaxAttempts)) {
                sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                return MediaPageItemLoopOutcome::Retryable;
            }
            return MediaPageItemLoopOutcome::Fatal;
        }
        if (!replaceMediaItemCollections(collections, item, e)) {
            const int collRc = sqlite3_errcode(m_db);
            result.error = CatalogDbErrorCategory::SqliteError;
            logMediaPageRollback(command, "media bind/step", n, &item, collRc);
            if (pageTransactionShouldRetry(collRc, attempt, kPageTransactionMaxAttempts)) {
                sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                return MediaPageItemLoopOutcome::Retryable;
            }
            return MediaPageItemLoopOutcome::Fatal;
        }
        if (!mediaPageUpsertStillValid(command, result)) {
            result.error = CatalogDbErrorCategory::Superseded;
            logMediaPageRollback(command, result.cancelled ? "stale/cancel" : "stale/cancel", n,
                                 &item);
            return MediaPageItemLoopOutcome::Fatal;
        }
        if (!command->page.viewId.empty()) {
            sqlite3_reset(membership);
            sqlite3_clear_bindings(membership);
            const int offset = staged ? 1 : 0;
            if (staged)
                sqlite3_bind_int64(membership, 1,
                                   static_cast<sqlite3_int64>(command->page.syncGeneration));
            sqlite3_bind_text(membership, 1 + offset, command->page.viewId.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(membership, 2 + offset, item.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(membership, 3 + offset,
                               static_cast<sqlite3_int64>(command->page.ordinalStart + n));
            const int membershipRc = sqlite3_step(membership);
            if (membershipRc != SQLITE_DONE) {
                result.error = CatalogDbErrorCategory::SqliteError;
                logMediaPageRollback(command, "membership bind/step", n, &item, membershipRc);
                if (pageTransactionShouldRetry(membershipRc, attempt,
                                               kPageTransactionMaxAttempts)) {
                    sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                    return MediaPageItemLoopOutcome::Retryable;
                }
                return MediaPageItemLoopOutcome::Fatal;
            }
        }
        ++result.rowsWritten;
    }
    return MediaPageItemLoopOutcome::Completed;
}
CatalogDb::MediaPageAttemptOutcome
CatalogDb::attemptMediaPageUpsert(const std::shared_ptr<MediaPageUpsertCommand>& command,
                                  bool staged, std::size_t attempt,
                                  CatalogDbMediaPageUpsertResult& result)
{
    result = CatalogDbMediaPageUpsertResult{};
    result.workerOwned = true;
    logMediaPageTransition(command, "page_transaction_begin", &result, attempt + 1);
    catalogDiagnostic("page_transaction_begin");
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        const int rc = sqlite3_errcode(m_db);
        if (pageTransactionShouldRetry(rc, attempt, kPageTransactionMaxAttempts)) {
            logMediaPageTransition(command, "page_retry", &result, attempt + 1);
            catalogDiagnostic("page_transaction_retry_begin sqlite_rc=" + std::to_string(rc));
            return MediaPageAttemptOutcome::Retry;
        }
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = "CatalogDb transaction begin failed";
        catalogDiagnostic("page_transaction_rollback reason=begin_failed sqlite_rc=" +
                          std::to_string(rc));
        markMediaPagePopulationFailed(result);
        logMediaPageTransition(command, "page_complete", &result, attempt + 1);
        command->result.set_value(std::move(result));
        return MediaPageAttemptOutcome::Complete;
    }
    // All seven statements are RAII-owned: every exit below (commit, fatal,
    // retry, view failure) releases them automatically via ScopedSqliteStmt.
    ScopedSqliteStmt upsertGuard, deleteGenresGuard, insertGenreGuard, deleteImageTagsGuard,
        insertImageTagGuard, viewGuard, membershipGuard;
    if (!prepareMediaPageItemStatements(upsertGuard.receive(), deleteGenresGuard.receive(),
                                        insertGenreGuard.receive(), deleteImageTagsGuard.receive(),
                                        insertImageTagGuard.receive(), result)) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        markMediaPagePopulationFailed(result);
        logMediaPageTransition(command, "page_complete", &result, attempt + 1);
        command->result.set_value(std::move(result));
        return MediaPageAttemptOutcome::Complete;
    }
    const MediaItemCollectionStatements collections{deleteGenresGuard.get(), insertGenreGuard.get(),
                                                    deleteImageTagsGuard.get(),
                                                    insertImageTagGuard.get()};
    const bool viewReady =
        prepareMediaPageViewRow(command, staged, viewGuard.receive(), membershipGuard.receive());
    const MediaPageItemLoopOutcome itemOutcome =
        writeMediaPageItemRows(command, staged, attempt, viewReady, upsertGuard.get(), collections,
                               membershipGuard.get(), result);
    if (!viewReady && result.error == CatalogDbErrorCategory::None) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
    }
    if (itemOutcome == MediaPageItemLoopOutcome::Fatal) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        markMediaPagePopulationFailed(result);
        logMediaPageTransition(command, "page_complete", &result, attempt + 1);
        command->result.set_value(std::move(result));
        return MediaPageAttemptOutcome::Complete;
    }
    if (itemOutcome == MediaPageItemLoopOutcome::Retryable) {
        logMediaPageTransition(command, "page_retry", &result, attempt + 1);
        return MediaPageAttemptOutcome::Retry;
    }
    if (result.error == CatalogDbErrorCategory::None &&
        mediaPageUpsertStillValid(command, result)) {
        if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK) {
            result.success = true;
            logMediaPageTransition(command, "page_transaction_commit", &result, attempt + 1);
            catalogDiagnostic("page_transaction_commit");
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_populationStatus.pages;
            m_populationStatus.rows += result.rowsWritten;
            m_populationStatus.state =
                command->page.finalPage
                    ? (result.rowsWritten ? CatalogDbPopulationState::Ready
                                          : CatalogDbPopulationState::GenuinelyEmpty)
                    : CatalogDbPopulationState::Populating;
            logMediaPageTransition(command, "page_complete", &result, attempt + 1);
            command->result.set_value(std::move(result));
            return MediaPageAttemptOutcome::Complete;
        }
        const int commitRc = sqlite3_errcode(m_db);
        if (pageTransactionShouldRetry(commitRc, attempt, kPageTransactionMaxAttempts)) {
            logMediaPageTransition(command, "page_retry", &result, attempt + 1);
            catalogDiagnostic("page_transaction_retry_commit sqlite_rc=" +
                              std::to_string(commitRc));
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return MediaPageAttemptOutcome::Retry;
        }
        result.error = CatalogDbErrorCategory::SqliteError;
        catalogDiagnostic("page_transaction_rollback reason=commit_failed sqlite_rc=" +
                          std::to_string(commitRc));
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_populationStatus.state = CatalogDbPopulationState::Failed;
    } else {
        if (result.error == CatalogDbErrorCategory::None) {
            logMediaPageRollback(command, "population state", result.rowsWritten, nullptr);
        } else if (result.message.empty()) {
            logMediaPageRollback(command, "FK/unique", result.rowsWritten, nullptr,
                                 sqlite3_errcode(m_db));
        }
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        markMediaPagePopulationFailed(result);
    }
    logMediaPageTransition(command, "page_complete", &result, attempt + 1);
    command->result.set_value(std::move(result));
    return MediaPageAttemptOutcome::Complete;
}
void CatalogDb::processMediaPageUpsert(const std::shared_ptr<MediaPageUpsertCommand>& command)
{
    CatalogDbMediaPageUpsertResult result;
    result.workerOwned = true;
    logMediaPageTransition(command, "page_dequeue", nullptr);
    if (!mediaPageUpsertStillValid(command, result)) {
        result.error = CatalogDbErrorCategory::Superseded;
        logMediaPageTransition(command, "page_complete", &result);
        catalogDiagnostic(result.cancelled ? "page_submit_rejected reason=cancelled"
                                           : "page_submit_rejected reason=stale_or_not_ready");
        command->result.set_value(std::move(result));
        return;
    }
    catalogDiagnostic("page_submit_dequeued");
    const bool staged = command->page.syncGeneration != 0;
    if (staged) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->page.syncGeneration != m_activeTopLevelSyncGeneration) {
            result.superseded = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "top-level sync generation is not active";
            logMediaPageTransition(command, "page_complete", &result);
            command->result.set_value(std::move(result));
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_populationStatus.state = CatalogDbPopulationState::Populating;
    }
    // Bounded retry for transient SQLite errors (SQLITE_BUSY, SQLITE_LOCKED,
    // SQLITE_FULL).  Prevents a tight spin when /tmp is full or another
    // connection holds the lock; a short cancellable backoff gives the
    // condition time to resolve.
    for (std::size_t attempt = 0; attempt < kPageTransactionMaxAttempts; ++attempt) {
        if (attempt > 0) {
            for (int ms = 0; ms < 100; ms += 10) {
                if (command->metadata.cancellation && command->metadata.cancellation->load()) {
                    result.cancelled = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message = "page transaction cancelled during retry backoff";
                    logMediaPageTransition(command, "page_complete", &result, attempt + 1);
                    command->result.set_value(std::move(result));
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            catalogDiagnostic("page_transaction_retry attempt=" + std::to_string(attempt + 1));
        }
        if (attemptMediaPageUpsert(command, staged, attempt, result) ==
            MediaPageAttemptOutcome::Complete) {
            return;
        }
    }
    // All retry attempts exhausted — fail the sync cleanly.
    result.error = CatalogDbErrorCategory::SqliteError;
    result.message = "page transaction failed after retries";
    catalogDiagnostic("page_transaction_exhausted attempts=" +
                      std::to_string(kPageTransactionMaxAttempts));
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_populationStatus.state = CatalogDbPopulationState::Failed;
    }
    logMediaPageTransition(command, "page_complete", &result, kPageTransactionMaxAttempts);
    command->result.set_value(std::move(result));
}
} // namespace miyoofin
