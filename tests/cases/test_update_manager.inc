// -------------------------------------------------------------------
// tests/cases/test_update_manager.inc — UpdateManager pure logic tests
// -------------------------------------------------------------------
#include "../../src/update/UpdateManager.hpp"
#include "../../src/update/UpdateManifest.hpp"
#include "../../src/update/UpdateVersion.hpp"

using namespace miyoofin;

// -------------------------------------------------------------------
// testUpdateStatusText — verify display strings for all stages
// -------------------------------------------------------------------
static void testUpdateStatusText()
{
    std::printf("[test] Update status text\n");
    {
        UpdateSnapshot snap;
        CHECK_EQ(updateStatusText(snap), "Check for updates");
    }
    {
        UpdateSnapshot snap;
        snap.stage = UpdateStage::Checking;
        CHECK_EQ(updateStatusText(snap), "Checking...");
    }
    {
        UpdateSnapshot snap;
        snap.stage = UpdateStage::UpToDate;
        snap.availableVersion = "0.2.0";
        CHECK_EQ(updateStatusText(snap), "Up to date (v0.2.0)");
    }
    {
        UpdateSnapshot snap;
        snap.stage = UpdateStage::Available;
        snap.availableVersion = "0.3.0";
        CHECK_EQ(updateStatusText(snap), "v0.3.0 available - A to install");
    }
    {
        UpdateSnapshot snap;
        snap.stage = UpdateStage::Downloading;
        snap.percent = 42;
        CHECK_EQ(updateStatusText(snap), "Downloading 42%");
    }
    {
        UpdateSnapshot snap;
        snap.stage = UpdateStage::Verifying;
        CHECK_EQ(updateStatusText(snap), "Verifying...");
    }
    {
        UpdateSnapshot snap;
        snap.stage = UpdateStage::Installing;
        CHECK_EQ(updateStatusText(snap), "Installing... do not power off");
    }
    {
        UpdateSnapshot snap;
        snap.stage = UpdateStage::ReadyToRestart;
        snap.availableVersion = "0.3.0";
        CHECK_EQ(updateStatusText(snap), "Updated to v0.3.0 - A to exit");
    }
    {
        UpdateSnapshot snap;
        snap.stage = UpdateStage::Error;
        snap.error = "network error";
        CHECK_EQ(updateStatusText(snap), "Update failed: network error - A to retry");
    }
    std::printf("[test] Update status text OK\n");
}

// -------------------------------------------------------------------
// testUpdateManagerInitialState — disabled manager lifecycle
// -------------------------------------------------------------------
static void testUpdateManagerInitialState()
{
    std::printf("[test] Update manager initial state\n");
    {
        UpdateManager mgr;
        CHECK(!mgr.enabled());
        CHECK(!mgr.busy());
        CHECK(!mgr.pollDone());

        auto snap = mgr.snapshot();
        CHECK(snap.stage == UpdateStage::Idle);
        CHECK(snap.percent == 0);
        CHECK(snap.error.empty());
        CHECK(snap.availableVersion.empty());
        CHECK(snap.bytesReceived == 0);
        CHECK(snap.bytesTotal == 0);
    }
    std::printf("[test] Update manager initial state OK\n");
}

// -------------------------------------------------------------------
// testUpdateManagerEnabled — enabled manager (no network) lifecycle
// -------------------------------------------------------------------
static void testUpdateManagerEnabled()
{
    std::printf("[test] Update manager enabled lifecycle\n");
    {
        UpdateManager mgr("/tmp/miyoofin-test-update");
        CHECK(mgr.enabled());
        CHECK(!mgr.busy());
        CHECK(!mgr.pollDone());

        // checkForUpdates will fail fast (no network) but should complete.
        mgr.checkForUpdates();
        // Give the worker time to finish (network timeout can be slow).
        for (int i = 0; i < 200 && !mgr.pollDone(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

        CHECK(!mgr.busy());
        auto snap = mgr.snapshot();
        // Should end in Error (network unreachable), UpToDate, or Available
        // depending on network availability and manifest state.
        bool validEndState =
            snap.stage == UpdateStage::Error ||
            snap.stage == UpdateStage::UpToDate ||
            snap.stage == UpdateStage::Available;
        CHECK(validEndState);
    }
    std::printf("[test] Update manager enabled lifecycle OK\n");
}

// -------------------------------------------------------------------
// testUpdateVersionDecision — version comparison helpers
// -------------------------------------------------------------------
static void testUpdateVersionDecision()
{
    std::printf("[test] Update version decision helpers\n");
    // isNewerThan is already tested in test_update.inc; verify a few
    // edge cases relevant to the OTA flow.
    CHECK(isNewerThan("0.2.0", "0.1.0"));
    CHECK(isNewerThan("1.0.0", "0.9.9"));
    CHECK(!isNewerThan("0.1.0", "0.1.0"));
    CHECK(!isNewerThan("0.1.0", "0.2.0"));
    CHECK(isNewerThan("0.2.0-beta.1", "0.1.0"));
    CHECK(!isNewerThan("garbage", "0.1.0"));
    CHECK(!isNewerThan("0.2.0", "garbage"));
    std::printf("[test] Update version decision helpers OK\n");
}
