#include "test_support.hpp"
#include "cases/test_library_coordinator_support.hpp"

namespace {
void testFullPopulationSuccessCommitsCheckpoint()
{
    std::printf("[test] LibraryCoordinator full population success\n");
    const auto scope = coordinatorTestScope("success");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(10)));

    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    const std::string serverUrl = coordinatorTestUrl(listener);
    const std::string viewsBody =
        R"({"Items":[{"Id":"movies","Name":"Movies","CollectionType":"movies"}]})";
    const std::string pageBody =
        R"({"StartIndex":0,"TotalRecordCount":1,"Items":[{"Id":"coordinator-movie","Type":"Movie","Name":"Coordinator Movie"}]})";
    std::thread server([&] {
        const std::string bodies[] = {viewsBody, pageBody};
        for (const auto& body : bodies) {
            const int client = coordinatorAccept(listener);
            if (client < 0)
                return;
            coordinatorReadRequest(client);
            coordinatorSendJson(client, body);
            ::close(client);
        }
    });

    Session session;
    session.serverUrl = serverUrl;
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    server.join();

    CHECK(sawPage);
    CHECK(terminal.success && terminal.committed && terminal.checkpointCommitted);
    CHECK(!terminal.cancelled && !terminal.superseded);
    CHECK(terminal.generation > 0 && terminal.mediaCount == 1);
    CHECK(terminal.metadataTotal == 1 && terminal.metadataCompleted == 1);
    const auto state = db->readSyncState(false, 0, 0, {0, epoch, {}}).get();
    CHECK(state.success && state.committedGeneration == terminal.generation);
    CHECK(state.lastSuccessfulMs == terminal.checkpointMs && state.lastSuccessfulMs > 0);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator full population success OK\n");
}

void testFullPopulationAfterLiveChangeAdvancesGeneration()
{
    std::printf("[test] LibraryCoordinator full-after-live generation\n");
    const auto scope = coordinatorTestScope("full-after-live-generation");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(10)));

    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    const std::string viewsBody =
        R"({"Items":[{"Id":"movies","Name":"Movies","CollectionType":"movies"}]})";
    const std::string pageBody =
        R"({"StartIndex":0,"TotalRecordCount":1,"Items":[{"Id":"full-after-live","Type":"Movie","Name":"Full After Live"}]})";
    std::thread server([&] {
        const std::string bodies[] = {
            R"({"Items":[{"Id":"live-before-full","Type":"movie","Name":"Live Before Full"}]})",
            viewsBody,
            pageBody,
        };
        for (const auto& body : bodies) {
            const int client = coordinatorAccept(listener);
            if (client < 0)
                return;
            coordinatorReadRequest(client);
            coordinatorSendJson(client, body);
            ::close(client);
        }
    });

    Session session;
    session.serverUrl = coordinatorTestUrl(listener);
    session.accessToken = "test-token";
    session.userId = scope.user;
    session.manualOfflineMode = true;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();

    JellyfinLibraryChangeBatch liveBatch;
    liveBatch.itemsUpdated.push_back("live-before-full");
    CHECK(coordinator->requestLiveChange(liveBatch));
    library::LiveLibraryChangeResult liveResult;
    bool tookLiveResult = false;
    for (int i = 0; i < 1000 && !tookLiveResult; ++i) {
        tookLiveResult = coordinator->takeLiveChangeResult(liveResult);
        if (!tookLiveResult)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(tookLiveResult);
    CHECK(liveResult.success && liveResult.generation > 0);

    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    server.join();

    CHECK(sawPage && terminal.success && terminal.committed);
    CHECK(terminal.generation == liveResult.generation + 1);
    CHECK(terminal.committedGeneration == terminal.generation);
    const auto status = coordinator->status();
    CHECK(status.committedGeneration == terminal.generation);
    const auto state = db->readSyncState(false, 0, 0, {0, epoch, {}}).get();
    CHECK(state.success && state.committedGeneration == terminal.generation);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator full-after-live generation OK\n");
}

void testFullPopulationCancellationAbortsStagedGeneration()
{
    std::printf("[test] LibraryCoordinator full population cancellation\n");
    const auto scope = coordinatorTestScope("cancel");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(10)));
    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    const std::string serverUrl = coordinatorTestUrl(listener);
    const std::string viewsBody =
        R"({"Items":[{"Id":"movies","Name":"Movies","CollectionType":"movies"}]})";
    std::atomic_bool pageConnected{false};
    std::atomic_bool releasePage{false};
    std::thread server([&] {
        const int viewsClient = coordinatorAccept(listener);
        if (viewsClient < 0)
            return;
        coordinatorReadRequest(viewsClient);
        coordinatorSendJson(viewsClient, viewsBody);
        ::close(viewsClient);
        const int pageClient = coordinatorAccept(listener);
        if (pageClient < 0)
            return;
        pageConnected.store(true, std::memory_order_release);
        while (!releasePage.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ::close(pageClient);
    });

    Session session;
    session.serverUrl = serverUrl;
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    for (int i = 0; i < 500 && !pageConnected.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(pageConnected.load());
    coordinator->cancelFullPopulation();
    releasePage.store(true, std::memory_order_release);
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    server.join();

    CHECK(terminal.terminal && terminal.cancelled);
    CHECK(terminal.error == CatalogDbErrorCategory::Superseded && !terminal.success &&
          !terminal.committed);
    CHECK(!terminal.checkpointCommitted);

    // Consuming the cancellation terminal must release the serialized slot;
    // the next request can be admitted even though the first worker was
    // cancelled while its HTTP page was blocked.
    //
    // Hold the retry worker inside its views request before cancelling so the
    // cancellation is deterministically observed by the views-fetch failure
    // path (not just the pre-flight cancelled() check).  That path must still
    // publish a cancelled terminal so a consumer waiting on the gate can
    // distinguish cancellation from a generic fetch failure.
    std::atomic_bool retryViewsConnected{false};
    std::atomic_bool releaseRetryViews{false};
    std::thread retryServer([&] {
        const int viewsClient = coordinatorAccept(listener);
        if (viewsClient < 0)
            return;
        retryViewsConnected.store(true, std::memory_order_release);
        while (!releaseRetryViews.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ::close(viewsClient);
    });
    std::uint64_t retryRequest = 0;
    CHECK(coordinator->requestFullPopulation(retryRequest));
    for (int i = 0; i < 500 && !retryViewsConnected.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(retryViewsConnected.load());
    coordinator->cancelFullPopulation();
    releaseRetryViews.store(true, std::memory_order_release);
    library::FullPopulationUpdate retryTerminal;
    bool retrySawPage = false;
    CHECK(takeFullUpdate(*coordinator, retryRequest, retryTerminal, retrySawPage));
    CHECK(retryTerminal.terminal && retryTerminal.cancelled);
    retryServer.join();
    coordinator->stop();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator full population cancellation OK\n");
}

void testFullPopulationFailureAbortsWithoutCheckpoint()
{
    std::printf("[test] LibraryCoordinator full population failure\n");
    const auto scope = coordinatorTestScope("failure");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(10)));
    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    std::thread server([&] {
        const int client = coordinatorAccept(listener);
        if (client < 0)
            return;
        coordinatorReadRequest(client);
        coordinatorSendJson(client, R"({"Error":"failed"})", 500);
        ::close(client);
    });
    Session session;
    session.serverUrl = coordinatorTestUrl(listener);
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    server.join();

    CHECK(terminal.terminal && !terminal.success && !terminal.cancelled);
    CHECK(!terminal.committed && !terminal.checkpointCommitted);
    CHECK(!terminal.message.empty());
    const auto state = db->readSyncState(false, 0, 0, {0, epoch, {}}).get();
    CHECK(state.success && state.committedGeneration == 0);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator full population failure OK\n");
}

void testFullPopulationRejectsStaleRequest()
{
    std::printf("[test] LibraryCoordinator stale request rejection\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    // A stale request identity must never consume the current publication.
    library::FullPopulationUpdate update;
    CHECK(!coordinator.takeFullPopulationUpdate(1, update));

    coordinator.stop();
    std::printf("[test] LibraryCoordinator stale request rejection OK\n");
}

void testCoordinatorSerializesStartupFullSafetyAndLive()
{
    std::printf("[test] LibraryCoordinator serialization gates\n");
    const auto scope = coordinatorTestScope("serialization");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(10)));

    Session session;
    session.serverUrl = "http://127.0.0.1:1";
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();

    // Hold the startup DB read so all other top-level work is observed while
    // startup owns the serialized slot.
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->startStartupSync(false));
    std::uint64_t request = 0;
    CHECK(!coordinator->requestFullPopulation(request));
    CHECK(!coordinator->requestSafetyReconcileForTest());
    JellyfinLibraryChangeBatch batch;
    batch.itemsUpdated.push_back("startup-queued-live");
    CHECK(coordinator->requestLiveChange(batch));
    library::LiveLibraryChangeResult liveResult;
    CHECK(!coordinator->takeLiveChangeResult(liveResult));
    coordinator->cancelStartupSync();
    db->setWorkerPausedForTest(false);
    library::StartupSyncResult startupResult;
    for (int i = 0; i < 500 && !coordinator->takeStartupSyncResult(startupResult); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(startupResult.cancelled || startupResult.error == CatalogDbErrorCategory::ScopeNotReady ||
          startupResult.success);

    // The live change queued behind startup runs next and holds the
    // serialized slot until its result is published. Wait for it so the
    // safety admission below does not race that worker under CPU load.
    bool tookQueuedLive = false;
    for (int i = 0; i < 500 && !(tookQueuedLive = coordinator->takeLiveChangeResult(liveResult));
         ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(tookQueuedLive);

    // A paused safety worker similarly blocks startup, population, and live
    // consumption while retaining the queued event.
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->requestSafetyReconcileForTest());
    CHECK(!coordinator->startStartupSync(false));
    CHECK(!coordinator->requestFullPopulation(request));
    JellyfinLibraryChangeBatch safetyBatch;
    safetyBatch.itemsUpdated.push_back("safety-queued-live");
    CHECK(coordinator->requestLiveChange(safetyBatch));
    CHECK(!coordinator->takeLiveChangeResult(liveResult));
    coordinator->cancelSafetyReconcile();
    db->setWorkerPausedForTest(false);
    // The event retained behind safety is consumed once safety releases.
    bool tookRetainedLive = false;
    for (int i = 0; i < 500 && !(tookRetainedLive = coordinator->takeLiveChangeResult(liveResult));
         ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(tookRetainedLive);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator serialization gates OK\n");
}
} // namespace

int main()
{
    const std::string diagnosticsPath = "/tmp/miyoofin-library-coordinator-diagnostics-" +
                                        std::to_string(static_cast<long long>(::getpid())) + ".log";
    std::remove(diagnosticsPath.c_str());
    uiDiagnostics().start(diagnosticsPath);

    testFullPopulationSuccessCommitsCheckpoint();
    testFullPopulationAfterLiveChangeAdvancesGeneration();
    testFullPopulationCancellationAbortsStagedGeneration();
    testFullPopulationFailureAbortsWithoutCheckpoint();
    testFullPopulationRejectsStaleRequest();
    testCoordinatorSerializesStartupFullSafetyAndLive();
    // All producers above have joined.  stop() is the existing logger
    // drain/join barrier, not a timing delay.
    uiDiagnostics().stop();
    const auto diagnostics = miyoofin_test::readTestBytes(diagnosticsPath);

    CHECK(diagnostics.find("[LibraryCoordinator] full_population_rejected phase=admission") !=
          std::string::npos);
    CHECK(diagnostics.find("reasons=startup_in_flight") != std::string::npos);
    CHECK(diagnostics.find("reasons=safety_reconcile_in_flight") != std::string::npos);
    const auto scopeReady =
        diagnosticLine(diagnostics, "scope_stage=scope_ready epoch=", {"scope_hash="});
    CHECK(scopeReady.find("scope_hash=") != std::string::npos);
    const auto catalogPage =
        diagnosticLine(diagnostics, "page_complete request=",
                       {"success=1", "scope_epoch=", "scope_hash=", "source=full_population"});
    CHECK(catalogPage.find("success=1") != std::string::npos);
    CHECK(catalogPage.find("scope_epoch=") != std::string::npos);
    CHECK(catalogPage.find("scope_hash=") != std::string::npos);
    CHECK(catalogPage.find("source=full_population") != std::string::npos);
    const auto coordinatorStage =
        diagnosticLine(diagnostics, "full_population_stage_complete request=");
    CHECK(coordinatorStage.find("generation=") != std::string::npos);
    CHECK(coordinatorStage.find("scope_epoch=") != std::string::npos);
    CHECK(coordinatorStage.find("scope_hash=") != std::string::npos);
    CHECK(coordinatorStage.find("source=full_population") != std::string::npos);
    const auto coordinatorPublish = diagnosticLine(diagnostics, "full_population_publish request=");
    CHECK(coordinatorPublish.find("generation=") != std::string::npos);
    CHECK(coordinatorPublish.find("scope_epoch=") != std::string::npos);
    CHECK(coordinatorPublish.find("scope_hash=") != std::string::npos);
    CHECK(coordinatorPublish.find("source=full_population") != std::string::npos);
    std::remove(diagnosticsPath.c_str());
    return miyoofin_test::finish("library_coordinator_population");
}
