#include "LibraryCoordinator.hpp"
#include "LibraryCoordinatorInternal.hpp"
#include "LibrarySync.hpp"
#include "../net/HttpClient.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include "../data/CatalogPrimitives.hpp"
#include "../diagnostics/UiDiagnostics.hpp"
#include "../diagnostics/TelemetryClock.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>

namespace miyoofin {
namespace library {

using namespace coordinator_detail;

bool LibraryCoordinator::requestHierarchy(const std::vector<MediaItem>& shows,
                                          std::uint64_t generation, bool forceReconcile,
                                          std::uint64_t& request)
{
    if (shows.empty() || generation == 0)
        return false;
    std::lock_guard<std::mutex> startupLock(m_startupMutex);
    AdmissionRequest admission;
    admission.requester = OperationKind::Hierarchy;
    admission.requireQuery = true;
    if (!serializedOperationAdmittedLocked(admission, nullptr))
        return false;

    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    if (m_hierarchyRequests.size() >= 8 || generation < m_hierarchyGeneration)
        return false;
    for (const auto& entry : m_hierarchyAccepted) {
        if (entry.second.kind == HierarchyTaskKind::HomePrefetch)
            return false;
    }
    request = ++m_hierarchyRequest;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    HierarchyRequest task{request, generation, forceReconcile, HierarchyTaskKind::HomePrefetch,
                          {},      {},         shows,          cancellation};
    m_hierarchyAccepted.emplace(request, task);
    m_hierarchyGeneration = generation;
    m_hierarchyCompleted = 0;
    m_hierarchyTotal = shows.size();
    m_hierarchyOffline = false;
    m_hierarchyForceReconcile = forceReconcile;
    m_hierarchyLastSuccessfulMs = 0;
    m_hierarchyLastReconcileMs = 0;
    beginOperation(OperationKind::Hierarchy);
    m_hierarchyRequests.push_back(std::move(task));
    m_hierarchyWake.notify_one();
    return true;
}

bool LibraryCoordinator::requestSeriesSeasons(const MediaItem& series, std::uint64_t& request)
{
    if (series.id.empty())
        return false;
    std::lock_guard<std::mutex> startupLock(m_startupMutex);
    AdmissionRequest admission;
    admission.requester = OperationKind::Hierarchy;
    admission.requireQuery = true;
    if (!serializedOperationAdmittedLocked(admission, nullptr))
        return false;
    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    if (m_hierarchyRequests.size() >= 8)
        return false;
    HierarchyRequest task;
    task.request = ++m_hierarchyRequest;
    task.generation = m_catalogGeneration;
    task.kind = HierarchyTaskKind::SeriesSeasons;
    task.series = series;
    task.cancellation = std::make_shared<std::atomic_bool>(false);
    for (const auto& entry : m_hierarchyAccepted) {
        if (coordinatorHierarchyIdentityMatches(entry.second, task))
            return false;
    }
    request = task.request;
    m_hierarchyAccepted.emplace(request, task);
    beginOperation(OperationKind::Hierarchy);
    m_hierarchyRequests.push_back(std::move(task));
    m_hierarchyWake.notify_one();
    return true;
}

bool LibraryCoordinator::requestSeasonEpisodes(const MediaItem& series, const MediaItem& season,
                                               std::uint64_t& request)
{
    if (series.id.empty() || season.id.empty())
        return false;
    std::lock_guard<std::mutex> startupLock(m_startupMutex);
    AdmissionRequest admission;
    admission.requester = OperationKind::Hierarchy;
    admission.requireQuery = true;
    if (!serializedOperationAdmittedLocked(admission, nullptr))
        return false;
    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    if (m_hierarchyRequests.size() >= 8)
        return false;
    HierarchyRequest task;
    task.request = ++m_hierarchyRequest;
    task.generation = m_catalogGeneration;
    task.kind = HierarchyTaskKind::SeasonEpisodes;
    task.series = series;
    task.season = season;
    task.cancellation = std::make_shared<std::atomic_bool>(false);
    for (const auto& entry : m_hierarchyAccepted) {
        if (coordinatorHierarchyIdentityMatches(entry.second, task))
            return false;
    }
    request = task.request;
    m_hierarchyAccepted.emplace(request, task);
    beginOperation(OperationKind::Hierarchy);
    m_hierarchyRequests.push_back(std::move(task));
    m_hierarchyWake.notify_one();
    return true;
}

bool LibraryCoordinator::takeHierarchyResult(std::uint64_t request, HierarchyResult& result)
{
    bool took = false;
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        took = takeHierarchyResultLocked(request, result);
    }
    if (took)
        m_hierarchyWake.notify_all();
    return took;
}

bool LibraryCoordinator::takeHierarchyResultLocked(std::uint64_t request, HierarchyResult& result)
{
    if (request == 0 || m_hierarchyResults.empty())
        return false;
    const auto found =
        std::find_if(m_hierarchyResults.begin(), m_hierarchyResults.end(),
                     [request](const HierarchyResult& value) { return value.request == request; });
    if (found == m_hierarchyResults.end())
        return false;
    result = std::move(*found);
    m_hierarchyResults.erase(found);
    if (result.terminal) {
        m_hierarchyAccepted.erase(request);
        m_hierarchySupersededRequests.insert(request);
        if (m_hierarchyRequests.empty() && !m_hierarchyActiveRequest &&
            m_hierarchyResults.empty()) {
            releaseOperation(OperationKind::Hierarchy);
        }
    }
    return true;
}

WaitStatus LibraryCoordinator::waitHierarchyResult(std::uint64_t request, HierarchyResult& result,
                                                   const std::atomic_bool* consumerCancellation)
{
    std::unique_lock<std::mutex> lock(m_hierarchyMutex);
    for (;;) {
        if (takeHierarchyResultLocked(request, result))
            return WaitStatus::Ready;
        if (request == 0)
            return WaitStatus::InvalidRequest;
        if (m_hierarchyStop)
            return WaitStatus::Stopped;
        if (consumerCancellation && consumerCancellation->load())
            return WaitStatus::Cancelled;
        const auto accepted = m_hierarchyAccepted.find(request);
        if (accepted == m_hierarchyAccepted.end())
            return m_hierarchySupersededRequests.count(request) ? WaitStatus::Superseded
                                                                : WaitStatus::InvalidRequest;
        if (accepted->second.cancellation && accepted->second.cancellation->load())
            return WaitStatus::Cancelled;
        m_hierarchyWake.wait(lock);
    }
}

void LibraryCoordinator::cancelHierarchyRequest(std::uint64_t request) noexcept
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        const auto accepted = m_hierarchyAccepted.find(request);
        if (accepted == m_hierarchyAccepted.end())
            return;
        accepted->second.cancellation->store(true);
        m_hierarchyRequests.erase(std::remove_if(m_hierarchyRequests.begin(),
                                                 m_hierarchyRequests.end(),
                                                 [request](const HierarchyRequest& value) {
                                                     return value.request == request;
                                                 }),
                                  m_hierarchyRequests.end());
        m_hierarchyResults.erase(std::remove_if(m_hierarchyResults.begin(),
                                                m_hierarchyResults.end(),
                                                [request](const HierarchyResult& value) {
                                                    return value.request == request;
                                                }),
                                 m_hierarchyResults.end());
        // The cancellation API is fire-and-forget.  Removing an active request
        // also suppresses its terminal publication once the network future
        // unwinds, so a caller that is leaving cannot strand scheduler state.
        m_hierarchySupersededRequests.insert(request);
        m_hierarchyAccepted.erase(accepted);
        if (m_hierarchyRequests.empty() && !m_hierarchyActiveRequest && m_hierarchyResults.empty())
            releaseOperation(OperationKind::Hierarchy);
        cancelled = true;
    }
    if (cancelled)
        m_hierarchyWake.notify_all();
}

void LibraryCoordinator::cancelHierarchy() noexcept
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        std::vector<std::uint64_t> homeRequests;
        for (const auto& entry : m_hierarchyAccepted) {
            if (entry.second.kind == HierarchyTaskKind::HomePrefetch)
                homeRequests.push_back(entry.first);
        }
        for (const auto request : homeRequests) {
            const auto accepted = m_hierarchyAccepted.find(request);
            if (accepted != m_hierarchyAccepted.end()) {
                accepted->second.cancellation->store(true);
                m_hierarchySupersededRequests.insert(request);
                m_hierarchyAccepted.erase(accepted);
                cancelled = true;
            }
        }
        // Cancellation belongs to the Home lifetime, not to the next Home
        // request.  Invalidate every publication from this lifetime and discard
        // anything Home did not consume before teardown.  A cancelled worker may
        // still be unwinding a network future; allowing the next request into the
        // queue keeps that worker serialized without letting its results reserve
        // the request slot forever.
        const auto oldRequestCount = m_hierarchyRequests.size();
        const auto oldResultCount = m_hierarchyResults.size();
        m_hierarchyRequests.erase(
            std::remove_if(m_hierarchyRequests.begin(), m_hierarchyRequests.end(),
                           [](const HierarchyRequest& value) {
                               return value.kind == HierarchyTaskKind::HomePrefetch;
                           }),
            m_hierarchyRequests.end());
        m_hierarchyResults.erase(
            std::remove_if(m_hierarchyResults.begin(), m_hierarchyResults.end(),
                           [](const HierarchyResult& value) {
                               return value.kind == HierarchyTaskKind::HomePrefetch;
                           }),
            m_hierarchyResults.end());
        cancelled = cancelled || oldRequestCount != m_hierarchyRequests.size() ||
                    oldResultCount != m_hierarchyResults.size();
        if (m_hierarchyRequests.empty() && !m_hierarchyActiveRequest && m_hierarchyResults.empty())
            releaseOperation(OperationKind::Hierarchy);
    }
    if (cancelled)
        m_hierarchyWake.notify_all();
}

void LibraryCoordinator::hierarchyWorker()
{
    for (;;) {
        HierarchyRequest request;
        std::shared_ptr<std::atomic_bool> cancellation;
        {
            std::unique_lock<std::mutex> lock(m_hierarchyMutex);
            m_hierarchyWake.wait(lock,
                                 [&] { return m_hierarchyStop || !m_hierarchyRequests.empty(); });
            if (m_hierarchyStop)
                return;
            request = std::move(m_hierarchyRequests.front());
            m_hierarchyRequests.pop_front();
            cancellation = request.cancellation;
            m_hierarchyActiveRequest = request;
            m_hierarchyActiveCancellation = cancellation;
        }

        const auto cancelled = [&] { return cancellation && cancellation->load(); };
        const auto loadSeasons = [&](const MediaItem& series) {
            HierarchyResult result;
            result.kind = request.kind;
            result.seriesId = series.id;
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "season hierarchy request cancelled";
                return result;
            }

            std::string error;
            std::vector<MediaItem> seasons;
            const bool networkOk = RouteRequest(m_session).run(
                [&](const std::string& base) {
                    return JellyfinApi::getSeasons(base, m_session.accessToken, m_session.userId,
                                                   m_session.deviceId, series.id, seasons, error,
                                                   cancellation.get());
                },
                error);
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "season hierarchy request cancelled";
                return result;
            }
            if (!networkOk) {
                result.message = error;
                return result;
            }

            if (m_db) {
                CatalogDbJobMetadata metadata;
                metadata.scopeEpoch = m_scopeEpoch;
                metadata.cancellation = cancellation;
                std::map<std::string, std::vector<MediaItem>> episodesBySeason;
                for (const auto& season : seasons)
                    episodesBySeason.emplace(season.id, std::vector<MediaItem>{});
                const auto written =
                    m_db->stageSeriesHierarchy(series, seasons, episodesBySeason, 0,
                                               coordinatorWallClockMs(), false, metadata)
                        .get();
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
                result.message = "season hierarchy request cancelled";
                return result;
            }
            result.success = true;
            result.seasons = std::move(seasons);
            return result;
        };
        const auto loadEpisodes = [&](const MediaItem& series, const MediaItem& season) {
            HierarchyResult result;
            result.kind = request.kind;
            result.seriesId = series.id;
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "episode hierarchy request cancelled";
                return result;
            }

            std::string error;
            std::vector<MediaItem> episodes;
            const bool networkOk = RouteRequest(m_session).run(
                [&](const std::string& base) {
                    return JellyfinApi::getEpisodes(base, m_session.accessToken, m_session.userId,
                                                    m_session.deviceId, series.id, season.id,
                                                    episodes, error, cancellation.get());
                },
                error);
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "episode hierarchy request cancelled";
                return result;
            }
            if (!networkOk) {
                result.message = error;
                return result;
            }

            if (m_db) {
                CatalogDbJobMetadata metadata;
                metadata.scopeEpoch = m_scopeEpoch;
                metadata.cancellation = cancellation;
                const auto written =
                    m_db->reconcileSeasonHierarchy(series, season, episodes, 0,
                                                   coordinatorWallClockMs(), metadata)
                        .get();
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
                result.message = "episode hierarchy request cancelled";
                return result;
            }
            result.success = true;
            result.episodes = std::move(episodes);
            return result;
        };
        bool failed = false;
        std::size_t completed = 0;
        HierarchyResult terminal;
        terminal.request = request.request;
        terminal.generation = request.generation;
        terminal.kind = request.kind;
        terminal.terminal = true;

        const auto publish = [&](HierarchyResult result) {
            bool publicationMade = false;
            {
                std::lock_guard<std::mutex> lock(m_hierarchyMutex);
                // A Home lifetime may discard its active request while the
                // network future unwinds.  A direct caller keeps its request
                // accepted until it consumes the terminal publication.
                if (m_hierarchyAccepted.find(request.request) == m_hierarchyAccepted.end())
                    return;
                result.request = request.request;
                result.generation = request.generation;
                result.kind = request.kind;
                m_hierarchyResults.push_back(std::move(result));
                publicationMade = true;
            }
            if (publicationMade)
                m_hierarchyWake.notify_all();
        };

        try {
            if (request.kind == HierarchyTaskKind::SeriesSeasons) {
                terminal = loadSeasons(request.series);
                terminal.terminal = true;
                publish(std::move(terminal));
            } else if (request.kind == HierarchyTaskKind::SeasonEpisodes) {
                terminal = loadEpisodes(request.series, request.season);
                terminal.terminal = true;
                publish(std::move(terminal));
            } else {
                for (const auto& series : request.shows) {
                    if (cancelled()) {
                        terminal.cancelled = true;
                        terminal.error = CatalogDbErrorCategory::Superseded;
                        terminal.message = "hierarchy refresh cancelled";
                        break;
                    }

                    HierarchyResult result;
                    result.kind = request.kind;
                    result.seriesId = series.id;
                    if (m_query) {
                        const auto cached = m_query->seasons(series.id, cancellation).get();
                        if (cached.success) {
                            result.cachedSeasons = std::move(cached.items);
                            if (!result.cachedSeasons.empty()) {
                                HierarchyResult cachedResult;
                                cachedResult.kind = request.kind;
                                cachedResult.seriesId = series.id;
                                cachedResult.cachedSeasons = result.cachedSeasons;
                                cachedResult.cacheOnly = true;
                                publish(std::move(cachedResult));
                                result.cachedSeasons.clear();
                            }
                        }
                    }

                    const auto seasonRefresh = loadSeasons(series);
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
                    result.seasons = seasonRefresh.seasons;

                    bool complete = true;
                    for (const auto& season : result.seasons) {
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
                        const auto episodeRefresh = loadEpisodes(series, season);
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
                        if (m_hierarchyActiveRequest &&
                            m_hierarchyActiveRequest->request == request.request) {
                            ++m_hierarchyCompleted;
                        }
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
                if (!terminal.cancelled && !failed && completed == request.shows.size()) {
                    const std::int64_t nowMs = coordinatorWallClockMs();
                    const auto checkpoint =
                        m_sync
                            ->writeSyncState(nowMs, request.forceReconcile ? nowMs : 0,
                                             request.generation, cancellation)
                            .get();
                    terminal.checkpointCommitted = checkpoint.success;
                    terminal.checkpointMs = checkpoint.lastSuccessfulMs;
                    terminal.lastSuccessfulMs = checkpoint.lastSuccessfulMs;
                    terminal.lastReconcileMs = checkpoint.lastReconcileMs;
                    terminal.success = checkpoint.success;
                    terminal.error = checkpoint.error;
                    terminal.message = checkpoint.message;
                    if (checkpoint.success) {
                        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
                        if (m_hierarchyActiveRequest &&
                            m_hierarchyActiveRequest->request == request.request) {
                            m_hierarchyLastSuccessfulMs = checkpoint.lastSuccessfulMs;
                            m_hierarchyLastReconcileMs = checkpoint.lastReconcileMs;
                        }
                    }
                } else if (!terminal.cancelled && failed) {
                    terminal.message = "hierarchy refresh failed";
                }
                publish(std::move(terminal));
            }
        } catch (const std::exception& error) {
            terminal.request = request.request;
            terminal.generation = request.generation;
            terminal.kind = request.kind;
            terminal.terminal = true;
            terminal.cancelled = cancelled();
            terminal.error = terminal.cancelled ? CatalogDbErrorCategory::Superseded
                                                : CatalogDbErrorCategory::SqliteError;
            terminal.message = error.what();
            publish(std::move(terminal));
        } catch (...) {
            terminal.request = request.request;
            terminal.generation = request.generation;
            terminal.kind = request.kind;
            terminal.terminal = true;
            terminal.cancelled = cancelled();
            terminal.error = terminal.cancelled ? CatalogDbErrorCategory::Superseded
                                                : CatalogDbErrorCategory::SqliteError;
            terminal.message = "hierarchy refresh failed unexpectedly";
            publish(std::move(terminal));
        }

        {
            std::lock_guard<std::mutex> lock(m_hierarchyMutex);
            if (m_hierarchyActiveRequest && m_hierarchyActiveRequest->request == request.request) {
                m_hierarchyActiveRequest.reset();
                m_hierarchyActiveCancellation.reset();
            }
            if (m_hierarchyRequests.empty() && !m_hierarchyActiveRequest &&
                m_hierarchyResults.empty()) {
                releaseOperation(OperationKind::Hierarchy);
            }
        }
    }
}

} // namespace library
} // namespace miyoofin
