#include "test_support.hpp"
#include "cases/test_library_coordinator_support.hpp"

namespace {
void testSafetyReconcilePublishesCoordinatorResult()
{
    std::printf("[test] LibraryCoordinator safety reconcile result\n");
    Session session;
    auto coordinator = library::LibraryCoordinator(session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    CHECK(coordinator.requestSafetyReconcileForTest());
    CHECK(!coordinator.requestSafetyReconcileForTest());
    library::SafetyReconcileResult result;
    for (int i = 0; i < 200 && !coordinator.takeSafetyReconcileResult(result); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);
    CHECK(!coordinator.status().safetyReconcileInFlight);

    // Architecture guard (class A): Home requests maintenance through the
    // coordinator and never drives safety reconcile itself or owns a safety
    // worker thread.  The coordinator-side publish is exercised above.
    const auto homeHeader = miyoofin_test::readTestBytes("src/ui/screens/HomeScreen.hpp");
    const auto homeSync = miyoofin_test::readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const auto homeApply = miyoofin_test::readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    const auto homeUpdate = miyoofin_test::readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(miyoofin_test::sourceContains(homeUpdate, "requestMaintenance()"));
    CHECK(!miyoofin_test::sourceContains(homeUpdate, "requestSafetyReconcile"));
    CHECK(!miyoofin_test::sourceContains(homeSync, "requestSafetyReconcile"));
    CHECK(!miyoofin_test::sourceContains(homeApply, "requestSafetyReconcile"));
    CHECK(!miyoofin_test::sourceContains(homeHeader, "m_safetyReconcileThread"));
    std::printf("[test] LibraryCoordinator safety reconcile result OK\n");
}

void testSafetyReconcileStopPublishesCompletion()
{
    std::printf("[test] LibraryCoordinator safety reconcile stop\n");
    Session session;
    auto coordinator = library::LibraryCoordinator(session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    CHECK(coordinator.requestSafetyReconcileForTest());
    coordinator.stop();

    library::SafetyReconcileResult result;
    CHECK(coordinator.takeSafetyReconcileResult(result));
    CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);
    coordinator.stop();
    std::printf("[test] LibraryCoordinator safety reconcile stop OK\n");
}

void testMaintenanceDueStartsReconcile()
{
    std::printf("[test] LibraryCoordinator maintenance due\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("maintenance-due", db, scope);
    coordinator->start();

    CHECK(coordinator->status().maintenanceDue);
    CHECK(coordinator->requestMaintenance());
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator maintenance due OK\n");
}

void testMaintenanceNotDueRejectsRequest()
{
    std::printf("[test] LibraryCoordinator maintenance not due\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("maintenance-not-due", db, scope);
    const auto now = coordinatorTestNowMs();
    seedMaintenanceCheckpoint(db, scope.epoch, now - 1000);
    coordinator->start();
    CHECK(coordinator->startStartupSync(true));
    const auto startup = takeStartupResult(*coordinator);
    CHECK(startup.success && startup.mode == library::StartupSyncMode::SkipFresh);
    CHECK(!coordinator->status().maintenanceDue);
    CHECK(!coordinator->requestMaintenance());
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator maintenance not due OK\n");
}

void testMaintenanceElapsedStartsReconcile()
{
    std::printf("[test] LibraryCoordinator maintenance elapsed\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("maintenance-elapsed", db, scope);
    const auto elapsed = coordinatorTestNowMs() - library::kSafetyReconcileIntervalMs - 1000;
    seedMaintenanceCheckpoint(db, scope.epoch, elapsed);
    coordinator->start();
    CHECK(coordinator->startStartupSync(true));
    const auto startup = takeStartupResult(*coordinator);
    CHECK(startup.success && startup.mode == library::StartupSyncMode::FullReconcile);
    CHECK(coordinator->status().maintenanceDue);
    CHECK(coordinator->requestMaintenance());
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator maintenance elapsed OK\n");
}

void testMaintenanceManualOfflineSuppressesRequest()
{
    std::printf("[test] LibraryCoordinator maintenance manual offline\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("maintenance-offline", db, scope, true);
    coordinator->start();
    CHECK(coordinator->status().manualOffline);
    CHECK(!coordinator->status().maintenanceDue);
    CHECK(!coordinator->requestMaintenance());
    coordinator->setManualOfflineMode(false);
    CHECK(coordinator->requestMaintenance());
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator maintenance manual offline OK\n");
}

void testMaintenanceRepeatedRequestRejectsActiveWorker()
{
    std::printf("[test] LibraryCoordinator maintenance repeated request\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("maintenance-repeated", db, scope);
    coordinator->start();
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->requestMaintenance());
    CHECK(!coordinator->requestMaintenance());
    CHECK(coordinator->status().safetyReconcileInFlight);
    coordinator->cancelSafetyReconcile();
    db->setWorkerPausedForTest(false);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator maintenance repeated request OK\n");
}

void testConcurrentMaintenanceRequestsReserveExactlyOnce()
{
    std::printf("[test] LibraryCoordinator concurrent maintenance requests\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("maintenance-concurrent", db, scope);
    coordinator->start();
    db->setWorkerPausedForTest(true);

    std::mutex startMutex;
    std::condition_variable startCondition;
    int ready = 0;
    bool release = false;
    bool firstAccepted = false;
    bool secondAccepted = false;
    const auto request = [&](bool& accepted) {
        {
            std::unique_lock<std::mutex> lock(startMutex);
            ++ready;
            startCondition.notify_all();
            startCondition.wait(lock, [&] { return release; });
        }
        accepted = coordinator->requestMaintenance();
    };

    std::thread first(request, std::ref(firstAccepted));
    std::thread second(request, std::ref(secondAccepted));
    {
        std::unique_lock<std::mutex> lock(startMutex);
        CHECK(startCondition.wait_for(lock, std::chrono::seconds(2), [&] { return ready == 2; }));
        release = true;
        startCondition.notify_all();
    }
    first.join();
    second.join();

    CHECK(firstAccepted != secondAccepted);
    CHECK(coordinator->status().safetyReconcileInFlight);
    coordinator->cancelSafetyReconcile();
    db->setWorkerPausedForTest(false);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator concurrent maintenance requests OK\n");
}

void testMaintenanceRejectsActiveStartup()
{
    std::printf("[test] LibraryCoordinator maintenance active startup\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("maintenance-active", db, scope);
    coordinator->start();
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->startStartupSync(false));
    CHECK(!coordinator->requestMaintenance());
    CHECK(coordinator->status().startupInFlight);
    coordinator->cancelStartupSync();
    db->setWorkerPausedForTest(false);
    (void)takeStartupResult(*coordinator);
    CHECK(coordinator->startStartupSync(false));
    (void)takeStartupResult(*coordinator);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator maintenance active startup OK\n");
}

void testMaintenanceRejectsIncompatibleHierarchyMutation()
{
    std::printf("[test] LibraryCoordinator maintenance hierarchy mutation\n");
    const int listener = coordinatorTestListener();
    if (listener < 0)
        return;
    const auto serverUrl = coordinatorTestUrl(listener);
    std::thread server([&] {
        const int client = coordinatorAccept(listener);
        if (client < 0)
            return;
        coordinatorReadRequest(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::close(client);
    });

    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator =
        makeMaintenanceCoordinator("maintenance-mutation", db, scope, false, serverUrl);
    coordinator->start();
    MediaItem series;
    series.id = "mutation-series";
    std::uint64_t request = 0;
    CHECK(coordinator->requestSeriesSeasons(series, request));
    CHECK(!coordinator->requestMaintenance());
    coordinator->cancelHierarchyRequest(request);
    coordinator->stop();
    server.join();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator maintenance hierarchy mutation OK\n");
}

void testMaintenanceDefersUntilCoordinatorStarts()
{
    std::printf("[test] LibraryCoordinator maintenance deferred start\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("maintenance-deferred", db, scope);
    CHECK(!coordinator->requestMaintenance());
    coordinator->start();
    CHECK(coordinator->requestMaintenance());
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator maintenance deferred start OK\n");
}

void testHomeRailStopPublishesCompletion()
{
    std::printf("[test] LibraryCoordinator Home rail lifecycle\n");
    Session session;
    session.serverUrl = "http://127.0.0.1:1";
    auto coordinator = library::LibraryCoordinator(session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    std::uint64_t request = 0;
    CHECK(coordinator.requestHomeRailRefresh(request));
    coordinator.stop();

    // stop() may race the transport worker.  Either way, the worker must
    // publish a terminal (possibly cancelled) result for Home's wait loop.
    library::HomeRailResult result;
    CHECK(coordinator.waitHomeRailResult(request, result) == library::WaitStatus::Ready);
    CHECK(result.request == request);
    std::printf("[test] LibraryCoordinator Home rail lifecycle OK\n");
}

void testHomeRailCancellationReleasesSlot()
{
    std::printf("[test] LibraryCoordinator Home rail cancellation reuse\n");
    // A listener that accepts connections (kernel backlog) but never answers
    // keeps each request in flight until the cancellation lands. A refused
    // port fails fast and can finish before the cancel, so it is not used.
    const int listener = coordinatorTestListener();
    if (listener < 0)
        return;
    Session session;
    session.serverUrl = coordinatorTestUrl(listener);
    session.userId = "home-rail-cancel-user";
    auto coordinator = library::LibraryCoordinator(session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    std::uint64_t firstRequest = 0;
    CHECK(coordinator.requestHomeRailRefresh(firstRequest));
    coordinator.cancelHomeRailRefresh();
    library::HomeRailResult firstResult;
    CHECK(coordinator.waitHomeRailResult(firstRequest, firstResult) == library::WaitStatus::Ready);
    CHECK(firstResult.request == firstRequest && firstResult.cancelled);
    CHECK(coordinator.running() && !coordinator.stopped());

    std::uint64_t secondRequest = 0;
    CHECK(coordinator.requestHomeRailRefresh(secondRequest));
    coordinator.cancelHomeRailRefresh();
    library::HomeRailResult secondResult;
    CHECK(coordinator.waitHomeRailResult(secondRequest, secondResult) ==
          library::WaitStatus::Ready);
    CHECK(secondResult.request == secondRequest && secondResult.cancelled);
    coordinator.stop();
    ::close(listener);
    std::printf("[test] LibraryCoordinator Home rail cancellation reuse OK\n");
}

void testHomeRailSuccessPublishesBothRails()
{
    std::printf("[test] LibraryCoordinator Home rail success\n");
    const int listener = coordinatorTestListener();
    if (listener < 0)
        return;
    std::thread server([&] {
        serveHomeRailResponses(
            listener, {
                          {R"({"Items":[{"Id":"continue-1","Type":"Episode","Name":"Continue"}]})"},
                          {R"({"Items":[{"Id":"recent-1","Type":"Movie","Name":"Recent"}]})"},
                      });
    });

    Session session;
    session.serverUrl = coordinatorTestUrl(listener);
    session.userId = "home-rail-user";
    auto coordinator =
        std::make_unique<library::LibraryCoordinator>(session, std::make_shared<CatalogDb>(), 0);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestHomeRailRefresh(request));
    library::HomeRailResult result;
    CHECK(takeHomeRailResult(*coordinator, request, result));
    server.join();

    CHECK(result.request == request);
    CHECK(result.success && !result.cancelled);
    CHECK(result.continueValid && result.recentlyAddedValid);
    CHECK(result.continueWatching.size() == 1);
    CHECK(result.recentlyAdded.size() == 1);
    CHECK(result.continueWatching[0].id == "continue-1");
    CHECK(result.recentlyAdded[0].id == "recent-1");
    coordinator->stop();
    coordinator.reset();
    ::close(listener);
    std::printf("[test] LibraryCoordinator Home rail success OK\n");
}

void testHomeRailCachedFailureRetainsInvalidRail()
{
    std::printf("[test] LibraryCoordinator Home rail cached failure\n");
    const int listener = coordinatorTestListener();
    if (listener < 0)
        return;
    std::thread server([&] {
        serveHomeRailResponses(
            listener,
            {
                {R"({"Error":"resume unavailable"})", 500},
                {R"({"Items":[{"Id":"recent-after-failure","Type":"Movie","Name":"Recent"}]})"},
            });
    });

    Session session;
    session.serverUrl = coordinatorTestUrl(listener);
    session.userId = "home-rail-cache-user";
    auto coordinator =
        std::make_unique<library::LibraryCoordinator>(session, std::make_shared<CatalogDb>(), 0);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestHomeRailRefresh(request));
    library::HomeRailResult result;
    CHECK(takeHomeRailResult(*coordinator, request, result));
    server.join();

    // Home must retain its cached Continue Watching row when this optional
    // rail fails, while still consuming the valid Recently Added result.
    CHECK(result.success && !result.cancelled);
    CHECK(!result.continueValid && result.continueWatching.empty());
    CHECK(result.recentlyAddedValid && result.recentlyAdded.size() == 1);
    CHECK(!result.error.empty());
    coordinator->stop();
    coordinator.reset();
    ::close(listener);
    std::printf("[test] LibraryCoordinator Home rail cached failure OK\n");
}

void testHomeRailCoalescesAndRerunsAfterConsumption()
{
    std::printf("[test] LibraryCoordinator Home rail coalescing\n");
    const int listener = coordinatorTestListener();
    if (listener < 0)
        return;
    std::thread server([&] {
        serveHomeRailResponses(listener, {
                                             {R"({"Items":[]})"},
                                             {R"({"Items":[]})"},
                                             {R"({"Items":[]})"},
                                             {R"({"Items":[]})"},
                                         });
    });

    Session session;
    session.serverUrl = coordinatorTestUrl(listener);
    session.userId = "home-rail-coalesce-user";
    auto coordinator =
        std::make_unique<library::LibraryCoordinator>(session, std::make_shared<CatalogDb>(), 0);
    coordinator->start();
    std::uint64_t firstRequest = 0;
    std::uint64_t duplicateRequest = 0;
    CHECK(coordinator->requestHomeRailRefresh(firstRequest));
    CHECK(!coordinator->requestHomeRailRefresh(duplicateRequest));

    library::HomeRailResult firstResult;
    CHECK(takeHomeRailResult(*coordinator, firstRequest, firstResult));
    std::uint64_t secondRequest = 0;
    CHECK(coordinator->requestHomeRailRefresh(secondRequest));
    CHECK(secondRequest != firstRequest);
    CHECK(!coordinator->requestHomeRailRefresh(duplicateRequest));

    library::HomeRailResult secondResult;
    CHECK(takeHomeRailResult(*coordinator, secondRequest, secondResult));
    server.join();
    CHECK(firstResult.success && secondResult.success);
    coordinator->stop();
    coordinator.reset();
    ::close(listener);
    std::printf("[test] LibraryCoordinator Home rail coalescing OK\n");
}
} // namespace

int main()
{
    const std::string diagnosticsPath = "/tmp/miyoofin-library-coordinator-diagnostics-" +
                                        std::to_string(static_cast<long long>(::getpid())) + ".log";
    std::remove(diagnosticsPath.c_str());
    uiDiagnostics().start(diagnosticsPath);

    testSafetyReconcilePublishesCoordinatorResult();
    testSafetyReconcileStopPublishesCompletion();
    testMaintenanceDueStartsReconcile();
    testMaintenanceNotDueRejectsRequest();
    testMaintenanceElapsedStartsReconcile();
    testMaintenanceManualOfflineSuppressesRequest();
    testMaintenanceRepeatedRequestRejectsActiveWorker();
    testConcurrentMaintenanceRequestsReserveExactlyOnce();
    testMaintenanceRejectsActiveStartup();
    testMaintenanceRejectsIncompatibleHierarchyMutation();
    testMaintenanceDefersUntilCoordinatorStarts();
    testHomeRailStopPublishesCompletion();
    testHomeRailCancellationReleasesSlot();
    testHomeRailSuccessPublishesBothRails();
    testHomeRailCachedFailureRetainsInvalidRail();
    testHomeRailCoalescesAndRerunsAfterConsumption();
    // All producers above have joined.  stop() is the existing logger
    // drain/join barrier, not a timing delay.
    uiDiagnostics().stop();
    std::remove(diagnosticsPath.c_str());
    return miyoofin_test::finish("library_coordinator_maintenance");
}
