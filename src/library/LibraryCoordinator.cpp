#include "LibraryCoordinator.hpp"
#include "../net/HttpClient.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>

namespace {

constexpr bool kCoordinatorStartupSyncEnabled = true;

std::int64_t coordinatorWallClockMs()
{
    return static_cast<std::int64_t>(std::time(nullptr)) * 1000;
}

bool coordinatorSupportedLibraryView(const miyoofin::LibraryView &view)
{
    return view.collectionType == "movies"
        || view.collectionType == "tvshows";
}

bool coordinatorLiveChangeIsEmpty(
    const miyoofin::JellyfinLibraryChangeBatch &batch)
{
    return !batch.catchUpRequired && !batch.userDataChanged
        && batch.itemsAdded.empty() && batch.itemsUpdated.empty()
        && batch.itemsRemoved.empty();
}

} // namespace

namespace miyoofin {
namespace library {

LibraryCoordinator::LibraryCoordinator(Session session,
                                       std::shared_ptr<CatalogDb> db,
                                       std::uint64_t scopeEpoch)
    : m_session(session)
    , m_sync(std::make_shared<LibrarySync>(std::move(session), db,
                                              scopeEpoch))
    , m_query(std::make_shared<LibraryQuery>(db, scopeEpoch))
    , m_db(std::move(db))
    , m_scopeEpoch(scopeEpoch)
{
}

LibraryCoordinator::~LibraryCoordinator()
{
    stop();
}

void LibraryCoordinator::start()
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || m_running || !m_sync)
        return;
    m_sync->startLiveEvents();
    {
        std::lock_guard<std::mutex> hierarchyLock(m_hierarchyMutex);
        m_hierarchyStop = false;
    }
    m_liveChangeStop = false;
    m_liveChangeThread = std::thread(&LibraryCoordinator::liveChangeWorker, this);
    m_hierarchyThread = std::thread(&LibraryCoordinator::hierarchyWorker, this);
    m_running = true;
}

bool LibraryCoordinator::startStartupSync(bool catalogHasRows)
{
    if (!kCoordinatorStartupSyncEnabled) {
        (void)catalogHasRows;
        return false;
    }

    // Move any completed prior thread out while holding the mutex, then join
    // it after releasing the mutex. The worker takes this same mutex to
    // publish its result, so joining while locked can deadlock at the handoff.
    std::thread priorThread;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running || !m_sync || m_startupInFlight
            || m_fullSyncInFlight || !m_fullPopulationUpdates.empty()
            || m_safetyReconcileInFlight
            || m_safetyReconcileResultReady || m_liveChangeActive)
            return false;
        if (m_startupThread.joinable())
            priorThread = std::move(m_startupThread);

        // Reserve the startup slot while the prior thread is being joined so
        // another caller cannot start a second operation in the gap.
        m_startupInFlight = true;
        m_startupCancellation = std::make_shared<std::atomic_bool>(false);
    }
    if (priorThread.joinable())
        priorThread.join();

    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running || !m_sync) {
            m_startupInFlight = false;
            return false;
        }

        const auto cancellation = m_startupCancellation;
        const auto sync = m_sync;
        const auto db = m_db;
        const auto scopeEpoch = m_scopeEpoch;
        m_startupResult = {};
        m_startupResultReady = false;
        m_startupThread = std::thread(
            [this, catalogHasRows, cancellation, sync, db, scopeEpoch] {
            StartupSyncResult result;
            const auto cancelled = [&] {
                return cancellation && cancellation->load();
            };

            if (!db || scopeEpoch == 0) {
                result.error = CatalogDbErrorCategory::ScopeNotReady;
                result.message = "CatalogDb scope is unavailable";
            } else if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "startup sync cancelled";
            } else {
                CatalogDbJobMetadata metadata;
                metadata.scopeEpoch = scopeEpoch;
                metadata.cancellation = cancellation;
                const auto state = db->readSyncState(
                    false, 0, 0, metadata).get();
                const std::int64_t nowMs = coordinatorWallClockMs();
                if (!state.success) {
                    result.mode = StartupSyncMode::FullReconcile;
                } else {
                    result.generation = state.committedGeneration;
                    result.lastSuccessfulMs = state.lastSuccessfulMs;
                    result.lastReconcileMs = state.lastReconcileMs;
                    if (result.generation > 0)
                        sync->seedGeneration(result.generation);
                    {
                        std::lock_guard<std::mutex> lock(m_startupMutex);
                        if (result.generation > m_catalogGeneration)
                            m_catalogGeneration = result.generation;
                    }

                    const bool validCheckpoint = catalogHasRows
                        && state.lastSuccessfulMs > 0
                        && result.generation > 0
                        && state.lastReconcileMs <= state.lastSuccessfulMs
                        && nowMs >= state.lastSuccessfulMs;
                    if (!validCheckpoint
                        || nowMs - state.lastReconcileMs >= 24LL * 60 * 60 * 1000) {
                        result.mode = StartupSyncMode::FullReconcile;
                    } else if (nowMs - state.lastSuccessfulMs
                               < 15LL * 60 * 1000) {
                        result.mode = StartupSyncMode::SkipFresh;
                    } else if (nowMs - state.lastSuccessfulMs
                               < 24LL * 60 * 60 * 1000) {
                        result.mode = StartupSyncMode::DeltaCatchUp;
                    } else {
                        result.mode = StartupSyncMode::FullReconcile;
                    }
                }

                if (cancelled()) {
                    result.cancelled = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message = "startup sync cancelled";
                } else if (result.mode == StartupSyncMode::DeltaCatchUp) {
                    const auto catchUp = sync->catchUpChangedCatalog(
                        state.lastSuccessfulMs, cancellation).get();
                    result.success = catchUp.success;
                    result.cancelled = catchUp.cancelled;
                    result.superseded = catchUp.superseded;
                    result.error = catchUp.error;
                    result.message = catchUp.message;
                    result.checkpointMs = catchUp.checkpointMs;
                } else {
                    // SkipFresh and FullReconcile are both successful policy
                    // decisions.  Home owns only the still-unmigrated full
                    // walk; it cannot run until this operation is complete.
                    result.success = true;
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_startupMutex);
                m_startupResult = std::move(result);
                m_startupResultReady = true;
                m_startupInFlight = false;
            }
            m_startupWake.notify_all();
            });
    }
    return true;
}

bool LibraryCoordinator::takeStartupSyncResult(StartupSyncResult &result)
{
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (!m_startupResultReady || m_startupInFlight)
            return false;
        result = std::move(m_startupResult);
        m_startupResultReady = false;
    }
    // Leave the completed thread joinable for stop() or the next startup
    // request to claim under the mutex. This keeps concurrent result-taking
    // and stopping from racing on std::thread itself.
    return true;
}

bool LibraryCoordinator::requestFullPopulation(std::uint64_t &request)
{
    std::thread priorThread;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running || !m_sync || !m_db
            || m_scopeEpoch == 0 || m_startupInFlight
            || m_startupResultReady || m_fullSyncInFlight
            || !m_fullPopulationUpdates.empty()
            || m_safetyReconcileInFlight || m_safetyReconcileResultReady
            || m_liveChangeActive)
            return false;
        if (m_fullPopulationThread.joinable())
            priorThread = std::move(m_fullPopulationThread);
        m_fullSyncInFlight = true;
        m_fullPopulationCancellation =
            std::make_shared<std::atomic_bool>(false);
        m_fullPopulationUpdates.clear();
        request = ++m_fullPopulationRequest;
    }
    if (priorThread.joinable())
        priorThread.join();

    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || !m_running || !m_sync || !m_db) {
        m_fullSyncInFlight = false;
        return false;
    }

    const auto sync = m_sync;
    const auto db = m_db;
    const Session session = m_session;
    const auto cancellation = m_fullPopulationCancellation;
    const std::uint64_t requestId = request;
    const std::uint64_t scopeEpoch = m_scopeEpoch;
    try {
        m_fullPopulationThread = std::thread(
            [this, sync, db, session, cancellation, requestId, scopeEpoch] {
            FullPopulationUpdate terminal;
            terminal.request = requestId;
            terminal.terminal = true;
            std::uint64_t generation = 0;
            bool transactionStarted = false;
            bool transactionCommitted = false;
            std::vector<LibraryView> views;
            std::vector<std::pair<std::string, std::vector<MediaItem>>>
                moviesByView;
            std::vector<std::pair<std::string, std::vector<MediaItem>>>
                showsByView;
            std::size_t metadataTotal = 0;
            std::size_t metadataCompleted = 0;
            std::size_t mediaCount = 0;
            std::size_t requestCount = 0;
            bool firstPage = true;

            const auto cancelled = [&] {
                return cancellation && cancellation->load();
            };
            const auto publish = [this, requestId](FullPopulationUpdate value) {
                std::lock_guard<std::mutex> guard(m_startupMutex);
                // A request cannot normally be superseded while this worker is
                // alive, but retain the identity check so a stale publication
                // can never be consumed by a later Home fetch.
                if (requestId != m_fullPopulationRequest)
                    return;
                value.request = requestId;
                if (value.terminal)
                    m_fullSyncInFlight = false;
                m_fullPopulationUpdates.push_back(std::move(value));
            };
            const auto publishTerminal = [&](FullPopulationUpdate value) {
                value.request = requestId;
                value.generation = generation;
                value.terminal = true;
                value.committed = transactionCommitted;
                value.views = views;
                value.moviesByView = moviesByView;
                value.showsByView = showsByView;
                value.metadataTotal = metadataTotal;
                value.metadataCompleted = metadataCompleted;
                value.mediaCount = mediaCount;
                value.requestCount = requestCount;
                publish(std::move(value));
            };
            const auto failForCancellation = [&] {
                FullPopulationUpdate value;
                value.cancelled = true;
                value.error = CatalogDbErrorCategory::Superseded;
                value.message = "full library population cancelled";
                publishTerminal(std::move(value));
            };

            try {
                if (!db || scopeEpoch == 0) {
                    terminal.error = CatalogDbErrorCategory::ScopeNotReady;
                    terminal.message = "CatalogDb scope is unavailable";
                    publishTerminal(std::move(terminal));
                    return;
                }
                if (cancelled()) {
                    failForCancellation();
                    return;
                }

                generation = sync->nextGeneration();
                const auto begin = sync->begin(generation).get();
                if (!begin.success) {
                    terminal.error = begin.error;
                    terminal.message = begin.message;
                    publishTerminal(std::move(terminal));
                    return;
                }
                transactionStarted = true;

                std::string viewsError;
                HttpClient client;
                if (!RouteRequest(session).run(
                        [&](const std::string &base) {
                            return JellyfinApi::getViews(
                                base, session.accessToken, session.userId,
                                session.deviceId, views, viewsError, client,
                                cancellation.get());
                        }, viewsError)) {
                    terminal.message = viewsError.empty()
                        ? "Failed to fetch libraries" : viewsError;
                    sync->abort(generation).get();
                    transactionStarted = false;
                    publishTerminal(std::move(terminal));
                    return;
                }
                views.erase(std::remove_if(views.begin(), views.end(),
                    [](const LibraryView &view) {
                        return !coordinatorSupportedLibraryView(view);
                    }), views.end());

                for (std::size_t viewOrdinal = 0; viewOrdinal < views.size();
                     ++viewOrdinal) {
                    const auto &view = views[viewOrdinal];
                    const std::string types = view.collectionType == "tvshows"
                        ? "Series" : "Movie";
                    int start = 0;
                    bool firstPageForView = true;
                    for (;;) {
                        if (cancelled()) {
                            sync->abort(generation).get();
                            transactionStarted = false;
                            failForCancellation();
                            return;
                        }
                        LibraryItemsPage page;
                        std::string pageError;
                        ++requestCount;
                        if (!RouteRequest(session).run(
                                [&](const std::string &base) {
                                    return JellyfinApi::getLibraryItemsPage(
                                        base, session.accessToken, session.userId,
                                        session.deviceId, view.id, types, start,
                                        48, page, pageError, client,
                                        cancellation.get());
                                }, pageError)) {
                            terminal.cancelled = cancelled();
                            terminal.superseded = !terminal.cancelled
                                && cancellation && cancellation->load();
                            terminal.error = terminal.cancelled
                                ? CatalogDbErrorCategory::Superseded
                                : CatalogDbErrorCategory::None;
                            terminal.message = pageError.empty()
                                ? "Failed to fetch library page" : pageError;
                            sync->abort(generation).get();
                            transactionStarted = false;
                            publishTerminal(std::move(terminal));
                            return;
                        }

                        if (firstPageForView) {
                            metadataTotal += page.totalRecordCount > 0
                                ? static_cast<std::size_t>(page.totalRecordCount)
                                : page.items.size();
                            firstPageForView = false;
                        }
                        metadataCompleted += page.items.size();
                        mediaCount += page.items.size();

                        CatalogDbMediaPageWrite writePage;
                        writePage.items = page.items;
                        writePage.viewId = view.id;
                        writePage.viewName = view.name;
                        writePage.collectionType = view.collectionType;
                        writePage.ordinalStart = static_cast<std::size_t>(start);
                        writePage.viewOrdinal = static_cast<int>(viewOrdinal);
                        writePage.syncGeneration = generation;
                        writePage.finalPage = !page.hasMore;
                        const auto written = sync->stage(writePage).get();
                        if (!written.success) {
                            terminal.cancelled = written.cancelled;
                            terminal.superseded = written.superseded;
                            terminal.error = written.error;
                            terminal.message = written.message.empty()
                                ? "Failed to persist library page"
                                : written.message;
                            sync->abort(generation).get();
                            transactionStarted = false;
                            publishTerminal(std::move(terminal));
                            return;
                        }

                        auto &target = view.collectionType == "tvshows"
                            ? showsByView : moviesByView;
                        if (target.empty() || target.back().first != view.name)
                            target.emplace_back(view.name,
                                                std::vector<MediaItem>{});
                        auto &bounded = target.back().second;
                        if (bounded.size() < 24) {
                            const std::size_t count = std::min<std::size_t>(
                                24 - bounded.size(), page.items.size());
                            bounded.insert(bounded.end(), page.items.begin(),
                                           page.items.begin()
                                               + static_cast<std::ptrdiff_t>(count));
                        }

                        FullPopulationUpdate update;
                        update.generation = generation;
                        update.pageValid = true;
                        update.firstPage = firstPage;
                        update.view = view;
                        update.page = page;
                        update.views = firstPage ? views
                                                : std::vector<LibraryView>{};
                        update.metadataTotal = metadataTotal;
                        update.metadataCompleted = metadataCompleted;
                        update.mediaCount = mediaCount;
                        update.requestCount = requestCount;
                        publish(std::move(update));
                        firstPage = false;

                        if (!page.hasMore || page.items.empty())
                            break;
                        start = page.startIndex
                            + static_cast<int>(page.items.size());
                    }
                }

                if (cancelled()) {
                    sync->abort(generation).get();
                    transactionStarted = false;
                    failForCancellation();
                    return;
                }
                const auto finalized = sync->finalize(generation).get();
                transactionStarted = false;
                if (!finalized.success) {
                    terminal.error = finalized.error;
                    terminal.message = finalized.message;
                    publishTerminal(std::move(terminal));
                    return;
                }
                transactionCommitted = true;
                {
                    std::lock_guard<std::mutex> guard(m_startupMutex);
                    if (generation > m_catalogGeneration)
                        m_catalogGeneration = generation;
                }
                terminal.committed = true;
                const std::int64_t nowMs = coordinatorWallClockMs();
                // Once finalize has committed the authoritative membership,
                // the checkpoint is not cancellable: restart must not regress
                // to the previous committed boundary.
                const auto checkpoint = sync->writeSyncState(
                    nowMs, nowMs, generation).get();
                terminal.checkpointCommitted = checkpoint.success;
                terminal.checkpointMs = checkpoint.lastSuccessfulMs;
                terminal.lastSuccessfulMs = checkpoint.lastSuccessfulMs;
                terminal.lastReconcileMs = checkpoint.lastReconcileMs;
                if (!checkpoint.success) {
                    terminal.error = checkpoint.error;
                    terminal.message = checkpoint.message.empty()
                        ? "Failed to write library sync checkpoint"
                        : checkpoint.message;
                }
                terminal.success = checkpoint.success;
                publishTerminal(std::move(terminal));
            } catch (const std::exception &error) {
                if (transactionStarted) {
                    try { sync->abort(generation).get(); } catch (...) { }
                }
                FullPopulationUpdate failure;
                failure.cancelled = cancelled();
                failure.superseded = !failure.cancelled && generation != 0;
                failure.error = failure.cancelled
                    ? CatalogDbErrorCategory::Superseded
                    : CatalogDbErrorCategory::SqliteError;
                failure.message = error.what();
                publishTerminal(std::move(failure));
            } catch (...) {
                if (transactionStarted) {
                    try { sync->abort(generation).get(); } catch (...) { }
                }
                FullPopulationUpdate failure;
                failure.cancelled = cancelled();
                failure.superseded = !failure.cancelled && generation != 0;
                failure.error = failure.cancelled
                    ? CatalogDbErrorCategory::Superseded
                    : CatalogDbErrorCategory::SqliteError;
                failure.message = "full library population failed unexpectedly";
                publishTerminal(std::move(failure));
            }
            });
    } catch (...) {
        m_fullSyncInFlight = false;
        throw;
    }
    return true;
}

bool LibraryCoordinator::takeFullPopulationUpdate(
    std::uint64_t request, FullPopulationUpdate &update)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (request == 0 || request != m_fullPopulationRequest
        || m_fullPopulationUpdates.empty())
        return false;
    update = std::move(m_fullPopulationUpdates.front());
    m_fullPopulationUpdates.pop_front();
    return true;
}

void LibraryCoordinator::cancelFullPopulation() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_fullPopulationCancellation)
        m_fullPopulationCancellation->store(true);
}

std::future<CatalogDbSyncState> LibraryCoordinator::checkpointLiveCatalog(
    std::int64_t lastSuccessfulMs, std::int64_t lastReconcileMs,
    std::uint64_t committedGeneration)
{
    return m_sync->writeSyncState(lastSuccessfulMs, lastReconcileMs,
                                  committedGeneration);
}

bool LibraryCoordinator::beginFullSync()
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || !m_running || m_startupInFlight || m_startupResultReady
        || m_fullSyncInFlight || !m_fullPopulationUpdates.empty()
        || m_safetyReconcileInFlight
        || m_safetyReconcileResultReady || m_liveChangeActive)
        return false;
    m_fullSyncInFlight = true;
    return true;
}

void LibraryCoordinator::finishFullSync() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    m_fullSyncInFlight = false;
}

bool LibraryCoordinator::requestSafetyReconcile()
{
    std::thread priorThread;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running || !m_sync || m_session.manualOfflineMode
            || m_startupInFlight || m_startupResultReady
            || m_fullSyncInFlight || !m_fullPopulationUpdates.empty()
            || m_safetyReconcileInFlight
            || m_safetyReconcileResultReady || m_liveChangeActive)
            return false;
        if (m_safetyReconcileThread.joinable())
            priorThread = std::move(m_safetyReconcileThread);
        m_safetyReconcileInFlight = true;
        m_safetyReconcileCancellation =
            std::make_shared<std::atomic_bool>(false);
        m_safetyReconcileResult = {};
        m_safetyReconcileResultReady = false;
    }
    if (priorThread.joinable())
        priorThread.join();

    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || !m_running || !m_sync) {
        m_safetyReconcileInFlight = false;
        return false;
    }

    const auto cancellation = m_safetyReconcileCancellation;
    const auto sync = m_sync;
    const auto db = m_db;
    const auto scopeEpoch = m_scopeEpoch;
    try {
        m_safetyReconcileThread = std::thread(
            [this, cancellation, sync, db, scopeEpoch] {
            SafetyReconcileResult result;
            std::uint64_t committedGeneration = 0;
            const auto cancelled = [&] {
                return cancellation && cancellation->load();
            };
            const auto advanceCommittedGeneration = [&] {
                std::lock_guard<std::mutex> guard(m_startupMutex);
                committedGeneration = ++m_catalogGeneration;
                return committedGeneration;
            };
            const auto publish = [this](SafetyReconcileResult value) {
                std::lock_guard<std::mutex> guard(m_startupMutex);
                m_safetyReconcileResult = std::move(value);
                m_safetyReconcileResultReady = true;
                m_safetyReconcileInFlight = false;
            };
            try {
                if (!db || scopeEpoch == 0) {
                    result.error = CatalogDbErrorCategory::ScopeNotReady;
                    result.message = "CatalogDb scope is unavailable";
                } else if (cancelled()) {
                    result.cancelled = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message = "safety reconcile cancelled";
                } else {
                    CatalogDbJobMetadata metadata;
                    metadata.scopeEpoch = scopeEpoch;
                    metadata.cancellation = cancellation;
                    const auto state = db->readSyncState(
                        false, 0, 0, metadata).get();
                    if (!state.success) {
                        result.error = state.error;
                        result.message = state.message;
                    } else {
                        sync->seedGeneration(state.committedGeneration);
                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            if (state.committedGeneration > m_catalogGeneration)
                                m_catalogGeneration = state.committedGeneration;
                            result.lastSuccessfulMs = state.lastSuccessfulMs;
                            result.lastReconcileMs = state.lastReconcileMs;
                        }
                        if (cancelled()) {
                            result.cancelled = true;
                            result.error = CatalogDbErrorCategory::Superseded;
                            result.message = "safety reconcile cancelled";
                        } else {
                            if (state.lastSuccessfulMs > 0) {
                                const auto catchUp = sync->catchUpChangedCatalog(
                                    state.lastSuccessfulMs, cancellation).get();
                                if (!catchUp.success) {
                                    result.cancelled = catchUp.cancelled;
                                    result.superseded = catchUp.superseded;
                                    result.error = catchUp.error;
                                    result.message = catchUp.message;
                                    publish(std::move(result));
                                    return;
                                }
                                result.checkpointMs = catchUp.checkpointMs;
                                result.lastSuccessfulMs = catchUp.checkpointMs;
                                committedGeneration =
                                    advanceCommittedGeneration();
                                result.generation = committedGeneration;
                            }

                            if (cancelled()) {
                                result.cancelled = true;
                                result.error = CatalogDbErrorCategory::Superseded;
                                result.message = "safety reconcile cancelled";
                            } else {
                                const auto reconciled =
                                    sync->reconcileAuthoritativeMembership(
                                        cancellation).get();
                                if (!reconciled.success) {
                                    result.cancelled = reconciled.cancelled;
                                    result.superseded = reconciled.superseded;
                                    result.error = reconciled.error;
                                    result.message = reconciled.message;
                                } else {
                                    committedGeneration =
                                        advanceCommittedGeneration();
                                    result.generation = committedGeneration;
                                    const auto nowMs = coordinatorWallClockMs();
                                    // The authoritative commit is already
                                    // durable.  Finish its checkpoint without
                                    // cancellation so restart cannot regress
                                    // to the prior committed boundary.
                                    auto checkpoint = sync->writeSyncState(
                                        nowMs, nowMs, reconciled.generation)
                                        .get();
                                    result.success = true;
                                    result.checkpointMs = checkpoint.success
                                        ? checkpoint.lastSuccessfulMs
                                        : result.checkpointMs;
                                    result.lastSuccessfulMs =
                                        checkpoint.success
                                            ? checkpoint.lastSuccessfulMs
                                            : result.lastSuccessfulMs;
                                    result.lastReconcileMs =
                                        checkpoint.success
                                            ? checkpoint.lastReconcileMs
                                            : result.lastReconcileMs;
                                    if (!checkpoint.success
                                        && result.message.empty()) {
                                        result.error = checkpoint.error;
                                        result.message = checkpoint.message;
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception &error) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = error.what();
            } catch (...) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = "safety reconcile failed unexpectedly";
            }
            result.generation = std::max(result.generation,
                                         committedGeneration);
            publish(std::move(result));
            });
    } catch (...) {
        m_safetyReconcileInFlight = false;
        throw;
    }
    return true;
}

bool LibraryCoordinator::takeSafetyReconcileResult(
    SafetyReconcileResult &result)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (!m_safetyReconcileResultReady || m_safetyReconcileInFlight)
        return false;
    result = std::move(m_safetyReconcileResult);
    m_safetyReconcileResultReady = false;
    return true;
}

bool LibraryCoordinator::requestHomeRailRefresh(std::uint64_t &request)
{
    std::thread priorThread;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running || m_session.manualOfflineMode
            || m_homeRailInFlight || m_homeRailResultReady)
            return false;
        if (m_homeRailThread.joinable())
            priorThread = std::move(m_homeRailThread);
        m_homeRailInFlight = true;
        m_homeRailCancellation = std::make_shared<std::atomic_bool>(false);
        request = ++m_homeRailRequest;
    }
    if (priorThread.joinable())
        priorThread.join();

    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running) {
            m_homeRailInFlight = false;
            return false;
        }
        const auto cancellation = m_homeRailCancellation;
        const Session session = m_session;
        const std::uint64_t requestId = request;
        m_homeRailResult = {};
        m_homeRailResultReady = false;
        m_homeRailThread = std::thread(
            [this, session, cancellation, requestId] {
            HomeRailResult result;
            result.request = requestId;
            HttpClient railClient;
            std::string continueError;
            std::string recentError;
            const bool continueOk = RouteRequest(session).run(
                [&](const std::string &base) {
                    return JellyfinApi::getResumeItems(
                        base, session.accessToken, session.userId,
                        session.deviceId, 12, result.continueWatching,
                        continueError, railClient, cancellation.get());
                }, continueError);
            const bool recentOk = RouteRequest(session).run(
                [&](const std::string &base) {
                    return JellyfinApi::getLatestItems(
                        base, session.accessToken, session.userId,
                        session.deviceId, 16, result.recentlyAdded,
                        recentError, railClient, cancellation.get());
                }, recentError);
            result.cancelled = cancellation && cancellation->load();
            result.continueValid = continueOk && !result.cancelled;
            result.recentlyAddedValid = recentOk && !result.cancelled;
            result.success = result.continueValid || result.recentlyAddedValid;
            if (!continueOk)
                result.error = continueError;
            else if (!recentOk)
                result.error = recentError;

            {
                std::lock_guard<std::mutex> lock(m_startupMutex);
                // A Home startup worker may be waiting on this publication
                // while teardown stops the coordinator.  Publish the
                // cancelled result even after stop; otherwise that worker's
                // wait loop has no completion signal and teardown deadlocks.
                if (requestId == m_homeRailRequest) {
                    m_homeRailResult = std::move(result);
                    m_homeRailResultReady = true;
                }
                m_homeRailInFlight = false;
            }
        });
    }
    return true;
}

bool LibraryCoordinator::takeHomeRailResult(
    std::uint64_t request, HomeRailResult &result)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (!m_homeRailResultReady || m_homeRailInFlight
        || request == 0 || m_homeRailResult.request != request)
        return false;
    result = std::move(m_homeRailResult);
    m_homeRailResultReady = false;
    return true;
}

void LibraryCoordinator::cancelHomeRailRefresh() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_homeRailCancellation)
        m_homeRailCancellation->store(true);
}

bool LibraryCoordinator::requestHierarchy(
    const std::vector<MediaItem> &shows, std::uint64_t generation,
    bool forceReconcile, std::uint64_t &request)
{
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running)
            return false;
    }
    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    if (m_hierarchyStop || !m_sync || !m_query
        || m_hierarchyInFlight || !m_hierarchyRequests.empty()
        || !m_hierarchyResults.empty() || generation == 0
        || generation < m_hierarchyGeneration)
        return false;

    request = ++m_hierarchyRequest;
    m_hierarchyGeneration = generation;
    m_hierarchyCompleted = 0;
    m_hierarchyTotal = shows.size();
    m_hierarchyOffline = false;
    m_hierarchyForceReconcile = forceReconcile;
    m_hierarchyLastSuccessfulMs = 0;
    m_hierarchyLastReconcileMs = 0;
    m_hierarchyCancellation = std::make_shared<std::atomic_bool>(false);
    m_hierarchyRequests.push_back(HierarchyRequest{
        request, generation, forceReconcile, shows});
    m_hierarchyInFlight = true;
    m_hierarchyWake.notify_one();
    return true;
}

bool LibraryCoordinator::takeHierarchyResult(
    std::uint64_t request, HierarchyResult &result)
{
    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    if (request == 0 || request != m_hierarchyRequest
        || m_hierarchyResults.empty())
        return false;
    result = std::move(m_hierarchyResults.front());
    m_hierarchyResults.pop_front();
    return true;
}

void LibraryCoordinator::cancelHierarchy() noexcept
{
    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    if (m_hierarchyCancellation)
        m_hierarchyCancellation->store(true);
    // Cancellation belongs to the Home lifetime, not to the next Home
    // request.  Invalidate every publication from this lifetime and discard
    // anything Home did not consume before teardown.  A cancelled worker may
    // still be unwinding a network future; allowing the next request into the
    // queue keeps that worker serialized without letting its results reserve
    // the request slot forever.
    ++m_hierarchyRequest;
    m_hierarchyRequests.clear();
    m_hierarchyResults.clear();
    m_hierarchyInFlight = false;
}

void LibraryCoordinator::hierarchyWorker()
{
    for (;;) {
        HierarchyRequest request;
        std::shared_ptr<std::atomic_bool> cancellation;
        {
            std::unique_lock<std::mutex> lock(m_hierarchyMutex);
            m_hierarchyWake.wait(lock, [&] {
                return m_hierarchyStop || !m_hierarchyRequests.empty();
            });
            if (m_hierarchyStop)
                return;
            request = std::move(m_hierarchyRequests.front());
            m_hierarchyRequests.pop_front();
            cancellation = m_hierarchyCancellation;
        }

        const auto cancelled = [&] {
            return cancellation && cancellation->load();
        };
        std::size_t completed = 0;
        bool failed = false;
        HierarchyResult terminal;
        terminal.request = request.request;
        terminal.generation = request.generation;
        terminal.terminal = true;

        const auto publish = [&](HierarchyResult result) {
            std::lock_guard<std::mutex> lock(m_hierarchyMutex);
            if (request.request != m_hierarchyRequest)
                return;
            result.request = request.request;
            result.generation = request.generation;
            m_hierarchyResults.push_back(std::move(result));
        };

        try {
            for (const auto &series : request.shows) {
                if (cancelled()) {
                    terminal.cancelled = true;
                    terminal.error = CatalogDbErrorCategory::Superseded;
                    terminal.message = "hierarchy refresh cancelled";
                    break;
                }

                HierarchyResult result;
                result.request = request.request;
                result.generation = request.generation;
                result.seriesId = series.id;
                if (m_query) {
                    const auto cached = m_query->seasons(
                        series.id, cancellation).get();
                    if (cached.success) {
                        result.cachedSeasons = std::move(cached.items);
                        if (!result.cachedSeasons.empty()) {
                            HierarchyResult cachedResult;
                            cachedResult.request = request.request;
                            cachedResult.generation = request.generation;
                            cachedResult.seriesId = series.id;
                            cachedResult.cachedSeasons = result.cachedSeasons;
                            cachedResult.cacheOnly = true;
                            publish(std::move(cachedResult));
                            result.cachedSeasons.clear();
                        }
                    }
                }

                const auto seasonRefresh = m_sync->refreshSeasons(
                    series, cancellation).get();
                if (!seasonRefresh.success) {
                    result.cancelled = seasonRefresh.cancelled;
                    result.superseded = seasonRefresh.superseded;
                    result.error = seasonRefresh.error;
                    result.message = seasonRefresh.message;
                    failed = true;
                    publish(std::move(result));
                    if (cancelled()) {
                        terminal.cancelled = true;
                        terminal.error = CatalogDbErrorCategory::Superseded;
                        terminal.message = "hierarchy refresh cancelled";
                        break;
                    }
                    continue;
                }
                result.seasons = seasonRefresh.items;

                bool complete = true;
                for (const auto &season : result.seasons) {
                    if (cancelled()) {
                        complete = false;
                        terminal.cancelled = true;
                        terminal.error = CatalogDbErrorCategory::Superseded;
                        terminal.message = "hierarchy refresh cancelled";
                        break;
                    }
                    if (season.id.empty()) {
                        complete = false;
                        break;
                    }
                    const auto episodeRefresh = m_sync->refreshEpisodes(
                        series, season, cancellation).get();
                    if (!episodeRefresh.success) {
                        complete = false;
                        result.cancelled = episodeRefresh.cancelled;
                        result.superseded = episodeRefresh.superseded;
                        result.error = episodeRefresh.error;
                        result.message = episodeRefresh.message;
                        break;
                    }
                }

                result.success = complete;
                if (result.success) {
                    ++completed;
                    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
                    if (request.request == m_hierarchyRequest)
                        ++m_hierarchyCompleted;
                } else {
                    failed = true;
                }
                publish(std::move(result));
                if (terminal.cancelled)
                    break;
            }
            if (cancelled()) {
                terminal.cancelled = true;
                terminal.error = CatalogDbErrorCategory::Superseded;
                if (terminal.message.empty())
                    terminal.message = "hierarchy refresh cancelled";
            }
            if (!terminal.cancelled && !failed
                && completed == request.shows.size()) {
                const std::int64_t nowMs = coordinatorWallClockMs();
                const auto checkpoint = m_sync->writeSyncState(
                    nowMs, request.forceReconcile ? nowMs : 0,
                    request.generation, cancellation).get();
                terminal.checkpointCommitted = checkpoint.success;
                terminal.checkpointMs = checkpoint.lastSuccessfulMs;
                terminal.lastSuccessfulMs = checkpoint.lastSuccessfulMs;
                terminal.lastReconcileMs = checkpoint.lastReconcileMs;
                terminal.success = checkpoint.success;
                terminal.error = checkpoint.error;
                terminal.message = checkpoint.message;
                if (checkpoint.success) {
                    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
                    if (request.request == m_hierarchyRequest) {
                        m_hierarchyLastSuccessfulMs =
                            checkpoint.lastSuccessfulMs;
                        m_hierarchyLastReconcileMs =
                            checkpoint.lastReconcileMs;
                    }
                }
            } else if (!terminal.cancelled && failed) {
                terminal.message = "hierarchy refresh failed";
            }
        } catch (const std::exception &error) {
            terminal.error = CatalogDbErrorCategory::SqliteError;
            terminal.message = error.what();
        } catch (...) {
            terminal.error = CatalogDbErrorCategory::SqliteError;
            terminal.message = "hierarchy refresh failed unexpectedly";
        }

        publish(std::move(terminal));
        {
            std::lock_guard<std::mutex> lock(m_hierarchyMutex);
            if (request.request == m_hierarchyRequest)
                m_hierarchyInFlight = false;
        }
    }
}

bool LibraryCoordinator::publishHomeState(HomeState state)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || state.scopeEpoch != m_scopeEpoch)
        return false;
    if (state.catalogGeneration < m_homeStateCatalogGeneration)
        return false;
    if (state.revision != 0 && state.revision <= m_homeStateRevision)
        return false;

    // A status/error-only publication must not erase useful cached content.
    // Each validity bit is independent so one failed optional rail does not
    // discard the other rail or the library projection.
    const auto prior = m_homeStateRetained;
    if (prior) {
        if (!state.contentValid) {
            state.contentValid = prior->contentValid;
            state.tabs = prior->tabs;
        }
        if (!state.cachedSnapshotValid) {
            state.cachedSnapshotValid = prior->cachedSnapshotValid;
            state.cachedSnapshot = prior->cachedSnapshot;
        }
        if (!state.continueValid) {
            state.continueValid = prior->continueValid;
            state.continueWatching = prior->continueWatching;
        }
        if (!state.recentlyAddedValid) {
            state.recentlyAddedValid = prior->recentlyAddedValid;
            state.recentlyAdded = prior->recentlyAdded;
        }
    }

    state.revision = state.revision == 0
        ? m_homeStateRevision + 1 : state.revision;
    m_homeStateRevision = state.revision;
    m_homeStateCatalogGeneration = state.catalogGeneration;
    m_homeState = std::make_shared<const HomeState>(std::move(state));
    m_homeStateRetained = m_homeState;
    return true;
}

bool LibraryCoordinator::takeHomeState(std::shared_ptr<const HomeState> &state)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (!m_homeState)
        return false;
    state = std::move(m_homeState);
    return true;
}

bool LibraryCoordinator::requestLiveChange(
    const JellyfinLibraryChangeBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || !m_running || !m_sync)
        return false;
    const bool accepted = m_liveChangeRequests.push(batch);
    m_liveChangeWake.notify_one();
    return accepted;
}

bool LibraryCoordinator::takeLiveChangeResult(LiveLibraryChangeResult &result)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (!m_liveChangeResult)
        return false;
    result = std::move(*m_liveChangeResult);
    m_liveChangeResult.reset();
    m_liveChangeActive.reset();
    m_liveChangeWake.notify_one();
    return true;
}

void LibraryCoordinator::cancelLiveChange() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_liveChangeCancellation)
        m_liveChangeCancellation->store(true);
}

void LibraryCoordinator::discardLiveChangeResults() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    m_liveChangeResult.reset();
    m_liveChangeActive.reset();
    m_liveChangeWake.notify_one();
}

void LibraryCoordinator::liveChangeWorker()
{
    for (;;) {
        JellyfinLibraryChangeBatch batch;
        LiveChangeIdentity identity;
        std::shared_ptr<std::atomic_bool> cancellation;
        {
            std::unique_lock<std::mutex> lock(m_startupMutex);
            // The event receiver writes directly to LibrarySync's bounded
            // queue. Drain it here, never on Home's SDL thread, before
            // checking the serialized top-level gates.  If the coordinator
            // queue is full, push() may have coalesced only part of the
            // batch.  Preserve that loss as an explicit catch-up barrier and
            // stop popping the source until the barrier is consumed.
            if (!m_liveChangeDrainPendingCatchUp && m_sync) {
                JellyfinLibraryChangeBatch incoming;
                while (m_sync->takeLiveChange(incoming)) {
                    if (m_liveChangeRequests.push(incoming))
                        continue;
                    m_liveChangeRequests.markCatchUpRequired();
                    m_liveChangeDrainPendingCatchUp = true;
                    break;
                }
            }
            if (m_liveChangeStop)
                return;
            const bool serializedSlotOpen = !m_startupInFlight
                && !m_startupResultReady && !m_fullSyncInFlight
                && m_fullPopulationUpdates.empty()
                && !m_safetyReconcileInFlight
                && !m_safetyReconcileResultReady
                && !m_liveChangeActive && !m_liveChangeResult;
            if (!serializedSlotOpen
                || !m_liveChangeRequests.pop(batch)) {
                m_liveChangeWake.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }
            if (batch.catchUpRequired)
                m_liveChangeDrainPendingCatchUp = true;
            if (coordinatorLiveChangeIsEmpty(batch))
                continue;

            identity.worker = ++m_liveChangeWorker;
            identity.generation = m_catalogGeneration;
            identity.request = ++m_liveChangeRequest;
            m_liveChangeActive = identity;
            m_liveChangeCancellation =
                std::make_shared<std::atomic_bool>(false);
            cancellation = m_liveChangeCancellation;
        }

        LiveLibraryChangeResult result;
        result.catchUpRequired = batch.catchUpRequired;
        result.userDataChanged = batch.userDataChanged;
        std::uint64_t committedGeneration = identity.generation;
        bool catchUpSucceeded = !batch.catchUpRequired;
        const auto cancelled = [&] {
            return cancellation && cancellation->load();
        };
        const auto advanceCommittedGeneration = [&] {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            committedGeneration = ++m_catalogGeneration;
            return committedGeneration;
        };
        const auto publish = [&](LiveLibraryChangeResult value) {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            // Discarding a result or accepting a newer worker identity makes
            // this publication stale. It must never be consumed by the next
            // live batch.
            if (!m_liveChangeActive
                || !(*m_liveChangeActive == identity)
                || m_liveChangeResult)
                return;
            value.generation = std::max(value.generation,
                                        committedGeneration);
            m_liveChangeResult = std::move(value);
            m_liveChangeWake.notify_one();
        };

        try {
            if (!m_db || m_scopeEpoch == 0) {
                result.error = CatalogDbErrorCategory::ScopeNotReady;
                result.message = "CatalogDb scope is unavailable";
            } else if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "live library change cancelled";
            } else if (batch.catchUpRequired) {
                CatalogDbJobMetadata metadata;
                metadata.scopeEpoch = m_scopeEpoch;
                metadata.cancellation = cancellation;
                const auto state = m_db->readSyncState(
                    false, 0, 0, metadata).get();
                if (!state.success) {
                    result.error = state.error;
                    result.message = state.message;
                    result.lastSuccessfulMs = state.lastSuccessfulMs;
                    result.lastReconcileMs = state.lastReconcileMs;
                } else {
                    result.lastSuccessfulMs = state.lastSuccessfulMs;
                    result.lastReconcileMs = state.lastReconcileMs;
                    if (state.committedGeneration > committedGeneration) {
                        std::lock_guard<std::mutex> lock(m_startupMutex);
                        if (state.committedGeneration > m_catalogGeneration)
                            m_catalogGeneration = state.committedGeneration;
                        committedGeneration = m_catalogGeneration;
                    }
                    if (cancelled()) {
                        result.cancelled = true;
                        result.error = CatalogDbErrorCategory::Superseded;
                        result.message = "live library change cancelled";
                    } else if (state.lastSuccessfulMs > 0) {
                        const auto catchUp = m_sync->catchUpChangedCatalog(
                            state.lastSuccessfulMs, cancellation).get();
                        if (!catchUp.success) {
                            result.cancelled = catchUp.cancelled;
                            result.superseded = catchUp.superseded;
                            result.error = catchUp.error;
                            result.message = catchUp.message;
                        } else {
                            catchUpSucceeded = true;
                            result.checkpointMs = catchUp.checkpointMs;
                            result.lastSuccessfulMs = catchUp.checkpointMs;
                            result.generation = advanceCommittedGeneration();
                        }
                    } else {
                        const auto reconciled =
                            m_sync->reconcileAuthoritativeMembership(
                                cancellation).get();
                        if (!reconciled.success) {
                            result.cancelled = reconciled.cancelled;
                            result.superseded = reconciled.superseded;
                            result.error = reconciled.error;
                            result.message = reconciled.message;
                        } else {
                            result.generation = advanceCommittedGeneration();
                            const std::int64_t nowMs =
                                coordinatorWallClockMs();
                            // The authoritative membership commit is already
                            // durable. Finish this checkpoint even if the
                            // caller requested cancellation meanwhile.
                            const auto checkpoint = m_sync->writeSyncState(
                                nowMs, nowMs, reconciled.generation).get();
                            if (checkpoint.success) {
                                catchUpSucceeded = true;
                                result.checkpointMs = checkpoint.lastSuccessfulMs;
                                result.lastSuccessfulMs =
                                    checkpoint.lastSuccessfulMs;
                                result.lastReconcileMs =
                                    checkpoint.lastReconcileMs;
                            } else if (result.message.empty()) {
                                result.error = checkpoint.error;
                                result.message = checkpoint.message;
                            }
                        }
                    }
                }
            }

            const bool catchUpFailed = batch.catchUpRequired
                && !catchUpSucceeded;
            if (!catchUpFailed && result.error == CatalogDbErrorCategory::None
                && !result.cancelled && !result.superseded) {
                if (batch.itemsAdded.empty() && batch.itemsRemoved.empty()
                    && batch.itemsUpdated.empty()
                    && !batch.catchUpRequired) {
                    result.success = true;
                } else {
                    auto applied = m_sync->applyLibraryChanges(
                        batch, cancellation).get();
                    const bool userDataChanged = result.userDataChanged;
                    const auto priorGeneration = result.generation;
                    result = std::move(applied);
                    result.userDataChanged = userDataChanged;
                    result.generation = std::max(result.generation,
                                                 priorGeneration);
                    result.catchUpRequired = batch.catchUpRequired
                        || result.catchUpRequired;
                    if (result.success)
                        result.generation = advanceCommittedGeneration();
                }
            }
        } catch (const std::exception &error) {
            result.cancelled = cancelled();
            result.superseded = !result.cancelled;
            result.error = result.cancelled
                ? CatalogDbErrorCategory::Superseded
                : CatalogDbErrorCategory::SqliteError;
            result.message = error.what();
        } catch (...) {
            result.cancelled = cancelled();
            result.superseded = !result.cancelled;
            result.error = result.cancelled
                ? CatalogDbErrorCategory::Superseded
                : CatalogDbErrorCategory::SqliteError;
            result.message = "live library change failed unexpectedly";
        }
        if (batch.catchUpRequired) {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            const bool transferredBarrierSucceeded = catchUpSucceeded
                && result.success;
            if (transferredBarrierSucceeded) {
                m_liveChangeDrainPendingCatchUp = false;
            } else if (!m_liveChangeStop) {
                // The source event batch has already been transferred and its
                // overflow bit was consumed.  Keep the catch-up barrier
                // queued until the whole transferred batch completes
                // successfully, including any item application after the
                // checkpoint advances.  Home only consumes the failure
                // publication and never owns retry policy.
                (void)m_liveChangeRequests.push(batch);
                m_liveChangeRequests.markCatchUpRequired();
                m_liveChangeDrainPendingCatchUp = true;
            }
        }
        result.generation = std::max(result.generation, committedGeneration);
        publish(std::move(result));
    }
}

void LibraryCoordinator::cancelStartupSync() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_startupCancellation)
        m_startupCancellation->store(true);
}

void LibraryCoordinator::cancelSafetyReconcile() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_safetyReconcileCancellation)
        m_safetyReconcileCancellation->store(true);
}

void LibraryCoordinator::stop() noexcept
{
    std::thread startupThread;
    std::thread fullPopulationThread;
    std::thread homeRailThread;
    std::thread safetyReconcileThread;
    std::thread liveChangeThread;
    std::thread hierarchyThread;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped)
            return;
        m_running = false;
        m_stopped = true;
        if (m_startupCancellation)
            m_startupCancellation->store(true);
        if (m_fullPopulationCancellation)
            m_fullPopulationCancellation->store(true);
        if (m_homeRailCancellation)
            m_homeRailCancellation->store(true);
        if (m_safetyReconcileCancellation)
            m_safetyReconcileCancellation->store(true);
        if (m_liveChangeCancellation)
            m_liveChangeCancellation->store(true);
        m_liveChangeStop = true;
        if (m_startupThread.joinable())
            startupThread = std::move(m_startupThread);
        if (m_fullPopulationThread.joinable())
            fullPopulationThread = std::move(m_fullPopulationThread);
        if (m_homeRailThread.joinable())
            homeRailThread = std::move(m_homeRailThread);
        if (m_safetyReconcileThread.joinable())
            safetyReconcileThread = std::move(m_safetyReconcileThread);
        if (m_liveChangeThread.joinable())
            liveChangeThread = std::move(m_liveChangeThread);
    }
    m_liveChangeWake.notify_all();
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        m_hierarchyStop = true;
        if (m_hierarchyCancellation)
            m_hierarchyCancellation->store(true);
        hierarchyThread = std::move(m_hierarchyThread);
    }
    m_hierarchyWake.notify_all();
    // The startup worker publishes through m_startupMutex, so never join it
    // while holding that mutex.
    if (startupThread.joinable())
        startupThread.join();
    if (fullPopulationThread.joinable())
        fullPopulationThread.join();
    if (homeRailThread.joinable())
        homeRailThread.join();
    if (safetyReconcileThread.joinable())
        safetyReconcileThread.join();
    if (liveChangeThread.joinable())
        liveChangeThread.join();
    if (hierarchyThread.joinable())
        hierarchyThread.join();
    if (m_sync)
        m_sync->stop();
}

LibraryCoordinator::Status LibraryCoordinator::status() const
{
    Status status;
    if (m_sync) {
        const auto syncStatus = m_sync->status();
        status.inFlight = syncStatus.inFlight;
        status.success = syncStatus.success;
        status.generation = syncStatus.generation;
    }
    std::lock_guard<std::mutex> lock(m_startupMutex);
    status.startupInFlight = m_startupInFlight;
    status.fullSyncInFlight = m_fullSyncInFlight;
    status.safetyReconcileInFlight = m_safetyReconcileInFlight;
    status.cancelRequested = m_startupCancellation
        && m_startupCancellation->load();
    status.inFlight = status.inFlight || status.startupInFlight
        || status.fullSyncInFlight || status.safetyReconcileInFlight;
    return status;
}

std::future<HierarchyRefreshResult> LibraryCoordinator::refreshSeasons(
    const MediaItem &series,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return m_sync->refreshSeasons(series, cancellation);
}

std::future<HierarchyRefreshResult> LibraryCoordinator::refreshEpisodes(
    const MediaItem &series, const MediaItem &season,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    return m_sync->refreshEpisodes(series, season, cancellation);
}

} // namespace library
} // namespace miyoofin
