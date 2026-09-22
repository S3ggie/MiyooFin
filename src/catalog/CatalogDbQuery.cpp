#include "CatalogDb.hpp"
#include "CatalogDbInternal.hpp"

namespace miyoofin {
using namespace catalog_db_internal;

std::future<CatalogDbHierarchyResult> CatalogDb::getSeasons(const std::string& seriesId,
                                                            const CatalogDbJobMetadata& metadata)
{
    auto command = std::make_shared<QueryCommand>();
    command->kind = QueryCommand::Kind::Seasons;
    command->parentId = seriesId;
    std::future<CatalogDbHierarchyResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb query cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb query queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        command->metadata = metadata;
        command->enqueuedMonotonicUs = telemetryNowIfEnabled();
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_queryCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}
std::future<CatalogDbHierarchyResult> CatalogDb::getEpisodes(const std::string& seasonId,
                                                             const CatalogDbJobMetadata& metadata)
{
    auto command = std::make_shared<QueryCommand>();
    command->kind = QueryCommand::Kind::Episodes;
    command->parentId = seasonId;
    std::future<CatalogDbHierarchyResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb query cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb query queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        command->metadata = metadata;
        command->enqueuedMonotonicUs = telemetryNowIfEnabled();
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_queryCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}
std::future<CatalogDbHierarchyResult>
CatalogDb::readMediaItemsByIds(const std::vector<std::string>& itemIds,
                               const CatalogDbJobMetadata& metadata)
{
    auto command = std::make_shared<QueryCommand>();
    command->kind = QueryCommand::Kind::MediaItemsByIds;
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
            rejected.message = "metadata-by-ID query exceeds bounded limit";
            command->result.set_value(std::move(rejected));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            rejected.cancelled = true;
            rejected.error = CatalogDbErrorCategory::Superseded;
            rejected.message = "CatalogDb query cancelled";
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
std::future<CatalogDbMediaPageResult>
CatalogDb::readMediaPage(const std::string& type, int alphabetLetter, std::size_t limit,
                         const CatalogDbPageCursor& after, const CatalogDbJobMetadata& metadata,
                         CatalogDbMediaPageFilter filter)
{
    return enqueueMediaPage(type, alphabetLetter, limit, after, metadata, filter);
}
std::future<CatalogDbMediaPageResult>
CatalogDb::enqueueMediaPage(const std::string& type, int alphabetLetter, std::size_t limit,
                            const CatalogDbPageCursor& after, const CatalogDbJobMetadata& metadata,
                            CatalogDbMediaPageFilter filter)
{
    auto command = std::make_shared<MediaPageCommand>();
    command->type = type;
    command->letter = alphabetLetter;
    command->limit = std::min<std::size_t>(limit, 64);
    command->filter = filter;
    command->after = after;
    command->metadata = metadata;
    auto future = command->result.get_future();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
        CatalogDbMediaPageResult r;
        r.error = CatalogDbErrorCategory::ScopeNotReady;
        r.message = "CatalogDb is stopping";
        command->result.set_value(std::move(r));
        return future;
    }
    if (command->limit == 0 || (type != "movie" && type != "show")) {
        CatalogDbMediaPageResult r;
        r.error = CatalogDbErrorCategory::SqliteError;
        r.message = "invalid media page request";
        command->result.set_value(std::move(r));
        return future;
    }
    if (m_pendingJobs >= kMaxPendingJobs) {
        CatalogDbMediaPageResult r;
        r.error = CatalogDbErrorCategory::OpenFailed;
        r.message = "CatalogDb page queue is full";
        command->result.set_value(std::move(r));
        return future;
    }
    command->metadata.generation =
        command->metadata.generation ? command->metadata.generation : m_generation;
    command->metadata.scopeEpoch =
        command->metadata.scopeEpoch ? command->metadata.scopeEpoch : m_requestedEpoch;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    m_mediaPageCommands.push_back(command);
    ++m_pendingJobs;
    performanceTelemetry().setCatalogDbQueueDepth(static_cast<std::uint32_t>(m_pendingJobs));
    m_wake.notify_one();
    return future;
}
std::future<CatalogCompatibilityReadResult>
CatalogDb::enqueueLibraryRead(const CatalogDbJobMetadata& metadata)
{
    auto command = std::make_shared<LibraryReadCommand>();
    command->metadata = metadata;
    auto future = command->result.get_future();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
        CatalogCompatibilityReadResult r;
        r.error = CatalogDbErrorCategory::ScopeNotReady;
        r.message = "CatalogDb is stopping";
        command->result.set_value(std::move(r));
        return future;
    }
    if (m_pendingJobs >= kMaxPendingJobs) {
        CatalogCompatibilityReadResult r;
        r.error = CatalogDbErrorCategory::OpenFailed;
        r.message = "CatalogDb library read queue is full";
        command->result.set_value(std::move(r));
        return future;
    }
    command->metadata.generation =
        command->metadata.generation ? command->metadata.generation : m_generation;
    command->metadata.scopeEpoch =
        command->metadata.scopeEpoch ? command->metadata.scopeEpoch : m_requestedEpoch;
    m_libraryReadCommands.push_back(command);
    ++m_pendingJobs;
    m_wake.notify_one();
    return future;
}
void CatalogDb::processHierarchyQuery(const std::shared_ptr<QueryCommand>& command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbHierarchyResult result;
    result.workerOwned = true;
    const uint64_t operationStartUs = telemetryNowIfEnabled();
    auto finish = [&] {
        PerformanceTelemetry& telemetry = performanceTelemetry();
        const uint64_t endUs = telemetryNowIfEnabled();
        if (operationStartUs != 0 && endUs >= operationStartUs)
            telemetry.recordCatalogDbQuery(endUs - operationStartUs);
        if (command->enqueuedMonotonicUs != 0 && endUs >= command->enqueuedMonotonicUs)
            telemetry.recordCatalogDbQueueWait(endUs - command->enqueuedMonotonicUs);
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
    if (command->kind != QueryCommand::Kind::MediaItemsByIds &&
        command->kind != QueryCommand::Kind::DeleteMediaItemsByIds && command->parentId.empty()) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        result.message = "hierarchy query requires a parent ID";
        finish();
        return;
    }
    if ((command->kind == QueryCommand::Kind::MediaItemsByIds ||
         command->kind == QueryCommand::Kind::DeleteMediaItemsByIds) &&
        (command->itemIds.empty() || command->itemIds.size() > kMaxMetadataByIdRows)) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        result.message = "metadata-by-ID query exceeds bounded limit";
        finish();
        return;
    }

    enum class QueryState : unsigned char
    {
        Valid,
        Cancelled,
        Superseded,
    };
    auto state = [&] {
        if (command->metadata.cancellation && command->metadata.cancellation->load()) {
            return QueryState::Cancelled;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation ||
            command->metadata.scopeEpoch != m_requestedEpoch || !m_scopeConfigured ||
            !m_scopeReady) {
            return QueryState::Superseded;
        }
        return QueryState::Valid;
    };
    auto rejectState = [&](QueryState queryState) {
        result.error = CatalogDbErrorCategory::Superseded;
        if (queryState == QueryState::Cancelled) {
            result.cancelled = true;
            result.message = "CatalogDb query cancelled";
        } else {
            result.superseded = true;
            result.message = "CatalogDb query superseded";
        }
    };
    if (const QueryState queryState = state(); queryState != QueryState::Valid) {
        rejectState(queryState);
        finish();
        return;
    }

    if (command->kind == QueryCommand::Kind::DeleteMediaItemsByIds) {
        std::string deleteSql = "DELETE FROM media_items WHERE id IN (";
        for (std::size_t i = 0; i < kMaxMetadataByIdRows; ++i) {
            if (i)
                deleteSql += ",";
            deleteSql += "?" + std::to_string(i + 1);
        }
        deleteSql += ")";
        sqlite3_stmt* deleteItems = nullptr;
        auto existing = m_statements.find("media_items_delete_by_ids");
        if (existing != m_statements.end()) {
            deleteItems = existing->second;
        } else if (sqlite3_prepare_v2(m_db, deleteSql.c_str(), -1, &deleteItems, nullptr) !=
                   SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            finish();
            return;
        } else {
            m_statements.emplace("media_items_delete_by_ids", deleteItems);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_preparedStatementCount = m_statements.size();
            }
        }
        auto reset = [](sqlite3_stmt* statement) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        };
        reset(deleteItems);
        for (std::size_t i = 0; i < kMaxMetadataByIdRows; ++i) {
            const int rc =
                i < command->itemIds.size()
                    ? sqlite3_bind_text(deleteItems, static_cast<int>(i + 1),
                                        command->itemIds[i].c_str(), -1, SQLITE_TRANSIENT)
                    : sqlite3_bind_null(deleteItems, static_cast<int>(i + 1));
            if (rc != SQLITE_OK) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = sqlite3_errmsg(m_db);
                reset(deleteItems);
                finish();
                return;
            }
        }
        auto rollback = [&] {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
        };
        if (!exec(m_db, "BEGIN IMMEDIATE;", result.message) ||
            sqlite3_step(deleteItems) != SQLITE_DONE) {
            if (result.message.empty())
                result.message = sqlite3_errmsg(m_db);
            result.error = CatalogDbErrorCategory::SqliteError;
            reset(deleteItems);
            rollback();
            finish();
            return;
        }
        reset(deleteItems);
        if (const QueryState queryState = state(); queryState != QueryState::Valid) {
            rollback();
            rejectState(queryState);
            finish();
            return;
        }
        if (!exec(m_db, "COMMIT;", result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            rollback();
            finish();
            return;
        }
        result.success = true;
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
    sqlite3_stmt* query = nullptr;
    sqlite3_stmt* deleteGenres = nullptr;
    sqlite3_stmt* insertGenre = nullptr;
    sqlite3_stmt* deleteImageTags = nullptr;
    sqlite3_stmt* insertImageTag = nullptr;
    sqlite3_stmt* selectGenres = nullptr;
    sqlite3_stmt* selectImageTags = nullptr;
    const bool metadataByIds = command->kind == QueryCommand::Kind::MediaItemsByIds;
    const char* queryName = metadataByIds ? "media_items_by_ids"
                            : command->kind == QueryCommand::Kind::Seasons
                                ? "hierarchy_get_seasons"
                                : "hierarchy_get_episodes";
    std::string querySql;
    if (metadataByIds) {
        querySql =
            "SELECT id, kind, title, overview, production_year, "
            "community_rating, etag, played, progress, "
            "playback_position_ticks, index_number, parent_index_number, "
            "runtime_ticks, series_name, series_id, season_id FROM media_items WHERE id IN (";
        for (std::size_t i = 0; i < kMaxMetadataByIdRows; ++i) {
            if (i)
                querySql += ",";
            querySql += "?" + std::to_string(i + 1);
        }
        querySql += ") ORDER BY id";
    } else if (command->kind == QueryCommand::Kind::Seasons) {
        querySql = "SELECT id, kind, title, overview, production_year, "
                   "community_rating, etag, played, progress, "
                   "playback_position_ticks, index_number, parent_index_number, "
                   "runtime_ticks, series_name, series_id, season_id FROM media_items WHERE "
                   "series_id=?1 AND kind=3 "
                   "ORDER BY index_number, title, id";
    } else {
        querySql = "SELECT id, kind, title, overview, production_year, "
                   "community_rating, etag, played, progress, "
                   "playback_position_ticks, index_number, parent_index_number, "
                   "runtime_ticks, series_name, series_id, season_id FROM media_items WHERE "
                   "season_id=?1 AND kind=4 "
                   "ORDER BY index_number, title, id";
    }
    if (!prepareCached(queryName, querySql.c_str(), query) ||
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
    reset(query);
    if (metadataByIds) {
        for (std::size_t i = 0; i < kMaxMetadataByIdRows; ++i) {
            const int rc =
                i < command->itemIds.size()
                    ? sqlite3_bind_text(query, static_cast<int>(i + 1), command->itemIds[i].c_str(),
                                        -1, SQLITE_TRANSIENT)
                    : sqlite3_bind_null(query, static_cast<int>(i + 1));
            if (rc != SQLITE_OK) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = sqlite3_errmsg(m_db);
                reset(query);
                finish();
                return;
            }
        }
    } else if (sqlite3_bind_text(query, 1, command->parentId.c_str(), -1, SQLITE_TRANSIENT) !=
               SQLITE_OK) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        reset(query);
        finish();
        return;
    }
    const MediaItemCollectionStatements collections{deleteGenres,   insertGenre,  deleteImageTags,
                                                    insertImageTag, selectGenres, selectImageTags};
    for (;;) {
        if (const QueryState queryState = state(); queryState != QueryState::Valid) {
            reset(query);
            rejectState(queryState);
            finish();
            return;
        }
        const int rc = sqlite3_step(query);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            reset(query);
            finish();
            return;
        }
        if (result.items.size() >= kMaxHierarchyQueryRows) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "hierarchy query result exceeds bounded limit";
            reset(query);
            finish();
            return;
        }
        MediaItem item;
        MediaItemSqlError readError = MediaItemSqlError::None;
        if (!readMediaItemScalars(query, item, readError) ||
            !readMediaItemCollections(collections, item, readError)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "hierarchy query row decode failed";
            reset(query);
            finish();
            return;
        }
        result.items.push_back(std::move(item));
    }
    reset(query);
    if (const QueryState queryState = state(); queryState != QueryState::Valid) {
        result.items.clear();
        rejectState(queryState);
        finish();
        return;
    }
    result.success = true;
    finish();
}
void CatalogDb::processMediaPage(const std::shared_ptr<MediaPageCommand>& command)
{
    CatalogDbMediaPageResult result;
    result.workerOwned = true;
    const std::uint64_t queryStartUs = telemetryNowIfEnabled();
    catalogDiagnostic("read_media_page_dequeued");
    auto finish = [&] {
        const std::uint64_t endUs = telemetryNowIfEnabled();
        if (queryStartUs != 0 && endUs >= queryStartUs)
            performanceTelemetry().recordCatalogDbQuery(endUs - queryStartUs);
        if (command->enqueuedMonotonicUs != 0 && endUs >= command->enqueuedMonotonicUs) {
            performanceTelemetry().recordCatalogDbQueueWait(endUs - command->enqueuedMonotonicUs);
        }
        if (result.cancelled || result.superseded) {
            catalogDiagnostic("read_media_page_cancelled");
            performanceTelemetry().addCatalogDbCancelled();
        } else if (result.success) {
            catalogDiagnostic("read_media_page_ready");
            performanceTelemetry().addCatalogDbCompleted();
        } else {
            catalogDiagnostic("read_media_page_failed");
            performanceTelemetry().addCatalogDbFailed();
        }
        command->result.set_value(std::move(result));
    };
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation ||
            command->metadata.scopeEpoch != m_requestedEpoch || !m_scopeReady) {
            result.superseded = true;
            result.error = CatalogDbErrorCategory::Superseded;
            finish();
            return;
        }
    }
    sqlite3_stmt *statement = nullptr, *genres = nullptr, *tags = nullptr, *memberships = nullptr;
    const std::string indexName = "idx_media_" + command->type + "_sort";
    const std::string animeFilter =
        command->filter == CatalogDbMediaPageFilter::Anime
            ? " AND (EXISTS (SELECT 1 FROM library_membership anime_membership "
              "JOIN library_views anime_views ON anime_views.id=anime_membership.view_id "
              "WHERE anime_membership.item_id=media_items.id "
              "AND anime_views.collection_type='tvshows' "
              "AND lower(anime_views.name) LIKE '%anime%') "
              "OR EXISTS (SELECT 1 FROM item_genres anime_genres "
              "WHERE anime_genres.item_id=media_items.id "
              "AND lower(anime_genres.genre)='anime'))"
            : "";
    const std::string sql = "SELECT id,kind,title,overview,production_year,community_rating,"
                            "etag,played,progress,playback_position_ticks,index_number,"
                            "parent_index_number,runtime_ticks,series_name,series_id,season_id "
                            "FROM media_items INDEXED BY " +
                            indexName +
                            " WHERE kind=?1 AND "
                            "EXISTS (SELECT 1 FROM library_membership "
                            "JOIN library_views ON library_views.id=library_membership.view_id "
                            "WHERE library_membership.item_id=media_items.id "
                            "AND library_views.collection_type=?10) AND "
                            "1=1" +
                            animeFilter +
                            " AND "
                            "(?2 < 0 OR (organizational_sort_key>=?3 AND "
                            "organizational_sort_key<?4)) AND (?5=0 OR "
                            "(organizational_sort_key,title,id)>(?6,?7,?8)) "
                            "ORDER BY organizational_sort_key,title,id LIMIT ?9";
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db,
                           "SELECT ordinal,genre FROM item_genres WHERE item_id=?1 "
                           "ORDER BY ordinal",
                           -1, &genres, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db,
                           "SELECT image_type,tag FROM item_image_tags WHERE item_id=?1 "
                           "ORDER BY image_type",
                           -1, &tags, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db,
                           "SELECT library_views.id,library_views.name,"
                           "library_views.collection_type FROM library_membership "
                           "JOIN library_views ON library_views.id=library_membership.view_id "
                           "WHERE library_membership.item_id=?1 "
                           "ORDER BY library_views.ordinal,library_views.id",
                           -1, &memberships, nullptr) != SQLITE_OK) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        sqlite3_finalize(statement);
        sqlite3_finalize(genres);
        sqlite3_finalize(tags);
        sqlite3_finalize(memberships);
        finish();
        return;
    }
    const std::string lower =
        command->letter >= 0 ? std::string(1, static_cast<char>('a' + command->letter)) : "";
    const std::string upper =
        command->letter >= 0 ? std::string(1, static_cast<char>('a' + command->letter + 1)) : "";
    sqlite3_bind_int(statement, 1, command->type == "movie" ? 1 : 2);
    sqlite3_bind_int(statement, 2, command->letter);
    sqlite3_bind_text(statement, 3, lower.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, upper.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 5, command->after.valid ? 1 : 0);
    sqlite3_bind_text(statement, 6, command->after.sortKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 7, command->after.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 8, command->after.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 9, static_cast<sqlite3_int64>(command->limit + 1));
    sqlite3_bind_text(statement, 10, command->type == "movie" ? "movies" : "tvshows", -1,
                      SQLITE_STATIC);
    const MediaItemCollectionStatements collections{nullptr, nullptr, nullptr,
                                                    nullptr, genres,  tags};
    while (sqlite3_step(statement) == SQLITE_ROW) {
        if (command->metadata.cancellation && command->metadata.cancellation->load()) {
            result.cancelled = true;
            break;
        }
        MediaItem item;
        MediaItemSqlError e = MediaItemSqlError::None;
        if (!readMediaItemScalars(statement, item, e) ||
            !readMediaItemCollections(collections, item, e)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            break;
        }
        sqlite3_reset(memberships);
        sqlite3_clear_bindings(memberships);
        sqlite3_bind_text(memberships, 1, item.id.c_str(), -1, SQLITE_TRANSIENT);
        int membershipRc = SQLITE_DONE;
        while ((membershipRc = sqlite3_step(memberships)) == SQLITE_ROW) {
            CatalogDbMediaPageMembership membership;
            const char* viewId = reinterpret_cast<const char*>(sqlite3_column_text(memberships, 0));
            const char* viewName =
                reinterpret_cast<const char*>(sqlite3_column_text(memberships, 1));
            const char* collectionType =
                reinterpret_cast<const char*>(sqlite3_column_text(memberships, 2));
            membership.viewId = viewId ? viewId : "";
            membership.viewName = viewName ? viewName : "";
            membership.collectionType = collectionType ? collectionType : "";
            result.membershipsByItem[item.id].push_back(std::move(membership));
        }
        if (membershipRc != SQLITE_DONE) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            break;
        }
        if (result.items.size() < command->limit)
            result.items.push_back(std::move(item));
        else
            result.hasMore = true;
    }
    if (!result.items.empty()) {
        const auto& item = result.items.back();
        result.next.sortKey = catalog::organizationalSortKey(item.title);
        result.next.title = item.title;
        result.next.id = item.id;
        result.next.valid = true;
    }
    sqlite3_finalize(statement);
    sqlite3_finalize(genres);
    sqlite3_finalize(tags);
    sqlite3_finalize(memberships);
    if (!result.cancelled && result.error == CatalogDbErrorCategory::None)
        result.success = true;
    finish();
}
void CatalogDb::processLibraryRead(const std::shared_ptr<LibraryReadCommand>& command)
{
    // This whole-snapshot path is retained for compatibility/tests. The
    // normal Home flow renders bounded library pages and receives ephemeral
    // rail responses from Jellyfin; it must not depend on home_items.
    CatalogCompatibilityReadResult result;
    result.workerOwned = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation ||
            (command->metadata.scopeEpoch && (command->metadata.scopeEpoch != m_requestedEpoch ||
                                              !m_scopeConfigured || !m_scopeReady))) {
            result.superseded = true;
            result.error = CatalogDbErrorCategory::Superseded;
            command->result.set_value(std::move(result));
            return;
        }
    }
    sqlite3_stmt *views = nullptr, *items = nullptr, *home = nullptr, *sg = nullptr, *st = nullptr;
    const char* cols = "id,kind,title,overview,production_year,community_rating,etag,played,"
                       "progress,playback_position_ticks,index_number,parent_index_number,runtime_"
                       "ticks,series_name,series_id,season_id";
    if (sqlite3_prepare_v2(m_db,
                           "SELECT id,name,collection_type FROM library_views ORDER BY ordinal,id",
                           -1, &views, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db,
                           (std::string("SELECT ") + cols +
                            " FROM media_items JOIN library_membership ON "
                            "media_items.id=library_membership.item_id WHERE view_id=? ORDER BY "
                            "library_membership.ordinal,library_membership.item_id")
                               .c_str(),
                           -1, &items, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db,
                           (std::string("SELECT ") + cols +
                            ",row_kind FROM media_items JOIN home_items ON "
                            "media_items.id=home_items.item_id ORDER BY row_kind,ordinal,item_id")
                               .c_str(),
                           -1, &home, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db,
                           "SELECT ordinal,genre FROM item_genres WHERE item_id=? ORDER BY ordinal",
                           -1, &sg, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(
            m_db, "SELECT image_type,tag FROM item_image_tags WHERE item_id=? ORDER BY image_type",
            -1, &st, nullptr) != SQLITE_OK) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        sqlite3_finalize(views);
        sqlite3_finalize(items);
        sqlite3_finalize(home);
        sqlite3_finalize(sg);
        sqlite3_finalize(st);
        command->result.set_value(std::move(result));
        return;
    }
    const MediaItemCollectionStatements collections{nullptr, nullptr, nullptr, nullptr, sg, st};
    auto cancelled = [&] {
        return command->metadata.cancellation && command->metadata.cancellation->load();
    };
    while (sqlite3_step(views) == SQLITE_ROW) {
        if (cancelled()) {
            result.cancelled = true;
            break;
        }
        CachedLibraryView view;
        view.id = (const char*)sqlite3_column_text(views, 0);
        view.name = (const char*)sqlite3_column_text(views, 1);
        view.collectionType = (const char*)sqlite3_column_text(views, 2);
        sqlite3_reset(items);
        sqlite3_clear_bindings(items);
        sqlite3_bind_text(items, 1, view.id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(items) == SQLITE_ROW) {
            if (cancelled()) {
                result.cancelled = true;
                break;
            }
            MediaItem item;
            MediaItemSqlError e = MediaItemSqlError::None;
            if (!readMediaItemScalars(items, item, e) ||
                !readMediaItemCollections(collections, item, e)) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = sqlite3_errmsg(m_db);
                sqlite3_finalize(views);
                sqlite3_finalize(items);
                sqlite3_finalize(sg);
                sqlite3_finalize(st);
                command->result.set_value(std::move(result));
                return;
            }
            view.items.push_back(std::move(item));
        }
        if (result.cancelled)
            break;
        if (view.collectionType == "tvshows")
            result.snapshot.shows.push_back(std::move(view));
        else
            result.snapshot.movies.push_back(std::move(view));
    }
    if (!result.cancelled)
        while (sqlite3_step(home) == SQLITE_ROW) {
            if (cancelled()) {
                result.cancelled = true;
                break;
            }
            const char* kind = (const char*)sqlite3_column_text(home, 16);
            MediaItem item;
            MediaItemSqlError e = MediaItemSqlError::None;
            if (!readMediaItemScalars(home, item, e) ||
                !readMediaItemCollections(collections, item, e)) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = sqlite3_errmsg(m_db);
                sqlite3_finalize(views);
                sqlite3_finalize(items);
                sqlite3_finalize(home);
                sqlite3_finalize(sg);
                sqlite3_finalize(st);
                command->result.set_value(std::move(result));
                return;
            }
            if (kind && std::string(kind) == "continue_watching")
                result.snapshot.continueWatching.push_back(std::move(item));
            else if (kind && std::string(kind) == "recently_added")
                result.snapshot.recentlyAdded.push_back(std::move(item));
        }
    if (cancelled())
        result.cancelled = true;
    sqlite3_finalize(views);
    sqlite3_finalize(items);
    sqlite3_finalize(home);
    sqlite3_finalize(sg);
    sqlite3_finalize(st);
    if (!result.cancelled)
        result.success = true;
    command->result.set_value(std::move(result));
}
} // namespace miyoofin
