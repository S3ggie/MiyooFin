#include "LibrarySync.hpp"
#include "../net/JellyfinLibraryEvents.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <future>
#include <set>
#include <unistd.h>

namespace miyoofin {
namespace library {

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
    // Coalesce: at most one authoritative reconcile may be in flight.
    // Concurrent callers receive a superseded result immediately.
    bool expected = false;
    if (!m_authoritativeSyncInFlight.compare_exchange_strong(expected, true)) {
        std::promise<MembershipReconcileResult> p;
        MembershipReconcileResult result;
        result.superseded = true;
        result.error = CatalogDbErrorCategory::Superseded;
        result.message = "authoritative membership reconcile already in flight";
        p.set_value(std::move(result));
        return p.get_future();
    }
    const Session session = m_session;
    const auto db = m_db;
    const CatalogDbJobMetadata metadata = m_metadata;
    const auto serviceCancellation = m_cancel;
    const auto operationCancellation = cancellation;
    const std::uint64_t generation = nextGeneration();
    // The m_authoritativeSyncInFlight flag was set by compare_exchange_strong
    // above and will be cleared by FlagGuard when the async worker exits.
    // If the launch itself fails (thread/resource exhaustion), clear the flag
    // before propagating so future reconcile calls are not permanently blocked.
    try {
        return std::async(std::launch::async,
            [session, db, metadata, serviceCancellation, operationCancellation,
             generation, this] {
            // Scope guard: clear the authoritative-sync in-flight flag
            // when this async worker exits, regardless of outcome.
            struct FlagGuard {
                std::atomic<bool> &flag;
                ~FlagGuard() { flag.store(false); }
            } flagGuard{m_authoritativeSyncInFlight};
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
    } catch (...) {
        m_authoritativeSyncInFlight.store(false);
        throw;
    }
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
            result.items = items;
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
                result.removedIds = removals;
            }
            result.success = true;
            return result;
        });
}

}
} // namespace miyoofin
