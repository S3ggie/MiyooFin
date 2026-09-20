#include "test_support.hpp"
#include "../src/library/LibraryCoordinator.hpp"
#include <atomic>
#include <chrono>
#include <thread>

using namespace miyoofin;

namespace {

library::LibraryCoordinator makeCoordinator()
{
    Session session;
    session.manualOfflineMode = true;
    return library::LibraryCoordinator(
        session, std::make_shared<CatalogDb>(), 0);
}

void testLibraryCoordinatorIsTheSingleStartupDriver()
{
    std::printf("[test] LibraryCoordinator single startup driver\n");
    auto coordinator = makeCoordinator();
    coordinator.start();
    CHECK(coordinator.running());

    // The first caller reserves the only startup slot; a second Home startup
    // request cannot create a competing operation.
    CHECK(coordinator.startStartupSync(false));
    CHECK(!coordinator.startStartupSync(true));
    library::StartupSyncResult result;
    for (int i = 0; i < 200 && !coordinator.takeStartupSyncResult(result); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);

    auto status = coordinator.status();
    CHECK(!status.startupInFlight && !status.cancelRequested);

    // Cancellation and stop are both intentionally safe to repeat.
    coordinator.cancelStartupSync();
    coordinator.cancelStartupSync();
    coordinator.stop();
    coordinator.stop();
    CHECK(coordinator.stopped() && !coordinator.running());
    CHECK(!coordinator.takeStartupSyncResult(result));

    // With startup disabled there is no worker/result thread to join. Keep a
    // source-level assertion on the enabled seam's lock-safe join ordering so
    // this test still guards the deadlock regression before that path opens.
    const auto source = miyoofin_test::readTestBytes(
        "src/library/LibraryCoordinator.cpp");
    CHECK(miyoofin_test::sourceContains(
        source, "kCoordinatorStartupSyncEnabled = true"));
    CHECK(miyoofin_test::sourceContains(
        source, "if (priorThread.joinable()) priorThread.join();"));
    CHECK(miyoofin_test::sourceContains(
        source, "if (startupThread.joinable()) startupThread.join();"));
    const auto priorMove = miyoofin_test::sourcePos(
        source, "priorThread = std::move(m_startupThread)");
    const auto priorJoin = miyoofin_test::sourcePos(
        source, "priorThread.join()");
    const auto startupLockReacquire = miyoofin_test::sourcePos(
        source, "const auto cancellation = m_startupCancellation");
    CHECK(priorMove < priorJoin && priorJoin < startupLockReacquire);
    const auto homeSync = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreenSync.cpp");
    CHECK(miyoofin_test::sourceContains(
        homeSync, "startStartupSync(initialPagePublished)"));
    CHECK(miyoofin_test::sourceContains(
        homeSync, "takeStartupSyncResult"));
    CHECK(!miyoofin_test::sourceContains(
        homeSync, "decideHomeStartupSync("));
    const auto startupGuard = miyoofin_test::sourcePos(
        homeSync, "m_initialPopulationInProgress = true");
    const auto coordinatorStart = miyoofin_test::sourcePos(
        homeSync, "startStartupSync(initialPagePublished)");
    CHECK(startupGuard != std::string::npos);
    CHECK(coordinatorStart != std::string::npos);
    CHECK(startupGuard < coordinatorStart);
    std::printf("[test] LibraryCoordinator single startup driver OK\n");
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
    std::printf("[test] LibraryCoordinator live-change seam\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    CHECK(coordinator.startStartupSync(false));
    JellyfinLibraryChangeBatch batch;
    batch.itemsUpdated.push_back("item-1");
    CHECK(coordinator.requestLiveChange(batch));
    library::LiveChangeIdentity identity;
    CHECK(!coordinator.takeLiveChangeRequest(batch, identity));

    library::StartupSyncResult startupResult;
    for (int i = 0; i < 200 && !coordinator.takeStartupSyncResult(startupResult);
         ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(coordinator.takeLiveChangeRequest(batch, identity));
    CHECK(batch.itemsUpdated.size() == 1);

    // A live result keeps the serialized slot until Home consumes it. A
    // queued event must survive both top-level sync rejections unchanged.
    JellyfinLibraryChangeBatch queuedDuringLive;
    queuedDuringLive.itemsUpdated.push_back("queued-item");
    CHECK(coordinator.requestLiveChange(queuedDuringLive));
    library::LiveChangeIdentity queuedIdentity;
    library::LiveLibraryChangeResult liveResult;
    liveResult.success = true;
    CHECK(coordinator.publishLiveChangeResult(identity, liveResult));
    CHECK(!coordinator.startStartupSync(false));
    CHECK(!coordinator.beginFullSync());
    CHECK(!coordinator.takeLiveChangeRequest(queuedDuringLive,
                                             queuedIdentity));
    CHECK(coordinator.takeLiveChangeResult(identity, liveResult));
    CHECK(coordinator.takeLiveChangeRequest(queuedDuringLive, queuedIdentity));
    CHECK(queuedDuringLive.itemsUpdated.size() == 1);
    liveResult.success = true;
    CHECK(coordinator.publishLiveChangeResult(queuedIdentity, liveResult));
    CHECK(coordinator.takeLiveChangeResult(queuedIdentity, liveResult));

    CHECK(coordinator.beginFullSync());
    CHECK(coordinator.status().inFlight);
    CHECK(coordinator.requestLiveChange(batch));
    CHECK(!coordinator.takeLiveChangeRequest(batch, identity));
    coordinator.finishFullSync();
    CHECK(coordinator.takeLiveChangeRequest(batch, identity));
    CHECK(batch.itemsUpdated.size() == 1);

    // A queued batch must survive while Home's previous live worker still
    // owns the serialized consumer slot.
    JellyfinLibraryChangeBatch nextBatch;
    nextBatch.itemsUpdated.push_back("item-2");
    CHECK(coordinator.requestLiveChange(nextBatch));
    library::LiveChangeIdentity nextIdentity;
    CHECK(!coordinator.takeLiveChangeRequest(nextBatch, nextIdentity));

    liveResult.success = true;
    library::LiveChangeIdentity stale = identity;
    ++stale.request;
    CHECK(!coordinator.publishLiveChangeResult(stale, liveResult));
    CHECK(coordinator.publishLiveChangeResult(identity, std::move(liveResult)));
    CHECK(coordinator.takeLiveChangeResult(identity, liveResult));
    CHECK(liveResult.success);
    CHECK(coordinator.takeLiveChangeRequest(nextBatch, nextIdentity));
    CHECK(nextBatch.itemsUpdated.size() == 1);
    coordinator.discardLiveChangeResults();
    CHECK(!coordinator.publishLiveChangeResult(identity, std::move(liveResult)));
    coordinator.stop();

    // A rejected publication is the completion path for Home's worker when
    // stop/discard clears the active identity; keep the Home-side signal in
    // place so this regression cannot become a silent wait again.
    const auto homeSync = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreenSync.cpp");
    const auto rejectedPublish = miyoofin_test::sourcePos(
        homeSync, "publishLiveChangeResult(");
    const auto doneSignal = miyoofin_test::sourcePos(
        homeSync, "m_liveChangeDone.store(true);");
    CHECK(rejectedPublish != std::string::npos);
    CHECK(doneSignal != std::string::npos);
    std::printf("[test] LibraryCoordinator live-change seam OK\n");
}

void testSafetyReconcilePublishesCoordinatorResult()
{
    std::printf("[test] LibraryCoordinator safety reconcile result\n");
    Session session;
    auto coordinator = library::LibraryCoordinator(
        session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    CHECK(coordinator.requestSafetyReconcile());
    CHECK(!coordinator.requestSafetyReconcile());
    library::SafetyReconcileResult result;
    for (int i = 0; i < 200 && !coordinator.takeSafetyReconcileResult(result);
         ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);
    CHECK(!coordinator.status().safetyReconcileInFlight);

    const auto homeHeader = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreen.hpp");
    const auto homeSync = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreenSync.cpp");
    const auto homeApply = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(miyoofin_test::sourceContains(
        homeSync, "requestSafetyReconcile()"));
    CHECK(miyoofin_test::sourceContains(
        homeApply, "takeSafetyReconcileResult(result)"));
    CHECK(!miyoofin_test::sourceContains(
        homeHeader, "m_safetyReconcileThread"));
    CHECK(!miyoofin_test::sourceContains(
        homeHeader, "m_safetyReconcileCancellation"));
    CHECK(!miyoofin_test::sourceContains(
        homeHeader, "m_safetyReconcileDone"));
    std::printf("[test] LibraryCoordinator safety reconcile result OK\n");
}

void testSafetyReconcileStopPublishesCompletion()
{
    std::printf("[test] LibraryCoordinator safety reconcile stop\n");
    Session session;
    auto coordinator = library::LibraryCoordinator(
        session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    CHECK(coordinator.requestSafetyReconcile());
    coordinator.stop();

    library::SafetyReconcileResult result;
    CHECK(coordinator.takeSafetyReconcileResult(result));
    CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);
    coordinator.stop();
    std::printf("[test] LibraryCoordinator safety reconcile stop OK\n");
}

void testHomeRailStopPublishesCompletion()
{
    std::printf("[test] LibraryCoordinator Home rail lifecycle\n");
    Session session;
    session.serverUrl = "http://127.0.0.1:1";
    auto coordinator = library::LibraryCoordinator(
        session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    std::uint64_t request = 0;
    CHECK(coordinator.requestHomeRailRefresh(request));
    coordinator.stop();

    // stop() may race the transport worker.  Either way, the worker must
    // publish a terminal (possibly cancelled) result for Home's wait loop.
    library::HomeRailResult result;
    CHECK(coordinator.takeHomeRailResult(request, result));
    CHECK(result.request == request);

    const auto source = miyoofin_test::readTestBytes(
        "src/library/LibraryCoordinator.cpp");
    const auto publish = miyoofin_test::sourcePos(
        source, "if (requestId == m_homeRailRequest)");
    CHECK(publish != std::string::npos);
    CHECK(miyoofin_test::sourceContains(
        source, "m_homeRailResultReady = true;"));
    std::printf("[test] LibraryCoordinator Home rail lifecycle OK\n");
}

} // namespace

int main()
{
    testLibraryCoordinatorIsTheSingleStartupDriver();
    testStopRacingStartupIsSafe();
    testLiveChangesWaitForSerializedSyncSlots();
    testSafetyReconcilePublishesCoordinatorResult();
    testSafetyReconcileStopPublishesCompletion();
    testHomeRailStopPublishesCompletion();
    return miyoofin_test::finish("library_coordinator");
}
