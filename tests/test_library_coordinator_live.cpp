#include "test_support.hpp"
#include "cases/test_library_coordinator_support.hpp"

namespace {
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
    CHECK(db->waitForIdleForTest(std::chrono::seconds(10)));
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
    CHECK(db->waitForIdleForTest(std::chrono::seconds(10)));
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
    CHECK(db->waitForIdleForTest(std::chrono::seconds(10)));
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
} // namespace

int main()
{
    const std::string diagnosticsPath = "/tmp/miyoofin-library-coordinator-diagnostics-" +
                                        std::to_string(static_cast<long long>(::getpid())) + ".log";
    std::remove(diagnosticsPath.c_str());
    uiDiagnostics().start(diagnosticsPath);

    testLiveChangeQueueFullDrainFallsBackToCatchUp();
    testLiveChangeCatchUpFailureIsRetriedByCoordinator();
    testLiveChangeCatchUpApplyFailureRetainsBarrier();
    testLiveChangeCatchUpAndApplyPublishExactlyOnce();
    testLiveChangesWaitForSerializedSyncSlots();
    testStartOnlyCoordinatorProcessesLiveChange();
    // All producers above have joined.  stop() is the existing logger
    // drain/join barrier, not a timing delay.
    uiDiagnostics().stop();
    std::remove(diagnosticsPath.c_str());
    return miyoofin_test::finish("library_coordinator_live");
}
