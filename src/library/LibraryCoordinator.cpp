#include "LibraryCoordinator.hpp"
#include "../net/HttpClient.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include <algorithm>
#include <ctime>
#include <exception>

namespace {

// HomeScreen retains the full population walk, while this coordinator owns
// the persisted-checkpoint decision and bounded top-level synchronization.
constexpr bool kCoordinatorStartupSyncEnabled = true;

std::int64_t coordinatorWallClockMs()
{
    return static_cast<std::int64_t>(std::time(nullptr)) * 1000;
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
            || m_fullSyncInFlight || m_safetyReconcileInFlight
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

bool LibraryCoordinator::beginFullSync()
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || !m_running || m_startupInFlight || m_startupResultReady
        || m_fullSyncInFlight || m_safetyReconcileInFlight
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
            || m_fullSyncInFlight || m_safetyReconcileInFlight
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
    return m_liveChangeRequests.push(batch);
}

bool LibraryCoordinator::takeLiveChangeRequest(
    JellyfinLibraryChangeBatch &batch, LiveChangeIdentity &identity)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);

    // LibraryCoordinator is the only consumer of the event queue. Drain the
    // LibrarySync queue before applying the serialized-sync gate so events
    // cannot be popped by Home and then lost when the gate is closed.
    if (m_sync) {
        JellyfinLibraryChangeBatch incoming;
        while (m_sync->takeLiveChange(incoming))
            (void)m_liveChangeRequests.push(incoming);
    }
    // Keep the queued batch intact until the serialized consumer has
    // released its previous live-change slot.  Home may still have a worker
    // thread active even though the coordinator's startup/full-sync gates are
    // open.
    if (m_startupInFlight || m_startupResultReady || m_fullSyncInFlight
        || m_safetyReconcileInFlight || m_safetyReconcileResultReady
        || m_liveChangeActive)
        return false;
    if (!m_liveChangeRequests.pop(batch))
        return false;

    const auto syncStatus = m_sync ? m_sync->status() : LibrarySync::Status{};
    identity.worker = ++m_liveChangeWorker;
    identity.generation = syncStatus.generation;
    identity.request = ++m_liveChangeRequest;
    m_liveChangeActive = identity;
    return true;
}

bool LibraryCoordinator::publishLiveChangeResult(
    const LiveChangeIdentity &identity, LiveLibraryChangeResult result)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || !m_liveChangeActive
        || !(*m_liveChangeActive == identity) || m_liveChangeResult)
        return false;
    m_liveChangeResult = std::move(result);
    return true;
}

bool LibraryCoordinator::takeLiveChangeResult(
    const LiveChangeIdentity &identity, LiveLibraryChangeResult &result)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (!m_liveChangeResult || !m_liveChangeActive
        || !(*m_liveChangeActive == identity))
        return false;
    result = std::move(*m_liveChangeResult);
    m_liveChangeResult.reset();
    m_liveChangeActive.reset();
    return true;
}

void LibraryCoordinator::discardLiveChangeResults() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    m_liveChangeResult.reset();
    m_liveChangeActive.reset();
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
    std::thread homeRailThread;
    std::thread safetyReconcileThread;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped)
            return;
        m_running = false;
        m_stopped = true;
        if (m_startupCancellation)
            m_startupCancellation->store(true);
        if (m_homeRailCancellation)
            m_homeRailCancellation->store(true);
        if (m_safetyReconcileCancellation)
            m_safetyReconcileCancellation->store(true);
        m_liveChangeResult.reset();
        m_liveChangeActive.reset();
        if (m_startupThread.joinable())
            startupThread = std::move(m_startupThread);
        if (m_homeRailThread.joinable())
            homeRailThread = std::move(m_homeRailThread);
        if (m_safetyReconcileThread.joinable())
            safetyReconcileThread = std::move(m_safetyReconcileThread);
    }
    // The startup worker publishes through m_startupMutex, so never join it
    // while holding that mutex.
    if (startupThread.joinable())
        startupThread.join();
    if (homeRailThread.joinable())
        homeRailThread.join();
    if (safetyReconcileThread.joinable())
        safetyReconcileThread.join();
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

} // namespace library
} // namespace miyoofin
