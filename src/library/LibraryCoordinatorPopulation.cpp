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

bool LibraryCoordinator::takeFullPopulationUpdateLocked(std::uint64_t request,
                                                        FullPopulationUpdate& update)
{
    if (request == 0 || request != m_fullPopulationRequest || m_fullPopulationUpdates.empty())
        return false;
    update = std::move(m_fullPopulationUpdates.front());
    m_fullPopulationUpdates.pop_front();
    // The terminal publication retains the slot until Home consumes the whole
    // queue; draining it releases the serialized operation.  An intermediate
    // page that momentarily empties the queue does not, because the worker is
    // still executing.
    if (m_fullPopulationUpdates.empty() &&
        currentOperationKind() == OperationKind::FullPopulation &&
        currentOperationPhase() == OperationPhase::PublicationPending) {
        releaseOperation(OperationKind::FullPopulation);
        // Draining the terminal publication ends the cold-start sequence; a
        // live change retained during startup/population may now be applied.
        finishStartupSequenceLocked();
    }
    return true;
}

WaitStatus
LibraryCoordinator::waitFullPopulationUpdate(std::uint64_t request, FullPopulationUpdate& update,
                                             const std::atomic_bool* consumerCancellation)
{
    std::unique_lock<std::mutex> lock(m_startupMutex);
    for (;;) {
        if (takeFullPopulationUpdateLocked(request, update))
            return WaitStatus::Ready;
        if (request == 0 || request != m_fullPopulationRequest)
            return WaitStatus::InvalidRequest;
        if (m_stopped)
            return WaitStatus::Stopped;
        const bool populationExecuting = currentOperationKind() == OperationKind::FullPopulation &&
                                         currentOperationPhase() == OperationPhase::Executing;
        const bool cancelled =
            (consumerCancellation && consumerCancellation->load()) ||
            (m_fullPopulationCancellation && m_fullPopulationCancellation->load());
        if (cancelled) {
            // Intermediate pages remain observable.  The terminal publication
            // clears the serialized gate and is consumed before cancellation
            // is reported to a caller with no remaining result.
            if (populationExecuting) {
                m_startupWake.wait(lock);
                continue;
            }
            return WaitStatus::Cancelled;
        }
        if (!populationExecuting)
            return WaitStatus::Superseded;
        m_startupWake.wait(lock);
    }
}

bool LibraryCoordinator::takeFullPopulationUpdate(std::uint64_t request,
                                                  FullPopulationUpdate& update)
{
    bool took = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        took = takeFullPopulationUpdateLocked(request, update);
    }
    if (took)
        m_startupWake.notify_all();
    return took;
}

bool LibraryCoordinator::requestFullPopulation(std::uint64_t& request)
{
    std::thread priorThread;
    std::string admissionDiagnostic;
    bool ownsStartupSequence = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        AdmissionRequest admission;
        admission.requester = OperationKind::FullPopulation;
        admission.requireDb = true;
        admission.requireScope = true;
        std::string reasons;
        if (!serializedOperationAdmittedLocked(admission, &reasons)) {
            admissionDiagnostic =
                "[LibraryCoordinator] full_population_rejected phase=admission request=" +
                std::to_string(request) +
                " current_request=" + std::to_string(m_fullPopulationRequest) +
                " generation=" + std::to_string(m_fullPopulationGeneration) +
                " scope_epoch=" + std::to_string(m_scopeEpoch) + " reasons=" + reasons;
        } else {
            if (m_fullPopulationThread.joinable())
                priorThread = std::move(m_fullPopulationThread);
            beginOperation(OperationKind::FullPopulation);
            // Only a successful admission consumes the cold-start handoff or
            // reservation.  Consuming it up front would let an unrelated
            // operation already occupying the slot as the same FullPopulation
            // kind (for example a beginFullSync reservation) make this request
            // fail admission after it had stolen Home's handoff, clearing the
            // owner token and armed demand.  Deferring keeps the handoff,
            // token, and demand intact for the intended continuation.
            ownsStartupSequence = claimStartupSequenceLocked(OperationKind::FullPopulation);
            m_fullPopulationCancellation = std::make_shared<std::atomic_bool>(false);
            m_fullPopulationUpdates.clear();
            request = ++m_fullPopulationRequest;
            m_fullPopulationGeneration = 0;
        }
    }
    if (!admissionDiagnostic.empty()) {
        // The failed admission did not consume the handoff/reservation, so the
        // owner-held sequence and its armed demand remain intact.
        uiDiagnostics().log(admissionDiagnostic);
        return false;
    }

    std::string postJoinDiagnostic;
    try {
        // The prior-thread join is part of the admitted operation's unwind
        // path: if it throws, the serialized slot and cold-start demand must
        // still be released by the catch below.
        if (priorThread.joinable())
            priorThread.join();

        {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            if (m_stopped || !m_running || !m_sync || !m_db) {
                std::string reasons;
                appendCoordinatorGateReason(reasons, "stopped", m_stopped);
                appendCoordinatorGateReason(reasons, "not_running", !m_running);
                appendCoordinatorGateReason(reasons, "missing_sync", !m_sync);
                appendCoordinatorGateReason(reasons, "missing_db", !m_db);
                postJoinDiagnostic =
                    "[LibraryCoordinator] full_population_rejected phase=post_join request=" +
                    std::to_string(request) +
                    " current_request=" + std::to_string(m_fullPopulationRequest) +
                    " generation=" + std::to_string(m_fullPopulationGeneration) +
                    " scope_epoch=" + std::to_string(m_scopeEpoch) + " reasons=" + reasons;
                releaseOperation(OperationKind::FullPopulation);
                if (ownsStartupSequence)
                    finishStartupSequenceLocked();
            } else {
                const auto sync = m_sync;
                const auto db = m_db;
                const Session session = m_session;
                const auto cancellation = m_fullPopulationCancellation;
                const std::uint64_t requestId = request;
                const std::uint64_t scopeEpoch = m_scopeEpoch;
                const std::string scopeHash =
                    (session.serverUrl.empty() || session.userId.empty())
                        ? "none"
                        : catalog::scopeKey(session.serverUrl, session.userId);
                m_fullPopulationThread = std::thread([this, sync, db, session, cancellation,
                                                      requestId, scopeEpoch, scopeHash] {
                    FullPopulationUpdate terminal;
                    terminal.request = requestId;
                    terminal.terminal = true;
                    std::uint64_t transactionGeneration = 0;
                    std::uint64_t committedGeneration = 0;
                    bool transactionStarted = false;
                    bool transactionCommitted = false;
                    std::vector<LibraryView> views;
                    std::vector<std::pair<std::string, std::vector<MediaItem>>> moviesByView;
                    std::vector<std::pair<std::string, std::vector<MediaItem>>> showsByView;
                    std::size_t metadataTotal = 0;
                    std::size_t metadataCompleted = 0;
                    std::size_t mediaCount = 0;
                    std::size_t requestCount = 0;
                    std::size_t completedPageCount = 0;
                    bool firstPage = true;

                    const auto cancelled = [&] { return cancellation && cancellation->load(); };
                    const auto publish = [this, requestId, scopeEpoch,
                                          scopeHash](FullPopulationUpdate value) {
                        std::string diagnostic;
                        bool publicationMade = false;
                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            // A request cannot normally be superseded while this worker is
                            // alive, but retain the identity check so a stale publication
                            // can never be consumed by a later Home fetch.
                            if (requestId != m_fullPopulationRequest) {
                                diagnostic =
                                    "[LibraryCoordinator] "
                                    "full_population_publish_rejected request=" +
                                    std::to_string(requestId) +
                                    " current_request=" + std::to_string(m_fullPopulationRequest) +
                                    " generation=" + std::to_string(value.generation) +
                                    " current_generation=" +
                                    std::to_string(m_fullPopulationGeneration) +
                                    " scope_epoch=" + std::to_string(scopeEpoch) +
                                    " scope_hash=" + scopeHash + " source=full_population" +
                                    " reason=request_mismatch";
                            } else {
                                value.request = requestId;
                                if (value.terminal)
                                    markOperationPublicationPending(OperationKind::FullPopulation);
                                m_fullPopulationUpdates.push_back(std::move(value));
                                publicationMade = true;
                                const auto& published = m_fullPopulationUpdates.back();
                                std::string kind =
                                    published.terminal
                                        ? (published.success
                                               ? "terminal_success"
                                               : (published.cancelled || published.superseded
                                                      ? "terminal_cancel"
                                                      : "terminal_failure"))
                                        : (published.firstPage ? "first_page" : "page");
                                diagnostic =
                                    "[LibraryCoordinator] full_population_publish request=" +
                                    std::to_string(published.request) +
                                    " generation=" + std::to_string(published.generation) +
                                    " scope_epoch=" + std::to_string(scopeEpoch) +
                                    " scope_hash=" + scopeHash + " source=full_population" +
                                    " kind=" + kind + " queue_depth=" +
                                    std::to_string(m_fullPopulationUpdates.size()) +
                                    " monotonic_us=" +
                                    std::to_string(TelemetryClock::monotonicUs());
                            }
                        }
                        if (publicationMade)
                            m_startupWake.notify_all();
                        uiDiagnostics().log(diagnostic);
                    };
                    const auto publishTerminal = [&](FullPopulationUpdate value) {
                        value.request = requestId;
                        value.generation =
                            transactionCommitted ? committedGeneration : m_catalogGeneration;
                        value.committedGeneration =
                            transactionCommitted ? committedGeneration : m_catalogGeneration;
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

                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            committedGeneration = m_catalogGeneration + 1;
                        }
                        transactionGeneration = sync->nextTransactionGeneration();
                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            m_fullPopulationGeneration = transactionGeneration;
                        }
                        const auto begin = sync->begin(transactionGeneration).get();
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
                                [&](const std::string& base) {
                                    return JellyfinApi::getViews(
                                        base, session.accessToken, session.userId, session.deviceId,
                                        views, viewsError, client, cancellation.get());
                                },
                                viewsError)) {
                            // Mirror the page-fetch failure path: a request
                            // aborted by cancellation must publish a cancelled
                            // terminal, not a generic failure, so a consumer
                            // waiting on the serialized gate can distinguish
                            // the two.
                            terminal.cancelled = cancelled();
                            terminal.superseded =
                                !terminal.cancelled && cancellation && cancellation->load();
                            terminal.error = terminal.cancelled ? CatalogDbErrorCategory::Superseded
                                                                : CatalogDbErrorCategory::None;
                            terminal.message =
                                viewsError.empty() ? "Failed to fetch libraries" : viewsError;
                            sync->abort(transactionGeneration).get();
                            transactionStarted = false;
                            publishTerminal(std::move(terminal));
                            return;
                        }
                        views.erase(std::remove_if(views.begin(), views.end(),
                                                   [](const LibraryView& view) {
                                                       return !coordinatorSupportedLibraryView(
                                                           view);
                                                   }),
                                    views.end());

                        for (std::size_t viewOrdinal = 0; viewOrdinal < views.size();
                             ++viewOrdinal) {
                            const auto& view = views[viewOrdinal];
                            const std::string types =
                                view.collectionType == "tvshows" ? "Series" : "Movie";
                            int start = 0;
                            bool firstPageForView = true;
                            for (;;) {
                                if (cancelled()) {
                                    sync->abort(transactionGeneration).get();
                                    transactionStarted = false;
                                    failForCancellation();
                                    return;
                                }
                                LibraryItemsPage page;
                                std::string pageError;
                                ++requestCount;
                                if (!RouteRequest(session).run(
                                        [&](const std::string& base) {
                                            return JellyfinApi::getLibraryItemsPage(
                                                base, session.accessToken, session.userId,
                                                session.deviceId, view.id, types, start, 48, page,
                                                pageError, client, cancellation.get());
                                        },
                                        pageError)) {
                                    terminal.cancelled = cancelled();
                                    terminal.superseded =
                                        !terminal.cancelled && cancellation && cancellation->load();
                                    terminal.error = terminal.cancelled
                                                         ? CatalogDbErrorCategory::Superseded
                                                         : CatalogDbErrorCategory::None;
                                    terminal.message = pageError.empty()
                                                           ? "Failed to fetch library page"
                                                           : pageError;
                                    sync->abort(transactionGeneration).get();
                                    transactionStarted = false;
                                    publishTerminal(std::move(terminal));
                                    return;
                                }

                                if (firstPageForView) {
                                    metadataTotal +=
                                        page.totalRecordCount > 0
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
                                writePage.syncGeneration = transactionGeneration;
                                writePage.request = requestId;
                                writePage.diagnosticSource = "full_population";
                                writePage.finalPage = !page.hasMore;
                                CatalogDbMediaPageUpsertResult written;
                                try {
                                    written = sync->stage(writePage).get();
                                    if (written.success)
                                        ++completedPageCount;
                                    uiDiagnostics().log(
                                        "[LibraryCoordinator] full_population_stage_complete"
                                        " request=" +
                                        std::to_string(requestId) +
                                        " generation=" + std::to_string(transactionGeneration) +
                                        " scope_epoch=" + std::to_string(scopeEpoch) +
                                        " scope_hash=" + scopeHash +
                                        " source=full_population page_kind=" +
                                        (writePage.collectionType == "movies"    ? "movies"
                                         : writePage.collectionType == "tvshows" ? "tvshows"
                                                                                 : "unknown") +
                                        " page_index=" + std::to_string(writePage.ordinalStart) +
                                        " status=" +
                                        (written.success ? "success"
                                                         : (written.cancelled || written.superseded
                                                                ? "cancelled"
                                                                : "failure")) +
                                        " error=" +
                                        std::to_string(static_cast<unsigned>(written.error)) +
                                        " completed_pages=" + std::to_string(completedPageCount) +
                                        " first_page=" + std::to_string(firstPage ? 1 : 0) +
                                        " monotonic_us=" +
                                        std::to_string(TelemetryClock::monotonicUs()));
                                } catch (const std::exception&) {
                                    uiDiagnostics().log(
                                        "[LibraryCoordinator] full_population_stage_exception"
                                        " request=" +
                                        std::to_string(requestId) +
                                        " generation=" + std::to_string(transactionGeneration) +
                                        " exception_class=std_exception" +
                                        " completed_pages=" + std::to_string(completedPageCount) +
                                        " first_page=" + std::to_string(firstPage ? 1 : 0) +
                                        " monotonic_us=" +
                                        std::to_string(TelemetryClock::monotonicUs()));
                                    throw;
                                } catch (...) {
                                    uiDiagnostics().log(
                                        "[LibraryCoordinator] full_population_stage_exception"
                                        " request=" +
                                        std::to_string(requestId) +
                                        " generation=" + std::to_string(transactionGeneration) +
                                        " exception_class=unknown" +
                                        " completed_pages=" + std::to_string(completedPageCount) +
                                        " first_page=" + std::to_string(firstPage ? 1 : 0) +
                                        " monotonic_us=" +
                                        std::to_string(TelemetryClock::monotonicUs()));
                                    throw;
                                }
                                if (!written.success) {
                                    terminal.cancelled = written.cancelled;
                                    terminal.superseded = written.superseded;
                                    terminal.error = written.error;
                                    terminal.message = written.message.empty()
                                                           ? "Failed to persist library page"
                                                           : written.message;
                                    sync->abort(transactionGeneration).get();
                                    transactionStarted = false;
                                    publishTerminal(std::move(terminal));
                                    return;
                                }

                                auto& target =
                                    view.collectionType == "tvshows" ? showsByView : moviesByView;
                                if (target.empty() || target.back().first != view.name)
                                    target.emplace_back(view.name, std::vector<MediaItem>{});
                                auto& bounded = target.back().second;
                                if (bounded.size() < 24) {
                                    const std::size_t count = std::min<std::size_t>(
                                        24 - bounded.size(), page.items.size());
                                    bounded.insert(bounded.end(), page.items.begin(),
                                                   page.items.begin() +
                                                       static_cast<std::ptrdiff_t>(count));
                                }

                                FullPopulationUpdate update;
                                update.generation = committedGeneration;
                                update.pageValid = true;
                                update.firstPage = firstPage;
                                update.view = view;
                                update.page = page;
                                update.views = firstPage ? views : std::vector<LibraryView>{};
                                update.metadataTotal = metadataTotal;
                                update.metadataCompleted = metadataCompleted;
                                update.mediaCount = mediaCount;
                                update.requestCount = requestCount;
                                publish(std::move(update));
                                firstPage = false;

                                if (!page.hasMore || page.items.empty())
                                    break;
                                start = page.startIndex + static_cast<int>(page.items.size());
                            }
                        }

                        if (cancelled()) {
                            sync->abort(transactionGeneration).get();
                            transactionStarted = false;
                            failForCancellation();
                            return;
                        }
                        const auto finalized = sync->finalize(transactionGeneration).get();
                        transactionStarted = false;
                        if (!finalized.success) {
                            terminal.error = finalized.error;
                            terminal.message = finalized.message;
                            publishTerminal(std::move(terminal));
                            return;
                        }
                        transactionCommitted = true;
                        const std::int64_t nowMs = coordinatorWallClockMs();
                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            if (committedGeneration > m_catalogGeneration)
                                m_catalogGeneration = committedGeneration;
                        }
                        terminal.committed = true;
                        // Once finalize has committed the authoritative membership,
                        // the checkpoint is not cancellable: restart must not regress
                        // to the previous committed boundary.
                        const auto checkpoint =
                            sync->writeSyncState(nowMs, nowMs, committedGeneration).get();
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
                        if (checkpoint.success) {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            m_lastSuccessfulMs = checkpoint.lastSuccessfulMs;
                            m_lastReconcileMs = checkpoint.lastReconcileMs;
                        }
                        publishTerminal(std::move(terminal));
                    } catch (const std::exception& error) {
                        if (transactionStarted) {
                            try {
                                sync->abort(transactionGeneration).get();
                            } catch (...) {
                            }
                        }
                        FullPopulationUpdate failure;
                        failure.cancelled = cancelled();
                        failure.superseded = !failure.cancelled && transactionGeneration != 0;
                        failure.error = failure.cancelled ? CatalogDbErrorCategory::Superseded
                                                          : CatalogDbErrorCategory::SqliteError;
                        failure.message = error.what();
                        publishTerminal(std::move(failure));
                    } catch (...) {
                        if (transactionStarted) {
                            try {
                                sync->abort(transactionGeneration).get();
                            } catch (...) {
                            }
                        }
                        FullPopulationUpdate failure;
                        failure.cancelled = cancelled();
                        failure.superseded = !failure.cancelled && transactionGeneration != 0;
                        failure.error = failure.cancelled ? CatalogDbErrorCategory::Superseded
                                                          : CatalogDbErrorCategory::SqliteError;
                        failure.message = "full library population failed unexpectedly";
                        publishTerminal(std::move(failure));
                    }
                });
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        releaseOperation(OperationKind::FullPopulation);
        // Worker-thread construction or any other post-join failure must not
        // leave the cold-start demand armed, or retained live changes would be
        // deferred forever.  Only an admitted owner can reach this unwind path,
        // but gate on ownership so a non-owner can never clear the active
        // sequence's demand.
        if (ownsStartupSequence)
            finishStartupSequenceLocked();
        throw;
    }
    if (!postJoinDiagnostic.empty()) {
        uiDiagnostics().log(postJoinDiagnostic);
        return false;
    }
    return true;
}

void LibraryCoordinator::cancelFullPopulation() noexcept
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_fullPopulationCancellation) {
            cancelled = true;
            m_fullPopulationCancellation->store(true);
        }
    }
    if (cancelled)
        m_startupWake.notify_all();
}

std::future<CatalogDbSyncState> LibraryCoordinator::checkpointLiveCatalog(
    std::int64_t lastSuccessfulMs, std::int64_t lastReconcileMs, std::uint64_t committedGeneration)
{
    return m_sync->writeSyncState(lastSuccessfulMs, lastReconcileMs, committedGeneration);
}

bool LibraryCoordinator::beginFullSync()
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    AdmissionRequest request;
    request.requester = OperationKind::FullPopulation;
    if (!serializedOperationAdmittedLocked(request, nullptr))
        return false;
    // Reserve the slot without a worker: callers use finishFullSync() to
    // release it.  No terminal publication is produced.
    beginOperation(OperationKind::FullPopulation);
    return true;
}

void LibraryCoordinator::finishFullSync() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    releaseOperation(OperationKind::FullPopulation);
}

} // namespace library
} // namespace miyoofin
