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

} // namespace

int main()
{
    testLibraryCoordinatorIsTheSingleStartupDriver();
    testStopRacingStartupIsSafe();
    return miyoofin_test::finish("library_coordinator");
}
