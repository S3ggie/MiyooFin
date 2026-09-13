#include "LibrarySync.hpp"
#include "OfflineLibraryQuery.hpp"
#include "../download/DownloadStore.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/JellyfinLibraryEvents.hpp"
#include "../net/RouteRequest.hpp"
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <future>
#include <set>
#include <unistd.h>

namespace miyoofin {
namespace library {
namespace {

std::future<CatalogDbHierarchyWriteResult> rejectedHierarchyWrite(
    const char *message)
{
    std::promise<CatalogDbHierarchyWriteResult> promise;
    CatalogDbHierarchyWriteResult result;
    result.error = CatalogDbErrorCategory::ConfigurationFailed;
    result.message = message;
    promise.set_value(std::move(result));
    return promise.get_future();
}

}
library::LibrarySync::LibrarySync(Session session, std::shared_ptr<CatalogDb> db,
                         std::uint64_t scopeEpoch)
    : m_session(std::move(session)), m_db(std::move(db)),
      m_cancel(std::make_shared<std::atomic_bool>(false)),
      m_offlineGeneration(std::make_shared<std::atomic<std::uint64_t>>(0)) {
    m_metadata.scopeEpoch = scopeEpoch;
    m_metadata.cancellation = m_cancel;
}
}
library::LibrarySync::~LibrarySync() { cancel(); }
std::uint64_t library::LibrarySync::nextGeneration() { return ++m_generation; }
std::future<CatalogDbTopLevelSyncResult> library::LibrarySync::begin(std::uint64_t generation) {
    m_inFlight = true; m_success = false;
    return m_db->beginTopLevelSync(generation, m_metadata);
}
std::future<CatalogDbMediaPageUpsertResult> library::LibrarySync::stage(const CatalogDbMediaPageWrite &page) {
    return m_db->upsertMediaPage(page, m_metadata);
}
std::future<CatalogDbTopLevelSyncResult> library::LibrarySync::finalize(std::uint64_t generation) {
    m_success = true; m_inFlight = false;
    return m_db->finalizeTopLevelSync(generation, m_metadata);
}
std::future<CatalogDbTopLevelSyncResult> library::LibrarySync::abort(std::uint64_t generation) {
    m_inFlight = false;
    return m_db->abortTopLevelSync(generation, m_metadata);
}
std::future<library::HierarchyRefreshResult>
library::LibrarySync::refreshSeasons(
    const MediaItem &series,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    const Session session = m_session;
    const auto db = m_db;
    const CatalogDbJobMetadata metadata = m_metadata;
    const auto serviceCancellation = m_cancel;
    const auto operationCancellation = cancellation;
    return std::async(std::launch::async,
        [session, db, metadata, serviceCancellation, operationCancellation,
         series] {
            HierarchyRefreshResult result;
            const auto cancelled = [&] {
                return (serviceCancellation && serviceCancellation->load())
                    || (operationCancellation && operationCancellation->load());
            };
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "season refresh cancelled";
                return result;
            }

            std::string error;
            std::vector<MediaItem> seasons;
            const bool networkOk = RouteRequest(session).run(
                [&](const std::string &base) {
                    return JellyfinApi::getSeasons(
                        base, session.accessToken, session.userId,
                        session.deviceId, series.id, seasons, error,
                        operationCancellation
                            ? operationCancellation.get()
                            : serviceCancellation.get());
                }, error);
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "season refresh cancelled";
                return result;
            }
            if (!networkOk) {
                result.message = error;
                return result;
            }

            if (db) {
                CatalogDbJobMetadata writeMetadata = metadata;
                writeMetadata.cancellation = operationCancellation
                    ? operationCancellation : serviceCancellation;
                std::map<std::string, std::vector<MediaItem>> episodesBySeason;
                for (const auto &season : seasons)
                    episodesBySeason.emplace(season.id,
                                             std::vector<MediaItem>{});
                const auto written = db->stageSeriesHierarchy(
                    series, seasons, episodesBySeason, 0,
                    static_cast<std::int64_t>(std::time(nullptr)) * 1000,
                    false, writeMetadata).get();
                if (written.cancelled || written.superseded || !written.success) {
                    result.cancelled = written.cancelled;
                    result.superseded = written.superseded;
                    result.error = written.error;
                    result.message = written.message;
                    return result;
                }
            }
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "season refresh cancelled";
                return result;
            }
            result.success = true;
            result.items = std::move(seasons);
            return result;
        });
}

std::future<library::HierarchyRefreshResult>
library::LibrarySync::refreshEpisodes(
    const MediaItem &series, const MediaItem &season,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    const Session session = m_session;
    const auto db = m_db;
    const CatalogDbJobMetadata metadata = m_metadata;
    const auto serviceCancellation = m_cancel;
    const auto operationCancellation = cancellation;
    return std::async(std::launch::async,
        [session, db, metadata, serviceCancellation, operationCancellation,
         series, season] {
            HierarchyRefreshResult result;
            const auto cancelled = [&] {
                return (serviceCancellation && serviceCancellation->load())
                    || (operationCancellation && operationCancellation->load());
            };
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "episode refresh cancelled";
                return result;
            }

            std::string error;
            std::vector<MediaItem> episodes;
            const bool networkOk = RouteRequest(session).run(
                [&](const std::string &base) {
                    return JellyfinApi::getEpisodes(
                        base, session.accessToken, session.userId,
                        session.deviceId, series.id, season.id, episodes,
                        error,
                        operationCancellation
                            ? operationCancellation.get()
                            : serviceCancellation.get());
                }, error);
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "episode refresh cancelled";
                return result;
            }
            if (!networkOk) {
                result.message = error;
                return result;
            }

            if (db) {
                CatalogDbJobMetadata writeMetadata = metadata;
                writeMetadata.cancellation = operationCancellation
                    ? operationCancellation : serviceCancellation;
                const auto written = db->reconcileSeasonHierarchy(
                    series, season, episodes, 0,
                    static_cast<std::int64_t>(std::time(nullptr)) * 1000,
                    writeMetadata).get();
                if (written.cancelled || written.superseded || !written.success) {
                    result.cancelled = written.cancelled;
                    result.superseded = written.superseded;
                    result.error = written.error;
                    result.message = written.message;
                    return result;
                }
            }
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "episode refresh cancelled";
                return result;
            }
            result.success = true;
            result.items = std::move(episodes);
            return result;
        });
}

std::future<library::ChangedCatalogResult>
library::LibrarySync::catchUpChangedCatalog(
    std::int64_t sinceMs,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    const Session session = m_session;
    const auto db = m_db;
    const CatalogDbJobMetadata metadata = m_metadata;
    const auto serviceCancellation = m_cancel;
    const auto operationCancellation = cancellation;
    return std::async(std::launch::async,
        [session, db, metadata, serviceCancellation, operationCancellation,
         sinceMs] {
            ChangedCatalogResult result;
            const auto effectiveCancellation = operationCancellation
                ? operationCancellation : serviceCancellation;
            const auto cancelled = [&] {
                return effectiveCancellation && effectiveCancellation->load();
            };
            if (!db) {
                result.error = CatalogDbErrorCategory::ScopeNotReady;
                result.message = "CatalogDb service is unavailable";
                return result;
            }
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "changed catalog catch-up cancelled";
                return result;
            }

            std::vector<MediaItem> changed;
            std::string error;
            const bool networkOk = RouteRequest(session).run(
                [&](const std::string &base) {
                    return JellyfinApi::getChangedCatalogItems(
                        base, session.accessToken, session.userId,
                        session.deviceId, sinceMs, changed, error,
                        effectiveCancellation
                            ? effectiveCancellation.get() : nullptr);
                }, error);
            if (!networkOk) {
                result.cancelled = cancelled();
                result.error = result.cancelled
                    ? CatalogDbErrorCategory::Superseded
                    : CatalogDbErrorCategory::None;
                result.message = error;
                return result;
            }
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "changed catalog catch-up cancelled";
                return result;
            }

            CatalogDbJobMetadata writeMetadata = metadata;
            writeMetadata.cancellation = effectiveCancellation;
            if (!changed.empty()) {
                CatalogDbMediaPageWrite page;
                page.items = std::move(changed);
                // An empty view ID deliberately updates only media metadata;
                // membership remains authoritative to the full sync path.
                const auto written = db->upsertMediaPage(page, writeMetadata).get();
                if (!written.success) {
                    result.cancelled = written.cancelled;
                    result.superseded = written.superseded;
                    result.error = written.error;
                    result.message = written.message;
                    return result;
                }
                result.itemsUpserted = written.rowsWritten;
            }

            // The metadata transaction is the catch-up linearization point.
            // Finish the checkpoint with cancellation detached so a cancel
            // arriving after that commit cannot leave a successful update
            // paired with an older restart boundary.
            CatalogDbJobMetadata checkpointMetadata = writeMetadata;
            checkpointMetadata.cancellation.reset();
            const auto state = db->readSyncState(
                false, 0, 0, checkpointMetadata).get();
            if (!state.success) {
                result.error = state.error;
                result.message = state.message;
                return result;
            }
            const std::int64_t nowMs =
                static_cast<std::int64_t>(std::time(nullptr)) * 1000;
            const std::int64_t checkpointMs = nowMs > state.lastSuccessfulMs
                ? nowMs : state.lastSuccessfulMs;
            const auto checkpoint = db->writeSyncState(
                checkpointMs, state.lastReconcileMs,
                state.committedGeneration, checkpointMetadata).get();
            if (!checkpoint.success) {
                result.error = checkpoint.error;
                result.message = checkpoint.message;
                return result;
            }
            result.success = true;
            result.checkpointMs = checkpoint.lastSuccessfulMs;
            return result;
        });
}

std::future<library::MembershipReconcileResult>
library::LibrarySync::reconcileAuthoritativeMembership(
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    const Session session = m_session;
    const auto db = m_db;
    const CatalogDbJobMetadata metadata = m_metadata;
    const auto serviceCancellation = m_cancel;
    const auto operationCancellation = cancellation;
    const std::uint64_t generation = nextGeneration();
    return std::async(std::launch::async,
        [session, db, metadata, serviceCancellation, operationCancellation,
         generation] {
            MembershipReconcileResult result;
            const auto effectiveCancellation = operationCancellation
                ? operationCancellation : serviceCancellation;
            const auto cancelled = [&] {
                return effectiveCancellation && effectiveCancellation->load();
            };
            if (!db) {
                result.error = CatalogDbErrorCategory::ScopeNotReady;
                result.message = "CatalogDb service is unavailable";
                return result;
            }
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "membership reconciliation cancelled";
                return result;
            }

            std::vector<LibraryView> views;
            std::string error;
            const bool viewsOk = RouteRequest(session).run(
                [&](const std::string &base) {
                    return JellyfinApi::getViews(
                        base, session.accessToken, session.userId,
                        session.deviceId, views, error,
                        effectiveCancellation
                            ? effectiveCancellation.get() : nullptr);
                }, error);
            if (!viewsOk) {
                result.cancelled = cancelled();
                result.error = result.cancelled
                    ? CatalogDbErrorCategory::Superseded
                    : CatalogDbErrorCategory::None;
                result.message = error;
                return result;
            }
            views.erase(std::remove_if(views.begin(), views.end(),
                                       [](const LibraryView &view) {
                                           return view.collectionType != "movies"
                                               && view.collectionType != "tvshows";
                                       }), views.end());
            if (views.empty()) {
                result.error = CatalogDbErrorCategory::ConfigurationFailed;
                result.message = "No supported library views returned";
                return result;
            }

            const auto begin = db->beginTopLevelSync(generation, metadata).get();
            if (!begin.success) {
                result.error = begin.error;
                result.message = begin.message;
                result.superseded = begin.error == CatalogDbErrorCategory::Superseded;
                return result;
            }
            auto abort = [&] {
                const auto ignored = db->abortTopLevelSync(generation, metadata).get();
                (void)ignored;
            };
            const auto fail = [&](bool wasCancelled, const std::string &message,
                                 CatalogDbErrorCategory category) {
                abort();
                result.cancelled = wasCancelled;
                result.error = category;
                result.message = message;
                return result;
            };

            for (std::size_t viewOrdinal = 0; viewOrdinal < views.size();
                 ++viewOrdinal) {
                const auto &view = views[viewOrdinal];
                const std::string types = view.collectionType == "tvshows"
                    ? "Series" : "Movie";
                int start = 0;
                for (;;) {
                    if (cancelled())
                        return fail(true, "membership reconciliation cancelled",
                                    CatalogDbErrorCategory::Superseded);
                    LibraryItemsPage page;
                    error.clear();
                    const bool pageOk = RouteRequest(session).run(
                        [&](const std::string &base) {
                            return JellyfinApi::getLibraryItemsPage(
                                base, session.accessToken, session.userId,
                                session.deviceId, view.id, types, start, 100,
                                page, error,
                                effectiveCancellation
                                    ? effectiveCancellation.get() : nullptr);
                        }, error);
                    if (!pageOk)
                        return fail(cancelled(), error,
                                    cancelled()
                                        ? CatalogDbErrorCategory::Superseded
                                        : CatalogDbErrorCategory::None);
                    if (page.hasMore && page.items.empty())
                        return fail(false, "authoritative membership page made no progress",
                                    CatalogDbErrorCategory::ConfigurationFailed);

                    CatalogDbMediaPageWrite write;
                    write.items = std::move(page.items);
                    write.viewId = view.id;
                    write.viewName = view.name;
                    write.collectionType = view.collectionType;
                    write.ordinalStart = static_cast<std::size_t>(start);
                    write.viewOrdinal = static_cast<int>(viewOrdinal);
                    write.syncGeneration = generation;
                    write.finalPage = !page.hasMore;
                    CatalogDbJobMetadata writeMetadata = metadata;
                    writeMetadata.cancellation = effectiveCancellation;
                    const auto staged = db->upsertMediaPage(write, writeMetadata).get();
                    if (!staged.success)
                        return fail(staged.cancelled || cancelled(), staged.message,
                                    staged.error);
                    ++result.pagesRead;
                    result.itemsStaged += staged.rowsWritten;
                    if (!page.hasMore)
                        break;
                    const int next = page.startIndex
                        + static_cast<int>(staged.rowsWritten);
                    if (next <= start)
                        return fail(false, "authoritative membership page made no progress",
                                    CatalogDbErrorCategory::ConfigurationFailed);
                    start = next;
                }
            }

            if (cancelled())
                return fail(true, "membership reconciliation cancelled",
                            CatalogDbErrorCategory::Superseded);
            const auto finalized = db->finalizeTopLevelSync(
                generation, metadata).get();
            if (!finalized.success) {
                result.error = finalized.error;
                result.message = finalized.message;
                result.superseded = finalized.superseded;
                return result;
            }
            result.success = true;
            result.generation = generation;
            return result;
        });
}

std::future<library::LiveLibraryChangeResult>
library::LibrarySync::applyLibraryChanges(
    const JellyfinLibraryChangeBatch &batch,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    const Session session = m_session;
    const auto db = m_db;
    const CatalogDbJobMetadata metadata = m_metadata;
    const auto serviceCancellation = m_cancel;
    const auto operationCancellation = cancellation;
    return std::async(std::launch::async,
        [session, db, metadata, serviceCancellation, operationCancellation,
         batch] {
            LiveLibraryChangeResult result;
            result.catchUpRequired = batch.catchUpRequired;
            const auto effectiveCancellation = operationCancellation
                ? operationCancellation : serviceCancellation;
            const auto cancelled = [&] {
                return effectiveCancellation && effectiveCancellation->load();
            };
            if (!db) {
                result.error = CatalogDbErrorCategory::ScopeNotReady;
                result.message = "CatalogDb service is unavailable";
                return result;
            }
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "live library change application cancelled";
                return result;
            }

            std::vector<std::string> itemIds;
            std::set<std::string> seenIds;
            for (const auto *ids : {&batch.itemsAdded, &batch.itemsUpdated,
                                    &batch.itemsRemoved}) {
                for (const auto &id : *ids) {
                    if (id.empty()) continue;
                    if (seenIds.insert(id).second) itemIds.push_back(id);
                }
            }
            constexpr std::size_t maxItemIds = 64;
            if (itemIds.size() > maxItemIds) {
                result.error = CatalogDbErrorCategory::ConfigurationFailed;
                result.message = "live library change batch exceeds bounded limit";
                result.catchUpRequired = true;
                return result;
            }
            if (itemIds.empty()) {
                result.success = true;
                return result;
            }

            std::vector<MediaItem> items;
            std::string error;
            const bool networkOk = RouteRequest(session).run(
                [&](const std::string &base) {
                    return JellyfinApi::getItemsByIds(
                        base, session.accessToken, session.userId,
                        session.deviceId, itemIds, items, error,
                        effectiveCancellation
                            ? effectiveCancellation.get() : nullptr);
                }, error);
            if (!networkOk) {
                result.cancelled = cancelled();
                result.error = result.cancelled
                    ? CatalogDbErrorCategory::Superseded
                    : CatalogDbErrorCategory::None;
                result.message = error;
                if (!result.cancelled) result.catchUpRequired = true;
                return result;
            }
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "live library change application cancelled";
                return result;
            }

            result.itemsFetched = items.size();
            std::set<std::string> returnedIds;
            for (const auto &item : items) returnedIds.insert(item.id);
            CatalogDbJobMetadata writeMetadata = metadata;
            writeMetadata.cancellation = effectiveCancellation;
            if (!items.empty()) {
                CatalogDbMediaPageWrite page;
                page.items = std::move(items);
                const auto written = db->upsertMediaPage(page, writeMetadata).get();
                if (!written.success) {
                    result.cancelled = written.cancelled;
                    result.superseded = written.superseded;
                    result.error = written.error;
                    result.message = written.message;
                    if (!result.cancelled && !result.superseded)
                        result.catchUpRequired = true;
                    return result;
                }
                result.itemsUpserted = written.rowsWritten;
            }

            std::vector<std::string> removals;
            for (const auto &id : batch.itemsRemoved) {
                if (seenIds.find(id) == seenIds.end()) continue;
                if (returnedIds.find(id) == returnedIds.end()
                    && std::find(removals.begin(), removals.end(), id)
                    == removals.end()) {
                    removals.push_back(id);
                }
            }
            if (!removals.empty()) {
                const auto deleted = db->deleteMediaItemsByIds(
                    removals, writeMetadata).get();
                if (!deleted.success) {
                    result.cancelled = deleted.cancelled;
                    result.superseded = deleted.superseded;
                    result.error = deleted.error;
                    result.message = deleted.message;
                    if (!result.cancelled && !result.superseded)
                        result.catchUpRequired = true;
                    return result;
                }
                result.itemsRemoved = removals.size();
            }
            result.success = true;
            return result;
        });
}

std::future<CatalogDbReconcileResult>
library::LibrarySync::reconcileSeries(
    const std::vector<MediaItem> &series, bool authoritative,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    CatalogDbJobMetadata metadata = m_metadata;
    metadata.cancellation = cancellation ? cancellation : m_cancel;
    return m_db->reconcileSeries(series, authoritative, metadata);
}

std::future<CatalogDbHierarchyWriteResult>
library::LibrarySync::stageSeriesHierarchy(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::uint64_t generation, bool complete,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    if (!complete)
        return rejectedHierarchyWrite(
            "incomplete hierarchy is not eligible for CatalogDb commit");
    if (!m_db)
        return rejectedHierarchyWrite("CatalogDb service is unavailable");
    CatalogDbJobMetadata metadata = m_metadata;
    metadata.cancellation = cancellation ? cancellation : m_cancel;
    return m_db->stageSeriesHierarchy(series, seasons, episodesBySeason,
                                       generation,
                                       static_cast<std::int64_t>(std::time(nullptr)) * 1000,
                                       true, metadata);
}

std::future<CatalogDbSyncState> library::LibrarySync::writeSyncState(
    std::int64_t lastSuccessfulMs, std::int64_t lastReconcileMs,
    std::uint64_t committedGeneration,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    CatalogDbJobMetadata metadata = m_metadata;
    metadata.cancellation = cancellation ? cancellation : m_cancel;
    return m_db->writeSyncState(lastSuccessfulMs, lastReconcileMs,
                                committedGeneration, metadata);
}

std::future<library::OfflineRebuildResult>
library::LibrarySync::reconstructOfflineDownloads(
    const std::string &downloadRoot)
{
    const auto db = m_db;
    const Session session = m_session;
    const CatalogDbJobMetadata metadata = m_metadata;
    const auto cancellation = m_cancel;
    const auto reconstructionGeneration = m_offlineGeneration;
    const auto operationCancellation =
        std::make_shared<std::atomic_bool>(false);
    {
        std::lock_guard<std::mutex> lock(m_offlineMutex);
        if (m_offlineCancellation)
            m_offlineCancellation->store(true);
        m_offlineCancellation = operationCancellation;
    }
    const std::uint64_t generation = ++(*reconstructionGeneration);
    return std::async(std::launch::async,
        [db, session, metadata, cancellation, reconstructionGeneration,
         operationCancellation, generation, downloadRoot] {
            OfflineRebuildResult result;
            const auto current = [&] {
                if (cancellation && cancellation->load()) {
                    result.cancelled = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message = "offline catalog reconstruction cancelled";
                    return false;
                }
                if (operationCancellation->load()) {
                    result.superseded = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message =
                        "offline catalog reconstruction superseded";
                    return false;
                }
                if (reconstructionGeneration->load() != generation) {
                    result.superseded = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message = "offline catalog reconstruction superseded";
                    return false;
                }
                return true;
            };
            if (!current()) return result;

            DownloadStore store(downloadRoot);
            const std::string scope =
                DownloadStore::scopeKey(session.serverUrl, session.userId);
            std::vector<DownloadItem> downloads;
            std::string error;
            if (!store.loadCompleteMetadata(scope, downloads, &error)) {
                const std::string indexPath = store.scopePath(scope) + "/index.v1";
                const bool indexMissing = ::access(indexPath.c_str(), F_OK) != 0;
                const int accessError = errno;
                if (indexMissing && accessError == ENOENT) {
                    result.success = true;
                    result.skipped = true;
                    result.message = "no durable download metadata";
                    return result;
                }
                result.error = CatalogDbErrorCategory::CorruptOrIo;
                result.message = error.empty()
                    ? "download metadata could not be read" : error;
                return result;
            }

            const OfflineLibraryQuery::Hierarchy hierarchy =
                OfflineLibraryQuery::hierarchy(downloads);
            CatalogDbJobMetadata writeMetadata = metadata;
            writeMetadata.cancellation = operationCancellation;
            const auto write = [&](const MediaItem &root,
                                   const std::vector<MediaItem> &seasons,
                                   const std::map<std::string,
                                                  std::vector<MediaItem>> &episodes) {
                if (!current()) return false;
                const auto written = db->stageSeriesHierarchy(
                    root, seasons, episodes, 0, 0, false, writeMetadata).get();
                result.itemsUpserted += written.rowsWritten;
                if (written.cancelled) {
                    result.cancelled = true;
                    result.error = written.error;
                    result.message = written.message;
                    return false;
                }
                if (written.superseded) {
                    result.superseded = true;
                    result.error = written.error;
                    result.message = written.message;
                    return false;
                }
                if (!written.success) {
                    result.error = written.error;
                    result.message = written.message;
                    return false;
                }
                return true;
            };

            for (const auto &movie : hierarchy.movies) {
                if (!write(movie, {}, {})) return result;
            }
            result.containersSynthesized =
                hierarchy.series.size() + hierarchy.seasons.size();
            for (const auto &seriesEntry : hierarchy.series) {
                std::vector<MediaItem> seasons;
                std::map<std::string, std::vector<MediaItem>> episodes;
                for (const auto &seasonEntry : hierarchy.seasons) {
                    if (seasonEntry.second.seriesId != seriesEntry.first)
                        continue;
                    seasons.push_back(seasonEntry.second);
                    const auto found = hierarchy.episodesBySeason.find(
                        seasonEntry.first);
                    episodes.emplace(seasonEntry.first,
                                     found == hierarchy.episodesBySeason.end()
                                         ? std::vector<MediaItem>{}
                                         : found->second);
                }
                if (!write(seriesEntry.second, seasons, episodes))
                    return result;
            }
            if (!current()) return result;
            result.success = true;
            return result;
        });
}
library::LibrarySync::Status library::LibrarySync::status() const {
    return {m_inFlight.load(), m_generation.load(), m_success.load()};
}
void library::LibrarySync::cancel() noexcept
{
    if (m_cancel)
        m_cancel->store(true);
    std::lock_guard<std::mutex> lock(m_offlineMutex);
    if (m_offlineCancellation)
        m_offlineCancellation->store(true);
}
}
