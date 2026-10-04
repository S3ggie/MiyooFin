#include "test_support.hpp"
#include "cases/test_downloads_support.hpp"

#include "cases/test_downloads_transfer.inc"

int main()
{
    testStartupReconcileSkip();
    testReconcileShouldSkip();
    testReconcilerPreservesLiveState();
    testTransferPreservesLiveState();
    testTransferSourceIdentityGuard();
    testTransferFinishDecision();
    testWorkerPersistMatchesLiveState();
    testStalePlaylistDiscoverySuppressed();
    testPersistTouchesOnlyAffectedItem();
    testPersistPendingCrashDurabilityAndTmpSweep();
    testIndexNeverNamesMissingManifest();
    testPlaybackToggleTouchesOnlyAffectedItems();
    return miyoofin_test::finish("downloads_transfer");
}
