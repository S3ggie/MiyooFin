#include "test_support.hpp"
#include "cases/test_library_coordinator_support.hpp"

namespace {
void testLibraryCoordinatorIsTheSingleStartupDriver()
{
    std::printf("[test] LibraryCoordinator single startup driver\n");
    auto coordinator = makeCoordinator();
    coordinator.start();
    CHECK(coordinator.running());

    // The first caller reserves the only startup slot; a second Home startup
    // request cannot create a competing operation.  Waiting on the publication
    // condition is deterministic and never polls with a sleep.
    CHECK(coordinator.startStartupSync(false));
    CHECK(!coordinator.startStartupSync(true));
    library::StartupSyncResult result;
    CHECK(coordinator.waitStartupSyncResult(result) == library::WaitStatus::Ready);
    CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);

    auto status = coordinator.status();
    CHECK(!status.startupInFlight && !status.cancelRequested);

    // Behavioural lifecycle/re-entry: admitting a new startup after a completed
    // one has been consumed joins the completed prior thread outside the result
    // lock and publishes a fresh result.  A deadlock in that join ordering
    // would hang this blocking wait rather than return.
    CHECK(coordinator.startStartupSync(false));
    library::StartupSyncResult reentry;
    CHECK(coordinator.waitStartupSyncResult(reentry) == library::WaitStatus::Ready);
    CHECK(reentry.error == CatalogDbErrorCategory::ScopeNotReady);

    // Cancellation and stop are both intentionally safe to repeat.
    coordinator.cancelStartupSync();
    coordinator.cancelStartupSync();
    coordinator.stop();
    coordinator.stop();
    CHECK(coordinator.stopped() && !coordinator.running());
    CHECK(!coordinator.takeStartupSyncResult(result));

    // Dependency-direction guards (class D): the coordinator is the single
    // startup driver, so Home/consumers drive it only through the request/wait
    // API, never the raw take* result API, never raw sync/API staging, and
    // never coordinator->stop().  The lock-safe prior-thread join ordering is
    // covered behaviourally by the re-entry wait above.
    const auto homeSync = miyoofin_test::readTestBytes("src/ui/screens/HomeLibraryController.cpp");
    const auto homeUpdate = miyoofin_test::readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(!miyoofin_test::sourceContains(homeSync, "takeStartupSyncResult"));
    CHECK(!miyoofin_test::sourceContains(homeSync, "takeFullPopulationUpdate"));
    for (const char* worker :
         {"src/ui/screens/SeriesScreenWorker.cpp", "src/ui/screens/EpisodeBrowserData.cpp",
          "src/download/DownloadManagerPlanning.cpp"}) {
        const auto workerSource = miyoofin_test::readTestBytes(worker);
        CHECK(!miyoofin_test::sourceContains(workerSource, "takeHierarchyResult"));
    }
    CHECK(!miyoofin_test::sourceContains(homeSync, "decideHomeStartupSync("));
    CHECK(!miyoofin_test::sourceContains(homeSync, "JellyfinApi::getViews"));
    CHECK(!miyoofin_test::sourceContains(homeSync, "JellyfinApi::getLibraryItemsPage"));
    CHECK(!miyoofin_test::sourceContains(homeSync, "sync->begin("));
    CHECK(!miyoofin_test::sourceContains(homeSync, "sync->stage("));
    CHECK(!miyoofin_test::sourceContains(homeSync, "sync->finalize("));
    CHECK(!miyoofin_test::sourceContains(homeSync, "m_libraryCoordinator->stop"));
    CHECK(!miyoofin_test::sourceContains(homeUpdate, "m_libraryCoordinator->stop"));
    const auto stopRequest = miyoofin_test::sourcePos(homeUpdate, "requestStopAllWorkers()");
    const auto controllerJoin = miyoofin_test::sourcePos(homeUpdate, "joinAllWorkers()");
    CHECK(stopRequest < controllerJoin);
    std::printf("[test] LibraryCoordinator single startup driver OK\n");
}

void testStartupAdmissionRejectsUnconsumedResult()
{
    std::printf("[test] LibraryCoordinator startup result-ready admission\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("startup-result-ready", db, scope);
    coordinator->start();

    // Hold the startup worker so the serialized slot is observably in flight.
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->startStartupSync(false));
    CHECK(!coordinator->startStartupSync(true));

    // Release the worker and wait, without consuming, for the result to
    // publish.  Publication is the only way m_startupResultReady becomes true.
    db->setWorkerPausedForTest(false);
    bool published = false;
    for (int i = 0; i < 200000 && !published; ++i) {
        published = coordinator->status().startupResultReady;
        if (!published)
            std::this_thread::yield();
    }
    CHECK(published);

    // The worker has finished (not in flight) but the result is still
    // unconsumed: the next startup must be rejected by the result-ready gate,
    // not merely by the in-flight gate.
    auto status = coordinator->status();
    CHECK(!status.startupInFlight);
    CHECK(status.startupResultReady);
    CHECK(!coordinator->startStartupSync(false));
    CHECK(coordinator->status().startupResultReady);

    // A rejected admission must not overwrite or discard the unconsumed
    // publication, so it is immediately available to the consumer.
    library::StartupSyncResult result;
    CHECK(coordinator->takeStartupSyncResult(result));
    CHECK(result.success || result.cancelled ||
          result.error == CatalogDbErrorCategory::ScopeNotReady);
    CHECK(!coordinator->status().startupResultReady);

    // Consuming the result releases the slot, but this fresh-db startup decided
    // FullReconcile (with no rows seeded), so the sequence hands off to the full
    // population Home requests next.  A competing startup must stay rejected
    // until that intended continuation completes the sequence.
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(!coordinator->startStartupSync(false));

    std::uint64_t populationRequest = 0;
    CHECK(coordinator->requestFullPopulation(populationRequest));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, populationRequest, terminal, sawPage));
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(!coordinator->startupSequenceClaimedForTest());

    // The completed sequence admits the next startup again.
    CHECK(coordinator->startStartupSync(false));
    library::StartupSyncResult reentry;
    CHECK(coordinator->waitStartupSyncResult(reentry) == library::WaitStatus::Ready);

    // Cancellation and stop remain correct and repeatable.
    coordinator->cancelStartupSync();
    coordinator->cancelStartupSync();
    coordinator->stop();
    coordinator->stop();
    CHECK(coordinator->stopped() && !coordinator->running());
    CHECK(!coordinator->takeStartupSyncResult(result));

    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator startup result-ready admission OK\n");
}

void testCoordinatorBlockingWaits()
{
    std::printf("[test] LibraryCoordinator blocking waits\n");

    // A publication that races request completion must still be consumable by
    // a waiter which starts after the worker has published it.
    {
        auto coordinator = makeCoordinator();
        coordinator.start();
        CHECK(coordinator.startStartupSync(false));
        library::StartupSyncResult result;
        CHECK(coordinator.waitStartupSyncResult(result) == library::WaitStatus::Ready);
        CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);
        coordinator.stop();
    }

    // Two waiters observe one publication without either polling or losing the
    // wakeup.  Only one consumer may take the value.
    {
        CoordinatorTestScope scope;
        std::shared_ptr<CatalogDb> db;
        auto coordinator = makeMaintenanceCoordinator("waiters", db, scope);
        coordinator->start();
        db->setWorkerPausedForTest(true);
        CHECK(coordinator->startStartupSync(false));
        // The worker thread reaches the Executing phase asynchronously; a
        // waiter that checks first would see InvalidRequest, not Stopped.
        for (int i = 0; i < 500 && !coordinator->status().startupInFlight; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(coordinator->status().startupInFlight);
        std::mutex barrierMutex;
        std::condition_variable barrierWake;
        int entered = 0;
        auto markEntered = [&] {
            std::lock_guard<std::mutex> lock(barrierMutex);
            ++entered;
            barrierWake.notify_all();
        };
        library::StartupSyncResult first;
        library::StartupSyncResult second;
        library::WaitStatus firstStatus = library::WaitStatus::InvalidRequest;
        library::WaitStatus secondStatus = library::WaitStatus::InvalidRequest;
        std::thread firstWaiter([&] {
            markEntered();
            firstStatus = coordinator->waitStartupSyncResult(first);
        });
        std::thread secondWaiter([&] {
            markEntered();
            secondStatus = coordinator->waitStartupSyncResult(second);
        });
        {
            std::unique_lock<std::mutex> lock(barrierMutex);
            barrierWake.wait(lock, [&] { return entered == 2; });
        }
        db->setWorkerPausedForTest(false);
        firstWaiter.join();
        secondWaiter.join();
        CHECK((firstStatus == library::WaitStatus::Ready) !=
              (secondStatus == library::WaitStatus::Ready));
        CHECK(firstStatus == library::WaitStatus::Ready ||
              secondStatus == library::WaitStatus::Ready);
        CHECK(firstStatus == library::WaitStatus::InvalidRequest ||
              secondStatus == library::WaitStatus::InvalidRequest);
        coordinator->stop();
        coordinator.reset();
        db.reset();
        removeCoordinatorTestScope(scope);
    }

    // Consumer cancellation wakes its waiter without stopping the session.
    {
        CoordinatorTestScope scope;
        std::shared_ptr<CatalogDb> db;
        auto coordinator = makeMaintenanceCoordinator("wait-cancel", db, scope);
        coordinator->start();
        db->setWorkerPausedForTest(true);
        CHECK(coordinator->startStartupSync(false));
        std::atomic_bool consumerCancellation{false};
        std::mutex barrierMutex;
        std::condition_variable barrierWake;
        bool entered = false;
        library::StartupSyncResult result;
        library::WaitStatus waitStatus = library::WaitStatus::InvalidRequest;
        std::thread waiter([&] {
            {
                std::lock_guard<std::mutex> lock(barrierMutex);
                entered = true;
                barrierWake.notify_all();
            }
            waitStatus = coordinator->waitStartupSyncResult(result, &consumerCancellation);
        });
        {
            std::unique_lock<std::mutex> lock(barrierMutex);
            barrierWake.wait(lock, [&] { return entered; });
        }
        consumerCancellation.store(true);
        coordinator->cancelStartupSync();
        db->setWorkerPausedForTest(false);
        waiter.join();
        CHECK(waitStatus == library::WaitStatus::Ready);
        CHECK(result.cancelled || result.error == CatalogDbErrorCategory::ScopeNotReady);
        CHECK(coordinator->running() && !coordinator->stopped());
        coordinator->stop();
        coordinator.reset();
        db.reset();
        removeCoordinatorTestScope(scope);
    }

    // Coordinator stop wakes a waiter before joining the worker.  Releasing
    // the paused CatalogDb worker afterwards proves no lock is held by wait().
    {
        CoordinatorTestScope scope;
        std::shared_ptr<CatalogDb> db;
        auto coordinator = makeMaintenanceCoordinator("wait-stop", db, scope);
        coordinator->start();
        db->setWorkerPausedForTest(true);
        CHECK(coordinator->startStartupSync(false));
        std::mutex barrierMutex;
        std::condition_variable barrierWake;
        bool entered = false;
        library::StartupSyncResult result;
        library::WaitStatus waitStatus = library::WaitStatus::InvalidRequest;
        std::thread waiter([&] {
            {
                std::lock_guard<std::mutex> lock(barrierMutex);
                entered = true;
                barrierWake.notify_all();
            }
            waitStatus = coordinator->waitStartupSyncResult(result);
        });
        {
            std::unique_lock<std::mutex> lock(barrierMutex);
            barrierWake.wait(lock, [&] { return entered; });
        }
        // ponytail: `entered` is set just before the blocking wait begins; a short
        // grace lets the waiter block so stop() races the wait, not its entry.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::thread stopper([&] { coordinator->stop(); });
        waiter.join();
        CHECK(waitStatus == library::WaitStatus::Stopped);
        db->setWorkerPausedForTest(false);
        stopper.join();
        CHECK(coordinator->stopped());
        coordinator.reset();
        db.reset();
        removeCoordinatorTestScope(scope);
    }

    std::printf("[test] LibraryCoordinator blocking waits OK\n");
}

void testCoordinatorWaitIdentityAndDomains()
{
    std::printf("[test] LibraryCoordinator wait identity and domains\n");
    Session session;
    session.serverUrl = "http://127.0.0.1:1";
    auto coordinator =
        std::make_unique<library::LibraryCoordinator>(session, std::make_shared<CatalogDb>(), 0);
    coordinator->start();

    std::uint64_t railRequest = 0;
    CHECK(coordinator->requestHomeRailRefresh(railRequest));
    library::HomeRailResult rail;
    CHECK(coordinator->waitHomeRailResult(railRequest + 1, rail) ==
          library::WaitStatus::InvalidRequest);

    MediaItem series;
    series.id = "superseded-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(coordinator->requestSeriesSeasons(series, hierarchyRequest));
    coordinator->cancelHierarchyRequest(hierarchyRequest);
    library::HierarchyResult hierarchy;
    CHECK(coordinator->waitHierarchyResult(hierarchyRequest, hierarchy) ==
          library::WaitStatus::Superseded);

    coordinator->stop();
    std::printf("[test] LibraryCoordinator wait identity and domains OK\n");
}

void testStopRacingStartupIsSafe()
{
    std::printf("[test] LibraryCoordinator stop vs startup\n");
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto coordinator = makeCoordinator();
        coordinator.start();

        std::atomic_bool go{false};
        std::thread startup([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)coordinator.startStartupSync(false);
        });
        std::thread stopper([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            coordinator.stop();
        });
        go.store(true, std::memory_order_release);

        startup.join();
        stopper.join();
        CHECK(coordinator.stopped());
        CHECK(!coordinator.running());
    }
    std::printf("[test] LibraryCoordinator stop vs startup OK\n");
}
} // namespace

int main()
{
    const std::string diagnosticsPath = "/tmp/miyoofin-library-coordinator-diagnostics-" +
                                        std::to_string(static_cast<long long>(::getpid())) + ".log";
    std::remove(diagnosticsPath.c_str());
    uiDiagnostics().start(diagnosticsPath);

    testLibraryCoordinatorIsTheSingleStartupDriver();
    testStartupAdmissionRejectsUnconsumedResult();
    testCoordinatorBlockingWaits();
    testCoordinatorWaitIdentityAndDomains();
    testStopRacingStartupIsSafe();
    // All producers above have joined.  stop() is the existing logger
    // drain/join barrier, not a timing delay.
    uiDiagnostics().stop();
    std::remove(diagnosticsPath.c_str());
    return miyoofin_test::finish("library_coordinator");
}
