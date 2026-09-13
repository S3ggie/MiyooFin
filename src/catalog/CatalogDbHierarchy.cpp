#include "CatalogDb.hpp"
#include "CatalogDbInternal.hpp"

namespace miyoofin {
using namespace catalog_db_internal;

std::future<CatalogDbReconcileResult> CatalogDb::reconcileSeries(
    const std::vector<MediaItem> &series, bool authoritative,
    const CatalogDbJobMetadata &metadata)
{
    return enqueueReconcile(series, authoritative, metadata, -1);
}
std::future<CatalogDbReconcileResult> CatalogDb::reconcileSeriesForTest(
    const std::vector<MediaItem> &series, bool authoritative,
    int failAfterRows)
{
    return enqueueReconcile(series, authoritative, {}, failAfterRows);
}
std::future<CatalogDbReconcileResult> CatalogDb::enqueueReconcile(
    const std::vector<MediaItem> &series, bool authoritative,
    const CatalogDbJobMetadata &metadata, int failAfterRows)
{
    auto command = std::make_shared<ReconcileCommand>();
    command->series = series;
    command->authoritative = authoritative;
    command->metadata = metadata;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    command->failAfterRows = failAfterRows;
    std::future<CatalogDbReconcileResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbReconcileResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbReconcileResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb reconciliation cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbReconcileResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb reconciliation queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_reconcileCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}
void CatalogDb::processReconcile(
    const std::shared_ptr<ReconcileCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbReconcileResult result;
    result.workerOwned = true;
    result.authoritative = command->authoritative;
    uint64_t transactionStartUs = 0;
    bool transactionActive = false;
    auto finish = [&] {
        PerformanceTelemetry &telemetry = performanceTelemetry();
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
    if (!command->authoritative) {
        result.skipped = true;
        result.success = true;
        result.message = "non-authoritative reconciliation skipped";
        finish();
        return;
    }
    if (!validateReconcileInput(command->series, result.message)) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        finish();
        return;
    }

    auto stateIsValid = [&] {
        if (command->metadata.cancellation
            && command->metadata.cancellation->load()) {
            result.cancelled = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "CatalogDb reconciliation cancelled";
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation
            || command->metadata.scopeEpoch != m_requestedEpoch
            || !m_scopeConfigured || !m_scopeReady) {
            result.superseded = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "CatalogDb reconciliation superseded";
            return false;
        }
        return true;
    };
    if (!stateIsValid()) {
        finish();
        return;
    }

    if (!exec(m_db,
              "CREATE TEMP TABLE IF NOT EXISTS catalog_reconcile_ids("
              "id TEXT PRIMARY KEY NOT NULL);",
              result.message)) {
        result.error = CatalogDbErrorCategory::SqliteError;
        finish();
        return;
    }
    auto prepareCached = [&](const char *name, const char *sql,
                             sqlite3_stmt *&statement) {
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
    sqlite3_stmt *upsert = nullptr;
    sqlite3_stmt *clearIds = nullptr;
    sqlite3_stmt *stageId = nullptr;
    sqlite3_stmt *deleteAbsent = nullptr;
    sqlite3_stmt *deleteGenres = nullptr;
    sqlite3_stmt *insertGenre = nullptr;
    sqlite3_stmt *deleteImageTags = nullptr;
    sqlite3_stmt *insertImageTag = nullptr;
    if (!prepareCached(
            "hierarchy_upsert_item",
            "INSERT INTO media_items("
            "id, kind, title, overview, production_year, community_rating,"
            "etag, played, progress, playback_position_ticks, index_number,"
            "parent_index_number, runtime_ticks, series_name, series_id,"
            "season_id, art_r, art_g, art_b) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
            "?13, ?14, ?15, ?16, ?17, ?18, ?19) "
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
            "season_id=excluded.season_id, art_r=excluded.art_r, "
            "art_g=excluded.art_g, art_b=excluded.art_b",
            upsert)
        || !prepareCached("reconcile_clear_ids",
                          "DELETE FROM catalog_reconcile_ids", clearIds)
        || !prepareCached(
               "reconcile_stage_id",
               "INSERT INTO catalog_reconcile_ids(id) VALUES(?1)", stageId)
        || !prepareCached(
               "reconcile_delete_absent",
               "DELETE FROM media_items WHERE kind=2 AND id NOT IN "
               "(SELECT id FROM catalog_reconcile_ids)",
               deleteAbsent)
        || !prepareCached("media_item_genres_delete",
                          "DELETE FROM item_genres WHERE item_id=?1",
                          deleteGenres)
        || !prepareCached(
               "media_item_genre_insert",
               "INSERT INTO item_genres(item_id, ordinal, genre) "
               "VALUES(?1, ?2, ?3)",
               insertGenre)
        || !prepareCached("media_item_image_tags_delete",
                          "DELETE FROM item_image_tags WHERE item_id=?1",
                          deleteImageTags)
        || !prepareCached(
               "media_item_image_tag_insert",
               "INSERT INTO item_image_tags(item_id, image_type, tag) "
               "VALUES(?1, ?2, ?3)",
               insertImageTag)) {
        finish();
        return;
    }
    auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };
    auto rollback = [&] {
        std::string ignored;
        exec(m_db, "ROLLBACK;", ignored);
    };
    auto executeNoParameter = [&](sqlite3_stmt *statement) {
        reset(statement);
        const bool ok = sqlite3_step(statement) == SQLITE_DONE;
        if (!ok) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(statement);
        return ok;
    };
    const MediaItemCollectionStatements collections{
        deleteGenres, insertGenre, deleteImageTags, insertImageTag};
    auto writeSeries = [&](const MediaItem &item) {
        if (!stateIsValid()) {
            return false;
        }
        MediaItemSqlError error = MediaItemSqlError::None;
        bool existed = false;
        if (performanceTelemetry().enabledFast()
            && !mediaItemRowExists(m_db, item.id, existed, result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            performanceTelemetry().recordCatalogDbSqliteError(
                sqlite3_extended_errcode(m_db));
            return false;
        }
        reset(upsert);
        const bool ok = bindMediaItemScalars(upsert, item, error)
            && sqlite3_step(upsert) == SQLITE_DONE
            && maintainOrganizationalSortKey(m_db, item, result.message);
        if (!ok) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            performanceTelemetry().recordCatalogDbSqliteError(
                sqlite3_extended_errcode(m_db));
            reset(upsert);
            return false;
        }
        performanceTelemetry().addCatalogDbRows(existed ? 0u : 1u,
                                                existed ? 1u : 0u, 0u);
        reset(upsert);
        if (!replaceMediaItemCollections(collections, item, error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "reconciliation collection write failed";
            return false;
        }
        ++result.seriesUpserted;
        if (command->failAfterRows >= 0
            && result.seriesUpserted >= static_cast<std::size_t>(
                                            command->failAfterRows)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "injected reconciliation failure";
            return false;
        }
        reset(stageId);
        const bool staged = sqlite3_bind_text(stageId, 1, item.id.c_str(), -1,
                                              SQLITE_TRANSIENT) == SQLITE_OK
            && sqlite3_step(stageId) == SQLITE_DONE;
        if (!staged) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(stageId);
        return staged;
    };

    const uint64_t beginUs = telemetryNowIfEnabled();
    const bool began = exec(m_db, "BEGIN IMMEDIATE;", result.message);
    if (began) {
        transactionStartUs = beginUs;
        transactionActive = true;
    }
    if (!began
        || !executeNoParameter(clearIds)) {
        rollback();
        finish();
        return;
    }
    for (const auto &item : command->series) {
        if (!writeSeries(item)) {
            rollback();
            finish();
            return;
        }
    }
    if (!stateIsValid()) {
        rollback();
        finish();
        return;
    }
    reset(deleteAbsent);
    if (sqlite3_step(deleteAbsent) != SQLITE_DONE) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        reset(deleteAbsent);
        rollback();
        finish();
        return;
    }
    result.seriesDeleted = static_cast<std::size_t>(sqlite3_changes(m_db));
    performanceTelemetry().addCatalogDbRows(
        0, 0, static_cast<uint32_t>(result.seriesDeleted));
    reset(deleteAbsent);
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
            performanceTelemetry().recordCatalogDbTransaction(
                transactionEndUs - transactionStartUs);
    }
    result.success = true;
    finish();
}
void CatalogDb::processTopLevelSync(
    const std::shared_ptr<TopLevelSyncCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbTopLevelSyncResult result; result.workerOwned = true;
    std::lock_guard<std::mutex> lock(m_mutex);
    result.generation = command->generation;
    if (!m_scopeConfigured || !m_scopeReady || command->metadata.scopeEpoch == 0
        || command->metadata.scopeEpoch != m_requestedEpoch
        || command->generation == 0) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "top-level sync scope is not current";
        command->result.set_value(std::move(result)); return;
    }
    if (!m_db) {
        result.error = CatalogDbErrorCategory::OpenFailed;
        result.message = "top-level sync database is not open";
        command->result.set_value(std::move(result)); return;
    }
    std::string error;
    const char *const createSql[] = {
        "CREATE TEMP TABLE IF NOT EXISTS top_level_sync_views (generation INTEGER NOT NULL, id TEXT NOT NULL, name TEXT NOT NULL, collection_type TEXT NOT NULL, ordinal INTEGER NOT NULL, PRIMARY KEY(generation, id))",
        "CREATE TEMP TABLE IF NOT EXISTS top_level_sync_membership (generation INTEGER NOT NULL, view_id TEXT NOT NULL, item_id TEXT NOT NULL, ordinal INTEGER NOT NULL, PRIMARY KEY(generation, view_id, item_id))",
        "CREATE TEMP TABLE IF NOT EXISTS top_level_sync_active (generation INTEGER NOT NULL PRIMARY KEY)",
    };
    for (const char *sql : createSql) {
        if (!exec(m_db, sql, error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = error;
            command->result.set_value(std::move(result)); return;
        }
    }
    if (command->begin) {
        if (!exec(m_db, "DELETE FROM top_level_sync_views; DELETE FROM top_level_sync_membership; DELETE FROM top_level_sync_active;", error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = error;
            command->result.set_value(std::move(result)); return;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "INSERT INTO top_level_sync_active(generation) VALUES(?1)",
                -1, &statement, nullptr) != SQLITE_OK
            || sqlite3_bind_int64(statement, 1,
                static_cast<sqlite3_int64>(command->generation)) != SQLITE_OK
            || sqlite3_step(statement) != SQLITE_DONE) {
            if (statement) sqlite3_finalize(statement);
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            command->result.set_value(std::move(result)); return;
        }
        sqlite3_finalize(statement);
        m_topLevelSyncGeneration = command->generation;
        m_activeTopLevelSyncGeneration = command->generation;
        result.success = true;
    } else if (command->finalize) {
        if (m_activeTopLevelSyncGeneration != command->generation) {
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "top-level sync generation is not active";
            command->result.set_value(std::move(result)); return;
        }
        const std::string copyViews = "INSERT INTO library_views(id,name,collection_type,ordinal) SELECT id,name,collection_type,ordinal FROM top_level_sync_views WHERE generation=" + std::to_string(command->generation) + " ORDER BY ordinal,id;";
        const std::string copyMembership = "INSERT INTO library_membership(view_id,item_id,ordinal) SELECT view_id,item_id,ordinal FROM top_level_sync_membership WHERE generation=" + std::to_string(command->generation) + " ORDER BY view_id,ordinal,item_id;";
        if (!exec(m_db, "BEGIN IMMEDIATE;", error)
            || !exec(m_db, "DELETE FROM library_membership; DELETE FROM library_views;", error)
            || !exec(m_db, copyViews.c_str(), error)
            || !exec(m_db, copyMembership.c_str(), error)
            || !exec(m_db, "COMMIT;", error)) {
            std::string ignored; exec(m_db, "ROLLBACK;", ignored);
            result.error = CatalogDbErrorCategory::SqliteError; result.message = error;
            command->result.set_value(std::move(result)); return;
        }
        exec(m_db, "DELETE FROM top_level_sync_views; DELETE FROM top_level_sync_membership; DELETE FROM top_level_sync_active;", error);
        m_activeTopLevelSyncGeneration = 0;
        result.success = true;
    } else if (m_activeTopLevelSyncGeneration == command->generation) {
        m_activeTopLevelSyncGeneration = 0;
        if (!exec(m_db, "DELETE FROM top_level_sync_active", error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = error;
            command->result.set_value(std::move(result)); return;
        }
        result.success = true;
    } else {
        result.success = true;
    }
    command->result.set_value(std::move(result));
}
void CatalogDb::processLibrarySeed(
    const std::shared_ptr<LibrarySeedCommand> &command)
{
    CatalogCompatibilitySeedResult result; result.workerOwned = true;
    auto stale = [&] { std::lock_guard<std::mutex> lock(m_mutex); return
        command->metadata.generation != m_generation ||
        (command->metadata.scopeEpoch != 0 && (command->metadata.scopeEpoch != m_requestedEpoch || !m_scopeConfigured || !m_scopeReady)); };
    if (command->metadata.cancellation && command->metadata.cancellation->load()) { result.cancelled=true; result.error=CatalogDbErrorCategory::Superseded; result.message="library seed cancelled"; command->result.set_value(std::move(result)); return; }
    if (stale()) { result.superseded=true; result.error=CatalogDbErrorCategory::Superseded; result.message="library seed superseded"; command->result.set_value(std::move(result)); return; }
    std::map<std::string, MediaItem> canonical;
    std::vector<MediaItem> items;
    auto collect = [&](const std::vector<CachedLibraryView> &views) {
        for (const auto &view : views) for (const auto &item : view.items) {
            if (item.id.empty() || (item.type != "movie" && item.type != "show")) return false;
            auto found = canonical.find(item.id);
            if (found != canonical.end()) { if (!mediaItemsEquivalentForCatalog(found->second, item)) return false; }
            else { canonical.emplace(item.id, item); items.push_back(item); }
        } return true;
    };
    if (!collect(command->request.snapshot.movies) || !collect(command->request.snapshot.shows)) { result.error=CatalogDbErrorCategory::CorruptOrIo; result.message="invalid library snapshot"; command->result.set_value(std::move(result)); return; }
    auto collectHome = [&](const std::vector<MediaItem> &row) { for (const auto &item : row) { if(item.id.empty() || (item.type!="movie"&&item.type!="show"&&item.type!="episode")) return false; auto found=canonical.find(item.id); if(found!=canonical.end()){if(!mediaItemsEquivalentForCatalog(found->second,item))return false;} else {canonical.emplace(item.id,item);items.push_back(item);} } return true; };
    if(!collectHome(command->request.snapshot.continueWatching)||!collectHome(command->request.snapshot.recentlyAdded)){result.error=CatalogDbErrorCategory::CorruptOrIo;result.message="invalid Home snapshot";command->result.set_value(std::move(result));return;}
    auto execSeed = [&](const std::string &sql) { char *err=nullptr; int rc=sqlite3_exec(m_db,sql.c_str(),nullptr,nullptr,&err); if(rc!=SQLITE_OK){result.message=err?err:sqlite3_errmsg(m_db); sqlite3_free(err); return false;} return true; };
    if (!execSeed("BEGIN IMMEDIATE;")) { result.error=CatalogDbErrorCategory::SqliteError; command->result.set_value(std::move(result)); return; }
    auto rollback = [&] { char *e=nullptr; sqlite3_exec(m_db,"ROLLBACK;",nullptr,nullptr,&e); sqlite3_free(e); };
    sqlite3_stmt *upsert=nullptr;
    sqlite3_stmt *deleteGenres=nullptr,*insertGenre=nullptr,*deleteTags=nullptr,*insertTag=nullptr;
    const char *sql="INSERT INTO media_items(id,kind,title,overview,production_year,community_rating,etag,played,progress,playback_position_ticks,index_number,parent_index_number,runtime_ticks,series_name,series_id,season_id,art_r,art_g,art_b) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET kind=excluded.kind,title=excluded.title,overview=excluded.overview,production_year=excluded.production_year,community_rating=excluded.community_rating,etag=excluded.etag,played=excluded.played,progress=excluded.progress,playback_position_ticks=excluded.playback_position_ticks,index_number=excluded.index_number,parent_index_number=excluded.parent_index_number,runtime_ticks=excluded.runtime_ticks,series_name=excluded.series_name,series_id=excluded.series_id,season_id=excluded.season_id,art_r=excluded.art_r,art_g=excluded.art_g,art_b=excluded.art_b";
    if (sqlite3_prepare_v2(m_db,sql,-1,&upsert,nullptr)!=SQLITE_OK
        || sqlite3_prepare_v2(m_db,"DELETE FROM item_genres WHERE item_id=?",-1,&deleteGenres,nullptr)!=SQLITE_OK
        || sqlite3_prepare_v2(m_db,"INSERT INTO item_genres(item_id,ordinal,genre) VALUES(?,?,?)",-1,&insertGenre,nullptr)!=SQLITE_OK
        || sqlite3_prepare_v2(m_db,"DELETE FROM item_image_tags WHERE item_id=?",-1,&deleteTags,nullptr)!=SQLITE_OK
        || sqlite3_prepare_v2(m_db,"INSERT INTO item_image_tags(item_id,image_type,tag) VALUES(?,?,?)",-1,&insertTag,nullptr)!=SQLITE_OK) { result.error=CatalogDbErrorCategory::SqliteError; result.message=sqlite3_errmsg(m_db); rollback(); command->result.set_value(std::move(result)); return; }
    const MediaItemCollectionStatements collections{deleteGenres,insertGenre,deleteTags,insertTag};
    int writes=0;
    for (const auto &item : items) { MediaItemSqlError e=MediaItemSqlError::None; sqlite3_reset(upsert); sqlite3_clear_bindings(upsert); if(!bindMediaItemScalars(upsert,item,e)||sqlite3_step(upsert)!=SQLITE_DONE||!maintainOrganizationalSortKey(m_db,item,result.message)||!replaceMediaItemCollections(collections,item,e)){result.error=CatalogDbErrorCategory::SqliteError;result.message=sqlite3_errmsg(m_db);sqlite3_finalize(upsert);sqlite3_finalize(deleteGenres);sqlite3_finalize(insertGenre);sqlite3_finalize(deleteTags);sqlite3_finalize(insertTag);rollback();command->result.set_value(std::move(result));return;} ++result.itemsUpserted; if(command->failAfterWrites>=0&&++writes>=command->failAfterWrites){result.error=CatalogDbErrorCategory::SqliteError;result.message="injected library seed failure";sqlite3_finalize(upsert);sqlite3_finalize(deleteGenres);sqlite3_finalize(insertGenre);sqlite3_finalize(deleteTags);sqlite3_finalize(insertTag);rollback();command->result.set_value(std::move(result));return;} }
    sqlite3_finalize(upsert); sqlite3_finalize(deleteGenres); sqlite3_finalize(insertGenre); sqlite3_finalize(deleteTags); sqlite3_finalize(insertTag);
    // This is the compatibility-only writer for legacy home_items.
    // Normal top-level synchronization never enters this path.
    if (!execSeed("DELETE FROM library_membership; DELETE FROM library_views; DELETE FROM home_items;")) { result.error=CatalogDbErrorCategory::SqliteError; rollback(); command->result.set_value(std::move(result)); return; }
    auto addView = [&](const CachedLibraryView &v, int ordinal) { sqlite3_stmt *s=nullptr; if(sqlite3_prepare_v2(m_db,"INSERT INTO library_views(id,name,collection_type,ordinal) VALUES(?,?,?,?)",-1,&s,nullptr)!=SQLITE_OK)return false; sqlite3_bind_text(s,1,v.id.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,v.name.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,3,v.collectionType.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,4,ordinal);bool ok=sqlite3_step(s)==SQLITE_DONE;sqlite3_finalize(s);if(!ok)return false; for(size_t i=0;i<v.items.size();++i){if(!execSeed("INSERT INTO library_membership(view_id,item_id,ordinal) VALUES('"+v.id+"','"+v.items[i].id+"',"+std::to_string(i)+")"))return false;} ++result.viewsWritten;return true; };
    for(size_t i=0;i<command->request.snapshot.movies.size();++i) if(!addView(command->request.snapshot.movies[i],i)){result.error=CatalogDbErrorCategory::SqliteError;rollback();command->result.set_value(std::move(result));return;}
    for(size_t i=0;i<command->request.snapshot.shows.size();++i) if(!addView(command->request.snapshot.shows[i],i+command->request.snapshot.movies.size())){result.error=CatalogDbErrorCategory::SqliteError;rollback();command->result.set_value(std::move(result));return;}
    auto addHome=[&](const char *kind,const std::vector<MediaItem>&v){for(size_t i=0;i<v.size();++i)if(!execSeed("INSERT INTO home_items(row_kind,item_id,ordinal) VALUES('"+std::string(kind)+"','"+v[i].id+"',"+std::to_string(i)+")"))return false;result.homeItemsWritten+=v.size();return true;};
    if(!addHome("continue_watching",command->request.snapshot.continueWatching)||!addHome("recently_added",command->request.snapshot.recentlyAdded)||stale()||!execSeed("COMMIT;")){result.error=CatalogDbErrorCategory::Superseded;rollback();command->result.set_value(std::move(result));return;}
    result.success=true; command->result.set_value(std::move(result));
}
} // namespace miyoofin
