#include "test_support.hpp"
#include "../src/library/LibraryCoordinator.hpp"
#include "../src/net/JellyfinLibraryEvents.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace miyoofin;

namespace {

library::LibraryCoordinator makeCoordinator()
{
    Session session;
    session.manualOfflineMode = true;
    return library::LibraryCoordinator(session, std::make_shared<CatalogDb>(), 0);
}

struct CoordinatorTestScope
{
    std::string url;
    std::string user;
    std::string scope;
    std::uint64_t epoch = 0;
};

CoordinatorTestScope coordinatorTestScope(const char* name)
{
    CoordinatorTestScope scope;
    scope.url = std::string("https://coordinator-") + name + "-" +
                std::to_string(static_cast<long long>(::getpid())) + ".example";
    scope.user = std::string("coordinator-") + name + "-user";
    scope.scope = LibraryCache::scopeKey(scope.url, scope.user);
    const std::string base = "cache/library/" + scope.scope + "/catalog.sqlite3";
    const std::string files[] = {base,
                                 base + "-journal",
                                 base + "-wal",
                                 base + "-shm",
                                 base + ".migrating",
                                 base + ".migrating-journal",
                                 base + ".migrating-wal",
                                 base + ".migrating-shm"};
    for (const auto& file : files)
        std::remove(file.c_str());
    return scope;
}

void removeCoordinatorTestScope(const CoordinatorTestScope& scope)
{
    const std::string libraryDirectory = "cache/library/" + scope.scope;
    const std::string base = libraryDirectory + "/catalog.sqlite3";
    const std::string files[] = {base,
                                 base + "-journal",
                                 base + "-wal",
                                 base + "-shm",
                                 base + ".migrating",
                                 base + ".migrating-journal",
                                 base + ".migrating-wal",
                                 base + ".migrating-shm"};
    for (const auto& file : files)
        std::remove(file.c_str());
    ::rmdir(libraryDirectory.c_str());
}

std::int64_t coordinatorTestNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::unique_ptr<library::LibraryCoordinator>
makeMaintenanceCoordinator(const char* name, std::shared_ptr<CatalogDb>& db,
                           CoordinatorTestScope& scope, bool manualOffline = false,
                           const std::string& serverUrl = "http://127.0.0.1:1")
{
    scope = coordinatorTestScope(name);
    db = std::make_shared<CatalogDb>();
    scope.epoch = db->configureScope(scope.url, scope.user);
    CHECK(scope.epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    Session session;
    session.serverUrl = serverUrl;
    session.userId = scope.user;
    session.manualOfflineMode = manualOffline;
    return std::make_unique<library::LibraryCoordinator>(session, db, scope.epoch);
}

void seedMaintenanceCheckpoint(const std::shared_ptr<CatalogDb>& db, std::uint64_t epoch,
                               std::int64_t timestamp)
{
    const auto seeded = db->writeSyncState(timestamp, timestamp, 1, {0, epoch, {}}).get();
    CHECK(seeded.success);
}

library::StartupSyncResult takeStartupResult(library::LibraryCoordinator& coordinator)
{
    library::StartupSyncResult result;
    const auto status = coordinator.waitStartupSyncResult(result);
    if (status != library::WaitStatus::Ready) {
        bool took = false;
        for (int i = 0; i < 500 && !took; ++i) {
            took = coordinator.takeStartupSyncResult(result);
            if (!took)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(took);
    }
    return result;
}

int coordinatorTestListener()
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0)
        return -1;
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(::listen(listener, 4) == 0);
    return listener;
}

std::string coordinatorTestUrl(int listener)
{
    sockaddr_in address{};
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
    return "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
}

int coordinatorAccept(int listener)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval timeout{5, 0};
    if (::select(listener + 1, &readable, nullptr, nullptr, &timeout) <= 0)
        return -1;
    return ::accept(listener, nullptr, nullptr);
}

void coordinatorReadRequest(int client)
{
    char buffer[4096];
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break;
        request.append(buffer, static_cast<std::size_t>(received));
    }
}

void coordinatorSendJson(int client, const std::string& body, int status = 200)
{
    const char* statusText = status == 200 ? "OK" : "Internal Server Error";
    const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + statusText +
                                 "\r\nContent-Type: application/json\r\n" +
                                 "Content-Length: " + std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + body;
    (void)::send(client, response.data(), response.size(), 0);
}

bool takeFullUpdate(library::LibraryCoordinator& coordinator, std::uint64_t request,
                    library::FullPopulationUpdate& terminal, bool& sawPage)
{
    for (;;) {
        library::FullPopulationUpdate update;
        if (coordinator.waitFullPopulationUpdate(request, update) != library::WaitStatus::Ready) {
            for (int i = 0; i < 500; ++i) {
                if (coordinator.takeFullPopulationUpdate(request, update))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!update.terminal && !update.pageValid)
                return false;
        }
        sawPage = sawPage || update.pageValid;
        if (update.terminal) {
            terminal = std::move(update);
            return true;
        }
    }
}

struct HomeRailResponse
{
    std::string body;
    int status = 200;
};

bool takeHomeRailResult(library::LibraryCoordinator& coordinator, std::uint64_t request,
                        library::HomeRailResult& result)
{
    return coordinator.waitHomeRailResult(request, result) == library::WaitStatus::Ready;
}

void serveHomeRailResponses(int listener, const std::vector<HomeRailResponse>& responses)
{
    for (const auto& response : responses) {
        const int client = coordinatorAccept(listener);
        if (client < 0)
            return;
        coordinatorReadRequest(client);
        coordinatorSendJson(client, response.body, response.status);
        ::close(client);
    }
}

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

void testLiveChangesWaitForSerializedSyncSlots()
{
    std::printf("[test] LibraryCoordinator live-change result seam\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    JellyfinLibraryChangeBatch batch;
    batch.itemsUpdated.push_back("item-1");
    CHECK(coordinator.requestLiveChange(batch));
    JellyfinLibraryChangeBatch queued;
    queued.itemsUpdated.push_back("item-2");
    CHECK(coordinator.requestLiveChange(queued));
    library::LiveLibraryChangeResult liveResult;
    for (int i = 0; i < 200 && !coordinator.takeLiveChangeResult(liveResult); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(!liveResult.success);
    CHECK(liveResult.error == CatalogDbErrorCategory::ScopeNotReady);

    // A published result holds the serialized slot until Home consumes it;
    // the queued second change is then published afterward.
    library::LiveLibraryChangeResult nextResult;
    for (int i = 0; i < 200 && !coordinator.takeLiveChangeResult(nextResult); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(!nextResult.success);
    coordinator.stop();

    // Home consumes coordinator publications and no longer owns a live-change
    // worker or identity/publish handshake.
    // Architecture guard (class A): Home owns no live-change worker thread;
    // the coordinator drives the apply.
    const auto homeSync = miyoofin_test::readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(miyoofin_test::sourcePos(homeSync, "m_liveChangeThread") == std::string::npos);
    std::printf("[test] LibraryCoordinator live-change result seam OK\n");
}

void testLiveChangeQueueFullDrainFallsBackToCatchUp()
{
    std::printf("[test] LibraryCoordinator live-change queue-full drain\n");

    // Model the LibrarySync -> coordinator transfer with the same bounded
    // queue semantics.  Existing ids still coalesce, while a new id that
    // cannot fit is represented by catch-up instead of being silently lost.
    JellyfinLibraryEventQueue coordinatorQueue(3);
    JellyfinLibraryChangeBatch queued;
    queued.itemsUpdated = {"a", "b", "c"};
    CHECK(coordinatorQueue.push(queued));

    JellyfinLibraryChangeBatch incoming;
    incoming.itemsUpdated = {"b", "d"};
    incoming.userDataChanged = true;
    JellyfinLibraryEventQueue librarySyncQueue(3);
    CHECK(librarySyncQueue.push(incoming));
    JellyfinLibraryChangeBatch transferred;
    CHECK(librarySyncQueue.pop(transferred));
    CHECK(!librarySyncQueue.pop(incoming));
    CHECK(!coordinatorQueue.push(transferred));
    CHECK(coordinatorQueue.overflowed());

    JellyfinLibraryChangeBatch drained;
    CHECK(coordinatorQueue.pop(drained));
    CHECK(drained.catchUpRequired);
    CHECK(drained.userDataChanged);
    CHECK(drained.itemsUpdated.size() == 3);
    CHECK(drained.itemsUpdated[0] == "a");
    CHECK(drained.itemsUpdated[1] == "b");
    CHECK(drained.itemsUpdated[2] == "c");
    CHECK(!coordinatorQueue.pop(drained));

    std::printf("[test] LibraryCoordinator live-change queue-full drain OK\n");
}

void testLiveChangeCatchUpFailureIsRetriedByCoordinator()
{
    std::printf("[test] LibraryCoordinator live-change catch-up retry\n");
    const auto scope = coordinatorTestScope("live-catch-up-retry");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    const auto seeded = db->writeSyncState(1000, 0, 0, {0, epoch, {}}).get();
    CHECK(seeded.success);

    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    std::thread server([&] {
        const int statuses[] = {500, 200};
        for (const int status : statuses) {
            const int client = coordinatorAccept(listener);
            if (client < 0)
                return;
            coordinatorReadRequest(client);
            coordinatorSendJson(
                client, status == 200 ? R"({"Items":[]})" : R"({"Error":"transient"})", status);
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

    JellyfinLibraryChangeBatch overflow;
    overflow.catchUpRequired = true;
    CHECK(coordinator->requestLiveChange(overflow));

    library::LiveLibraryChangeResult failed;
    bool tookFailure = false;
    for (int i = 0; i < 1000 && !tookFailure; ++i) {
        tookFailure = coordinator->takeLiveChangeResult(failed);
        if (!tookFailure)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(tookFailure);
    CHECK(!failed.success && failed.catchUpRequired);

    library::LiveLibraryChangeResult retried;
    bool tookRetry = false;
    for (int i = 0; i < 1000 && !tookRetry; ++i) {
        tookRetry = coordinator->takeLiveChangeResult(retried);
        if (!tookRetry)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(tookRetry);
    CHECK(retried.success && retried.catchUpRequired);

    coordinator->stop();
    server.join();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator live-change catch-up retry OK\n");
}

void testLiveChangeCatchUpApplyFailureRetainsBarrier()
{
    std::printf("[test] LibraryCoordinator catch-up apply failure retry\n");
    const auto scope = coordinatorTestScope("live-catch-up-apply-retry");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    const auto seeded = db->writeSyncState(1000, 0, 0, {0, epoch, {}}).get();
    CHECK(seeded.success);

    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    const int statuses[] = {200, 500, 200, 200};
    std::atomic<int> requestCount{0};
    std::thread server([&] {
        for (const int status : statuses) {
            const int client = coordinatorAccept(listener);
            if (client < 0)
                return;
            coordinatorReadRequest(client);
            ++requestCount;
            coordinatorSendJson(
                client, status == 500 ? R"({"Error":"transient"})" : R"({"Items":[]})", status);
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

    JellyfinLibraryChangeBatch overflow;
    overflow.catchUpRequired = true;
    overflow.itemsUpdated.push_back("apply-after-catch-up");
    CHECK(coordinator->requestLiveChange(overflow));

    library::LiveLibraryChangeResult failed;
    bool tookFailure = false;
    for (int i = 0; i < 1000 && !tookFailure; ++i) {
        tookFailure = coordinator->takeLiveChangeResult(failed);
        if (!tookFailure)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(tookFailure);
    CHECK(!failed.success && failed.catchUpRequired);

    library::LiveLibraryChangeResult retried;
    bool tookRetry = false;
    for (int i = 0; i < 1000 && !tookRetry; ++i) {
        tookRetry = coordinator->takeLiveChangeResult(retried);
        if (!tookRetry)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(tookRetry);
    CHECK(retried.success && retried.catchUpRequired);

    coordinator->stop();
    server.join();
    CHECK(requestCount == 4);
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator catch-up apply failure retry OK\n");
}

void testLiveChangeCatchUpAndApplyPublishExactlyOnce()
{
    std::printf("[test] LibraryCoordinator catch-up plus apply publication\n");
    const auto scope = coordinatorTestScope("live-catch-up-apply-once");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    const auto seeded = db->writeSyncState(1000, 0, 0, {0, epoch, {}}).get();
    CHECK(seeded.success);

    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    std::atomic<int> requestCount{0};
    std::thread server([&] {
        const std::string bodies[] = {
            R"({"Items":[]})",
            R"({"Items":[{"Id":"applied-once","Type":"movie","Name":"Applied Once"}]})",
        };
        for (const auto& body : bodies) {
            const int client = coordinatorAccept(listener);
            if (client < 0)
                return;
            coordinatorReadRequest(client);
            ++requestCount;
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

    JellyfinLibraryChangeBatch batch;
    batch.catchUpRequired = true;
    batch.itemsUpdated.push_back("applied-once");
    CHECK(coordinator->requestLiveChange(batch));

    library::LiveLibraryChangeResult result;
    bool tookResult = false;
    for (int i = 0; i < 1000 && !tookResult; ++i) {
        tookResult = coordinator->takeLiveChangeResult(result);
        if (!tookResult)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(tookResult);
    CHECK(result.success && result.catchUpRequired);
    CHECK(result.itemsFetched == 1 && result.itemsUpserted == 1);
    CHECK(result.generation == 1);

    // Catch-up and item application are one coordinator operation: Home gets
    // exactly one success publication, not a catch-up publication followed by
    // a second apply publication.
    library::LiveLibraryChangeResult duplicate;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!coordinator->takeLiveChangeResult(duplicate));
    server.join();
    CHECK(requestCount == 2);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator catch-up plus apply publication OK\n");
}

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
    Session session;
    session.serverUrl = "http://127.0.0.1:1";
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

void testFullPopulationSuccessCommitsCheckpoint()
{
    std::printf("[test] LibraryCoordinator full population success\n");
    const auto scope = coordinatorTestScope("success");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

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
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

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
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
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
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
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
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

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

    // A paused safety worker similarly blocks startup, population, and live
    // consumption while retaining the queued event.
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->requestSafetyReconcileForTest());
    CHECK(!coordinator->startStartupSync(false));
    CHECK(!coordinator->requestFullPopulation(request));
    CHECK(!coordinator->takeLiveChangeResult(liveResult));
    coordinator->cancelSafetyReconcile();
    db->setWorkerPausedForTest(false);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator serialization gates OK\n");
}

void testLegacyFullSyncReservationGatesAndReleases()
{
    std::printf("[test] LibraryCoordinator legacy reservation\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("legacy-reservation", db, scope);
    coordinator->start();

    // The legacy reservation owns the serialized slot without a worker; every
    // other top-level operation must observe it as occupied.
    CHECK(coordinator->beginFullSync());
    CHECK(!coordinator->beginFullSync());
    CHECK(!coordinator->startStartupSync(false));
    std::uint64_t populationRequest = 0;
    CHECK(!coordinator->requestFullPopulation(populationRequest));
    CHECK(!coordinator->requestMaintenance());
    MediaItem series;
    series.id = "legacy-gated-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(!coordinator->requestSeriesSeasons(series, hierarchyRequest));

    // Discarding live results must not release the slot held by a different
    // operation; the model only releases the matching kind.
    coordinator->discardLiveChangeResults();
    CHECK(!coordinator->startStartupSync(false));

    // Releasing the reservation admits the next serialized operation.
    coordinator->finishFullSync();
    CHECK(coordinator->startStartupSync(false));
    const auto startup = takeStartupResult(*coordinator);
    CHECK(startup.success || startup.cancelled ||
          startup.error == CatalogDbErrorCategory::ScopeNotReady);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator legacy reservation OK\n");
}

void testConcurrentLegacyReservationReservesExactlyOnce()
{
    std::printf("[test] LibraryCoordinator concurrent reservation\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    std::mutex startMutex;
    std::condition_variable startCondition;
    int ready = 0;
    bool release = false;
    bool firstAccepted = false;
    bool secondAccepted = false;
    const auto reserve = [&](bool& accepted) {
        {
            std::unique_lock<std::mutex> lock(startMutex);
            ++ready;
            startCondition.notify_all();
            startCondition.wait(lock, [&] { return release; });
        }
        accepted = coordinator.beginFullSync();
    };

    std::thread first(reserve, std::ref(firstAccepted));
    std::thread second(reserve, std::ref(secondAccepted));
    {
        std::unique_lock<std::mutex> lock(startMutex);
        CHECK(startCondition.wait_for(lock, std::chrono::seconds(2), [&] { return ready == 2; }));
        release = true;
        startCondition.notify_all();
    }
    first.join();
    second.join();

    // The single serialized-slot authority admits exactly one reservation.
    CHECK(firstAccepted != secondAccepted);
    coordinator.finishFullSync();
    CHECK(coordinator.beginFullSync());
    coordinator.finishFullSync();

    coordinator.stop();
    std::printf("[test] LibraryCoordinator concurrent reservation OK\n");
}

void testUnconsumedStartupResultRetainsSerializedSlot()
{
    std::printf("[test] LibraryCoordinator result retains serialized slot\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("result-slot", db, scope);
    coordinator->start();

    db->setWorkerPausedForTest(true);
    CHECK(coordinator->startStartupSync(false));
    db->setWorkerPausedForTest(false);

    bool published = false;
    for (int i = 0; i < 200000 && !published; ++i) {
        published = coordinator->status().startupResultReady;
        if (!published)
            std::this_thread::yield();
    }
    CHECK(published);

    // An unconsumed startup publication retains the slot for every other
    // operation, not merely for a second startup.
    CHECK(!coordinator->beginFullSync());
    MediaItem series;
    series.id = "result-slot-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(!coordinator->requestSeriesSeasons(series, hierarchyRequest));

    // Consuming the result releases the slot for a different operation.
    library::StartupSyncResult result;
    CHECK(coordinator->takeStartupSyncResult(result));
    CHECK(coordinator->beginFullSync());
    coordinator->finishFullSync();

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator result retains serialized slot OK\n");
}

// A safety reconcile releases the serialized slot as soon as its mutation is
// durable while keeping the terminal result for Home.  Same-kind admission is
// therefore gated on the pending result (not on slot occupancy), and a
// different kind such as hierarchy is admitted with the result unconsumed.
void testUnconsumedSafetyResultAdmitsHierarchyAndSurvives()
{
    std::printf("[test] LibraryCoordinator unconsumed safety result admits hierarchy\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("safety-unconsumed", db, scope);
    coordinator->start();

    CHECK(coordinator->requestSafetyReconcileForTest());
    for (int i = 0; i < 2000000 && !coordinator->safetyReconcileResultReadyForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator->safetyReconcileResultReadyForTest());
    // The slot is free once the mutation finishes, not merely after Home
    // consumes the result.
    CHECK(!coordinator->status().safetyReconcileInFlight);

    // A second same-kind request is rejected by the pending result while the
    // slot is otherwise free, so it cannot overwrite the publication.
    CHECK(!coordinator->requestSafetyReconcileForTest());

    // A different kind is admitted with the result still unconsumed.
    MediaItem series;
    series.id = "safety-unconsumed-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(coordinator->requestSeriesSeasons(series, hierarchyRequest));

    // The retained result is still consumable after the hierarchy admission.
    library::SafetyReconcileResult result;
    CHECK(coordinator->takeSafetyReconcileResult(result));
    CHECK(!coordinator->takeSafetyReconcileResult(result));

    coordinator->cancelHierarchyRequest(hierarchyRequest);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator unconsumed safety result admits hierarchy OK\n");
}

// Hierarchy is admitted while an unconsumed live result retains its
// publication, and the result survives that admission.
void testUnconsumedLiveResultAdmitsHierarchy()
{
    std::printf("[test] LibraryCoordinator unconsumed live result admits hierarchy\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    JellyfinLibraryChangeBatch first;
    first.itemsUpdated.push_back("live-first");
    first.userDataChanged = true;
    CHECK(coordinator.requestLiveChange(first));
    for (int i = 0; i < 2000000 && !coordinator.liveChangeResultReadyForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator.liveChangeResultReadyForTest());

    MediaItem series;
    series.id = "live-unconsumed-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(coordinator.requestSeriesSeasons(series, hierarchyRequest));

    // The unconsumed result survives the hierarchy admission.
    library::LiveLibraryChangeResult result;
    CHECK(coordinator.takeLiveChangeResult(result));
    CHECK(result.userDataChanged);

    coordinator.cancelHierarchyRequest(hierarchyRequest);
    coordinator.stop();
    std::printf("[test] LibraryCoordinator unconsumed live result admits hierarchy OK\n");
}

// A queued later live batch must neither overwrite an unconsumed result nor be
// lost once that result is consumed.
void testUnconsumedLiveResultIsNotOverwritten()
{
    std::printf("[test] LibraryCoordinator unconsumed live result not overwritten\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    JellyfinLibraryChangeBatch first;
    first.itemsUpdated.push_back("live-first");
    first.userDataChanged = true;
    CHECK(coordinator.requestLiveChange(first));
    for (int i = 0; i < 2000000 && !coordinator.liveChangeResultReadyForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator.liveChangeResultReadyForTest());

    // Queue a second live batch while the first result is still unconsumed.
    JellyfinLibraryChangeBatch second;
    second.itemsUpdated.push_back("live-second");
    second.userDataChanged = false;
    CHECK(coordinator.requestLiveChange(second));

    // The unconsumed result is the first batch's, not an overwrite.
    library::LiveLibraryChangeResult result;
    CHECK(coordinator.takeLiveChangeResult(result));
    CHECK(result.userDataChanged);

    // After consumption the queued batch is processed and published, so no
    // live change is silently lost.
    for (int i = 0; i < 2000000 && !coordinator.liveChangeResultReadyForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator.liveChangeResultReadyForTest());
    library::LiveLibraryChangeResult next;
    CHECK(coordinator.takeLiveChangeResult(next));
    CHECK(!next.userDataChanged);

    coordinator.stop();
    std::printf("[test] LibraryCoordinator unconsumed live result not overwritten OK\n");
}

// Cold-start precedence: a live change that becomes available while Home's
// startup and initial full population are still pending must not preempt them.
// Startup and population both complete first; the retained live change is then
// processed rather than lost.  Ordering is established with the coordinator's
// condition-variable seams rather than polling: the demand is armed before the
// live worker starts, and waitLiveChangeDeferredForTest proves the worker
// reached the gate without claiming the serialized slot.
void testStartupPopulationPrecedesLiveChangeAtColdStart()
{
    std::printf("[test] LibraryCoordinator cold-start live precedence\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("cold-start-live", db, scope);
    // Home's startup owner reserves cold-start precedence before the live
    // worker starts, so a queued live batch cannot claim the slot first.
    const auto startupToken = coordinator->reserveStartupSequence();
    coordinator->start();

    // The live change is available before Home requests startup.  The
    // reservation makes the live worker defer it deterministically.
    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));

    // Block until the worker has observed the cold-start gate; no sleeps or
    // yield budgets are involved.
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Startup wins the serialized slot despite the queued live change.
    CHECK(coordinator->startStartupSync(false));
    // A release from the original owner after startup has claimed the sequence
    // must be a no-op: the coordinator now owns the cold-start demand, so the
    // retained live change stays gated across the startup -> population
    // handoff.
    coordinator->releaseStartupSequence(startupToken);
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    // Startup is complete, but its decision requires a full population, so the
    // live change stays deferred across the startup -> population handoff.
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Full population also wins the slot and completes.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));

    // Draining the cold-start sequence released the demand; the retained live
    // change is now applied rather than lost.  Wait on the publication
    // condition variable instead of polling for it.
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.userDataChanged);
    CHECK(applied.success);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator cold-start live precedence OK\n");
}

// Regression: cold-start precedence is owned by the startup sequence, not
// armed unconditionally for every online start.  A coordinator that starts and
// never reserves must still process live work instead of deferring forever.
void testStartOnlyCoordinatorProcessesLiveChange()
{
    std::printf("[test] LibraryCoordinator start-only live processing\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("start-only-live", db, scope);
    // Deliberately no reserveStartupSequence(): this coordinator never runs
    // Home's startup/full-population sequence.
    coordinator->start();

    // A user-data-only change is a valid live batch that applies without any
    // network round trip, so success proves the worker actually processed it.
    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));

    // The worker must claim the serialized slot and publish rather than park
    // behind an unclaimed startup reservation.
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult result;
    CHECK(coordinator->takeLiveChangeResult(result));
    CHECK(result.success);
    CHECK(result.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator start-only live processing OK\n");
}

// Regression: an abandoned startup reservation must be releasable by its owner.
// AppSession reserves cold-start precedence before start(), but if Home's
// startup sequence is never run the reservation would otherwise defer every
// live change forever.  Releasing the still-unclaimed reservation must let the
// retained live batch apply.  No sleeps are involved: the gate is observed
// through the coordinator's condition-variable seams.
void testAbandonedStartupReservationReleasesLiveProcessing()
{
    std::printf("[test] LibraryCoordinator abandoned reservation release\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("abandoned-reservation", db, scope);
    const auto reservationToken = coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));

    // The unclaimed reservation deterministically gates the live worker.
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // The owner abandons Home's startup sequence before it ever starts.
    coordinator->releaseStartupSequence(reservationToken);
    // Releasing again is idempotent and must not disturb the resumed worker.
    coordinator->releaseStartupSequence(reservationToken);

    // The retained live change is now applied rather than stranded.
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.userDataChanged);
    CHECK(applied.success);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator abandoned reservation release OK\n");
}

// Regression: leaving manual offline with the cold-start reservation still
// unclaimed must re-arm the startup demand before Home's online fetch runs.
// Entering offline clears the demand (so retained live changes still apply),
// but the reservation itself must keep gating the live worker once the session
// returns online.
void testManualOfflineReturnReArmsStartupReservation()
{
    std::printf("[test] LibraryCoordinator offline->online re-arms reservation\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("offline-rearm", db, scope);
    // Online reserve, then go offline before any startup request claims it.
    coordinator->reserveStartupSequence();
    coordinator->start();

    coordinator->setManualOfflineMode(true);
    // Offline must not gate live processing: a user-data-only change applies.
    JellyfinLibraryChangeBatch offlineBatch;
    offlineBatch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(offlineBatch));
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult offlineResult;
    CHECK(coordinator->takeLiveChangeResult(offlineResult));
    CHECK(offlineResult.success && offlineResult.userDataChanged);

    // Returning online with the reservation still unclaimed re-arms the gate.
    coordinator->setManualOfflineMode(false);
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // The live worker is gated again until startup claims the sequence.
    JellyfinLibraryChangeBatch onlineBatch;
    onlineBatch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(onlineBatch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Startup claims the sequence; its FullReconcile decision keeps the demand
    // armed across the handoff into the full population Home requests next.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));

    // Completing the sequence releases the retained live change.
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator offline->online re-arms reservation OK\n");
}

// Regression: a competing startup/full-population admission that loses the
// serialized slot must not clear the demand owned by the sequence that already
// claimed it.  Ownership and the armed gate are observed through the
// coordinator's deterministic seams; the owner's retained live change stays
// gated after both competitors fail.
void testCompetingAdmissionDoesNotClobberStartupDemand()
{
    std::printf("[test] LibraryCoordinator competing admission keeps demand\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("competing-admission", db, scope);
    coordinator->start();

    // Pause the db worker so the first startup stays executing and keeps
    // owning the serialized slot while the competitors run.
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->startStartupSync(false));
    db->setWorkerPausedForTest(false);
    CHECK(coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // A retained live change must stay deferred behind the owned demand.
    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Competing startup and full-population admissions both lose the slot.
    CHECK(!coordinator->startStartupSync(false));
    std::uint64_t populationRequest = 0;
    CHECK(!coordinator->requestFullPopulation(populationRequest));

    // Neither competitor may clear the owning sequence's demand/ownership.
    CHECK(coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // The owner completes and the retained live change is finally applied.
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator competing admission keeps demand OK\n");
}

// Regression: after startup publishes a FullReconcile decision, the sequence
// hands off to the full population Home requests next.  During that window the
// demand stays armed but no operation actively claims it, so the owner-held
// handoff token must remain releasable.  If Home is torn down or cancelled
// before requesting the population (modelled here by releaseStartupSequence()
// from the owner), retained live changes must resume rather than starve.
void testStartupHandoffReleaseUnblocksLiveChange()
{
    std::printf("[test] LibraryCoordinator startup handoff release resumes live\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-release", db, scope);
    const auto ownerToken = coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    // Handoff state: the active startup claim ended, but Home still owns the
    // armed demand through the separate handoff token.
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Home abandons the pending population before requesting it.
    coordinator->releaseStartupSequence(ownerToken);
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    // The retained live change resumes instead of starving forever.
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.userDataChanged);
    CHECK(applied.success);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator startup handoff release resumes live OK\n");
}

// Regression: entering manual offline during the startup -> population handoff
// clears the demand so live changes still apply, and leaving offline must
// re-arm it from the still-held handoff token so the population Home requests
// next cannot be preempted by a retained live change.
void testManualOfflineReArmsAcrossStartupHandoff()
{
    std::printf("[test] LibraryCoordinator offline->online re-arms across handoff\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-offline-rearm", db, scope);
    coordinator->reserveStartupSequence();
    coordinator->start();

    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);

    // Offline across the handoff: the demand clears and a live change applies.
    coordinator->setManualOfflineMode(true);
    CHECK(!coordinator->startupPopulationDemandArmedForTest());
    JellyfinLibraryChangeBatch offlineBatch;
    offlineBatch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(offlineBatch));
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult offlineResult;
    CHECK(coordinator->takeLiveChangeResult(offlineResult));
    CHECK(offlineResult.success && offlineResult.userDataChanged);

    // Returning online re-arms the handoff demand from the retained token.
    coordinator->setManualOfflineMode(false);
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // The population Home requests next completes the handoff; draining its
    // terminal publication ends the sequence and clears the demand.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator offline->online re-arms across handoff OK\n");
}

// Regression: the startup -> full-population handoff owner belongs to Home's
// pending population request.  A competing startup/full admission that arrives
// in that window must not steal the handoff token, and if it fails to admit it
// must not clear Home's armed demand.  Otherwise an unrelated failure could
// release the cold-start gate before the population Home is about to request
// has run, letting a retained live change preempt it.  The competing admission
// failure is made deterministic by occupying the serialized slot with an
// unrelated full-sync reservation; no sleeps are involved.
void testCompetingAdmissionDuringHandoffKeepsOwner()
{
    std::printf("[test] LibraryCoordinator competing admission during handoff\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-competing", db, scope);
    coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Drive the sequence to the startup -> population handoff.  Home owns the
    // armed demand but has not yet requested the population.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // Occupy the slot with an unrelated operation so the competing startup
    // admission deterministically fails instead of starting.
    CHECK(coordinator->beginFullSync());

    // The competing startup must neither consume the handoff owner nor release
    // Home's demand when its admission fails.
    CHECK(!coordinator->startStartupSync(false));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Release the unrelated operation.  Home's intended population now consumes
    // the handoff, completes, and releases the retained live change.
    coordinator->finishFullSync();
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupSequenceClaimedForTest());
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator competing admission during handoff OK\n");
}

// Regression (reviewer): during the startup -> population handoff the
// serialized slot is idle, so occupancy alone does not reject a competing
// startup.  The handoff admission gate must reject it explicitly; otherwise it
// is admitted and its terminal path clears Home's handoff and armed demand
// before the intended population runs.  With the slot confirmed idle, the
// competing startup is rejected and publishes nothing, the intended
// FullPopulation still owns and completes the sequence, and the retained live
// change is finally applied.
void testCompetingStartupRejectedWhileHandoffIdle()
{
    std::printf("[test] LibraryCoordinator competing startup rejected at idle handoff\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-idle", db, scope);
    coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Drive the sequence to the startup -> population handoff.  Home owns the
    // armed demand but has not yet requested the population.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    // The serialized slot is genuinely idle here, so the competing startup is
    // rejected by the handoff gate rather than by slot occupancy.
    CHECK(coordinator->serializedOperationIdleForTest());

    // The competing startup must be rejected and must publish no result.
    CHECK(!coordinator->startStartupSync(false));
    CHECK(!coordinator->status().startupInFlight);
    CHECK(!coordinator->status().startupResultReady);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Home's intended population consumes the handoff, completes the sequence,
    // and the retained live change is finally applied.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupSequenceClaimedForTest());
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator competing startup rejected at idle handoff OK\n");
}

// Regression (reviewer): releaseStartupSequence() must be owner-scoped.  During
// the startup -> population handoff no operation actively claims the sequence,
// so a stale/non-owner Home destructor must not clear the intended owner's
// handoff or armed demand.  A newer reservation generation supersedes the older
// token; releasing the older token is inert, while the intended continuation
// (the population Home requests next) still consumes the handoff and completes.
void testStaleStartupReleaseKeepsOwnerHandoff()
{
    std::printf("[test] LibraryCoordinator stale startup release keeps owner handoff\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("stale-release-handoff", db, scope);
    const auto staleToken = coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Drive the sequence to the startup -> population handoff: the active claim
    // ends but the sequence and its demand are still owned.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // A new owner generation arms its own reservation (as a re-initialised
    // AppSession/Home would); the earlier token is now stale.
    const auto ownerToken = coordinator->reserveStartupSequence();
    CHECK(ownerToken != staleToken);
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);

    // A stale/non-owner destructor releasing the old token must not clear the
    // active owner's handoff or demand.
    coordinator->releaseStartupSequence(staleToken);
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // The kNo token from a controller that never reserved is likewise inert.
    coordinator->releaseStartupSequence(library::LibraryCoordinator::kNoStartupSequenceToken);
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // The intended population still consumes the handoff, completes the
    // sequence, and releases the retained live change.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupSequenceClaimedForTest());
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    // Terminal completion invalidates the owner token, so even the intended
    // owner's delayed release is now inert rather than resurrecting the
    // completed generation.
    CHECK(coordinator->startupSequenceOwnerToken() ==
          library::LibraryCoordinator::kNoStartupSequenceToken);
    coordinator->releaseStartupSequence(ownerToken);
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator stale startup release keeps owner handoff OK\n");
}

// Regression: the unclaimed base reservation is owner-scoped too.  A stale
// token from a superseded generation must not release the newer generation's
// reservation/demand, while the matching owner token still does.
void testStaleStartupReleaseKeepsUnclaimedReservation()
{
    std::printf("[test] LibraryCoordinator stale release keeps unclaimed reservation\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("stale-release-unclaimed", db, scope);
    const auto staleToken = coordinator->reserveStartupSequence();
    coordinator->start();

    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // A newer owner generation re-arms the still-unclaimed reservation.
    const auto ownerToken = coordinator->reserveStartupSequence();
    CHECK(ownerToken != staleToken);
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);

    // The stale token cannot release the newer owner's reservation.
    coordinator->releaseStartupSequence(staleToken);
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // The matching owner token releases it and the retained live change applies.
    coordinator->releaseStartupSequence(ownerToken);
    CHECK(coordinator->startupSequenceOwnerToken() ==
          library::LibraryCoordinator::kNoStartupSequenceToken);
    CHECK(!coordinator->startupPopulationDemandArmedForTest());
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator stale release keeps unclaimed reservation OK\n");
}

// Regression (reviewer): a reserved-but-unclaimed cold-start sequence must
// survive a failed startup admission caused by another operation occupying the
// serialized slot.  Claim/ownership must be finalized only after admission
// succeeds, so the failed attempt leaves the owner-held reservation, token, and
// armed demand intact for a retry.  beginFullSync() deterministically occupies
// the slot as an unrelated operation; no sleeps are involved.
void testReservedStartupAdmissionFailureKeepsReservation()
{
    std::printf("[test] LibraryCoordinator reserved startup admission failure\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("reserved-admission-fail", db, scope);
    const auto ownerToken = coordinator->reserveStartupSequence();
    coordinator->start();

    // A retained live change is deferred behind the armed reservation.
    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Occupy the slot with an unrelated operation so startup admission fails.
    CHECK(coordinator->beginFullSync());
    CHECK(!coordinator->startStartupSync(false));

    // The failed admission must not have consumed the reservation: no active
    // claim, the owner token is unchanged, and the demand stays armed.
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Once the unrelated operation releases the slot, the same reservation is
    // admitted and owns the sequence.
    coordinator->finishFullSync();
    CHECK(coordinator->startStartupSync(false));
    CHECK(coordinator->startupSequenceClaimedForTest());
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);

    // The startup -> population handoff completes and releases the retained
    // live change rather than stranding it.
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupPopulationDemandArmedForTest());
    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator reserved startup admission failure OK\n");
}

// Regression (reviewer): during the startup -> full-population handoff the
// intended continuation's FullPopulation admission can still fail because an
// unrelated operation already occupies the serialized slot as the same
// FullPopulation kind (beginFullSync).  Consuming the handoff before admission
// would steal Home's owner token and clear the armed demand on that failure.
// This verifies the handoff, token, and demand survive the failed admission and
// are consumed by the later, admitted population.  No sleeps are involved.
void testHandoffPopulationAdmissionFailureKeepsHandoff()
{
    std::printf("[test] LibraryCoordinator handoff population admission failure\n");
    CoordinatorTestScope scope;
    std::shared_ptr<CatalogDb> db;
    auto coordinator = makeMaintenanceCoordinator("handoff-admission-fail", db, scope);
    const auto ownerToken = coordinator->reserveStartupSequence();
    coordinator->start();

    // Defer a retained live change so the armed demand is observable.
    JellyfinLibraryChangeBatch batch;
    batch.userDataChanged = true;
    CHECK(coordinator->requestLiveChange(batch));
    CHECK(coordinator->waitLiveChangeDeferredForTest(std::chrono::seconds(5)));

    // Drive the sequence to the startup -> population handoff.
    CHECK(coordinator->startStartupSync(false));
    const library::StartupSyncResult startup = takeStartupResult(*coordinator);
    CHECK(startup.mode == library::StartupSyncMode::FullReconcile);
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());

    // An unrelated full-population kind operation occupies the slot, so the
    // intended continuation's admission fails.
    CHECK(coordinator->beginFullSync());
    std::uint64_t request = 0;
    CHECK(!coordinator->requestFullPopulation(request));

    // The failed admission must not consume the handoff: no active claim, the
    // handoff is still pending, the owner token is unchanged, and the demand
    // stays armed.
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupPopulationDemandArmedForTest());
    CHECK(coordinator->startupSequenceOwnerToken() == ownerToken);
    library::LiveLibraryChangeResult deferred;
    CHECK(!coordinator->liveChangeResultReadyForTest());
    CHECK(!coordinator->takeLiveChangeResult(deferred));

    // Once the unrelated operation releases the slot, the intended population
    // consumes the handoff and completes the sequence.
    coordinator->finishFullSync();
    CHECK(coordinator->requestFullPopulation(request));
    CHECK(!coordinator->startupHandoffPendingForTest());
    CHECK(coordinator->startupSequenceClaimedForTest());
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    CHECK(!coordinator->startupSequenceClaimedForTest());
    CHECK(!coordinator->startupPopulationDemandArmedForTest());

    CHECK(coordinator->waitLiveChangeResultForTest(std::chrono::seconds(5)));
    library::LiveLibraryChangeResult applied;
    CHECK(coordinator->takeLiveChangeResult(applied));
    CHECK(applied.success && applied.userDataChanged);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator handoff population admission failure OK\n");
}

void testAdmissionDiagnosticSnapshotSurvivesConcurrentRelease()
{
    std::printf("[test] LibraryCoordinator admission diagnostic snapshot race\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    // Hierarchy owns the serialized slot.  The unconsumed terminal result keeps
    // it held, so no live network peer or sleep is needed.
    MediaItem series;
    series.id = "admission-snapshot-series";
    std::uint64_t hierarchyRequest = 0;
    CHECK(coordinator.requestSeriesSeasons(series, hierarchyRequest));

    // Arm the seam so the next admission parks between taking its operation
    // snapshot and deriving the diagnostic reason.
    coordinator.setAdmissionSnapshotPauseForTest(true);

    bool admitted = true;
    std::thread admitter([&] { admitted = coordinator.startStartupSync(false); });

    // Spin (no sleep) until the admission has snapshotted the occupied slot.
    for (int i = 0; i < 1000000 && !coordinator.admissionSnapshotPausedForTest(); ++i)
        std::this_thread::yield();
    CHECK(coordinator.admissionSnapshotPausedForTest());

    // Release the hierarchy slot while the admission is parked.  The old
    // implementation re-read the slot here and appended a null reason.
    coordinator.releaseCurrentOperationForTest();
    coordinator.setAdmissionSnapshotPauseForTest(false);

    admitter.join();
    CHECK(!admitted);

    coordinator.stop();
    std::printf("[test] LibraryCoordinator admission diagnostic snapshot race OK\n");
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
    testLiveChangeQueueFullDrainFallsBackToCatchUp();
    testLiveChangeCatchUpFailureIsRetriedByCoordinator();
    testLiveChangeCatchUpApplyFailureRetainsBarrier();
    testLiveChangeCatchUpAndApplyPublishExactlyOnce();
    testLiveChangesWaitForSerializedSyncSlots();
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

    // Serialized-operation model coverage: legacy reservation, concurrent
    // reservation accounting, and slot retention across operation kinds.
    testLegacyFullSyncReservationGatesAndReleases();
    testConcurrentLegacyReservationReservesExactlyOnce();
    testUnconsumedStartupResultRetainsSerializedSlot();
    testUnconsumedSafetyResultAdmitsHierarchyAndSurvives();
    testUnconsumedLiveResultAdmitsHierarchy();
    testUnconsumedLiveResultIsNotOverwritten();
    testStartupPopulationPrecedesLiveChangeAtColdStart();
    testStartOnlyCoordinatorProcessesLiveChange();
    testAbandonedStartupReservationReleasesLiveProcessing();
    testManualOfflineReturnReArmsStartupReservation();
    testCompetingAdmissionDoesNotClobberStartupDemand();
    testStartupHandoffReleaseUnblocksLiveChange();
    testManualOfflineReArmsAcrossStartupHandoff();
    testCompetingAdmissionDuringHandoffKeepsOwner();
    testCompetingStartupRejectedWhileHandoffIdle();
    testStaleStartupReleaseKeepsOwnerHandoff();
    testStaleStartupReleaseKeepsUnclaimedReservation();
    testReservedStartupAdmissionFailureKeepsReservation();
    testHandoffPopulationAdmissionFailureKeepsHandoff();

    // Keep the diagnostics capture bounded to the full-population and gate
    // tests below.  The logger has a finite file retention limit; resetting
    // it here avoids unrelated earlier test diagnostics evicting the lines
    // under test, especially when sanitizer tests run in parallel.
    uiDiagnostics().stop();
    std::remove(diagnosticsPath.c_str());
    uiDiagnostics().start(diagnosticsPath);

    testFullPopulationSuccessCommitsCheckpoint();
    testFullPopulationAfterLiveChangeAdvancesGeneration();
    testFullPopulationCancellationAbortsStagedGeneration();
    testFullPopulationFailureAbortsWithoutCheckpoint();

    testFullPopulationRejectsStaleRequest();
    testCoordinatorSerializesStartupFullSafetyAndLive();
    testAdmissionDiagnosticSnapshotSurvivesConcurrentRelease();

    // All producers above have joined.  stop() is the existing logger
    // drain/join barrier, not a timing delay.
    uiDiagnostics().stop();
    const auto diagnostics = miyoofin_test::readTestBytes(diagnosticsPath);

    CHECK(diagnostics.find("[LibraryCoordinator] full_population_rejected phase=admission") !=
          std::string::npos);
    CHECK(diagnostics.find("reasons=startup_in_flight") != std::string::npos);
    CHECK(diagnostics.find("reasons=safety_reconcile_in_flight") != std::string::npos);
    // The admission diagnostic must still name the snapshotted hierarchy slot
    // even though it was released before the reason was derived.
    CHECK(diagnostics.find("reasons=hierarchy_mutation_in_flight") != std::string::npos);
    const auto diagnosticLine = [&](const std::string& marker,
                                    const std::vector<std::string>& fields = {}) {
        std::size_t search = 0;
        while (search < diagnostics.size()) {
            const auto begin = diagnostics.find(marker, search);
            if (begin == std::string::npos)
                return std::string();
            const auto end = diagnostics.find('\n', begin);
            const auto line = diagnostics.substr(begin, end == std::string::npos ? std::string::npos
                                                                                 : end - begin);
            bool matches = true;
            for (const auto& field : fields)
                matches = matches && line.find(field) != std::string::npos;
            if (matches)
                return line;
            if (end == std::string::npos)
                return std::string();
            search = end + 1;
        }
        return std::string();
    };
    const auto scopeReady = diagnosticLine("scope_stage=scope_ready epoch=", {"scope_hash="});
    CHECK(scopeReady.find("scope_hash=") != std::string::npos);
    const auto catalogPage =
        diagnosticLine("page_complete request=",
                       {"success=1", "scope_epoch=", "scope_hash=", "source=full_population"});
    CHECK(catalogPage.find("success=1") != std::string::npos);
    CHECK(catalogPage.find("scope_epoch=") != std::string::npos);
    CHECK(catalogPage.find("scope_hash=") != std::string::npos);
    CHECK(catalogPage.find("source=full_population") != std::string::npos);
    const auto coordinatorStage = diagnosticLine("full_population_stage_complete request=");
    CHECK(coordinatorStage.find("generation=") != std::string::npos);
    CHECK(coordinatorStage.find("scope_epoch=") != std::string::npos);
    CHECK(coordinatorStage.find("scope_hash=") != std::string::npos);
    CHECK(coordinatorStage.find("source=full_population") != std::string::npos);
    const auto coordinatorPublish = diagnosticLine("full_population_publish request=");
    CHECK(coordinatorPublish.find("generation=") != std::string::npos);
    CHECK(coordinatorPublish.find("scope_epoch=") != std::string::npos);
    CHECK(coordinatorPublish.find("scope_hash=") != std::string::npos);
    CHECK(coordinatorPublish.find("source=full_population") != std::string::npos);
    std::remove(diagnosticsPath.c_str());
    return miyoofin_test::finish("library_coordinator");
}
