#include "test_support.hpp"
#include "../src/download/DownloadSubtitles.hpp"
#include "../src/download/DownloadAudio.hpp"
#include <sys/stat.h>
#include <sys/resource.h>
#include <csignal>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <dirent.h>

static std::string readFixture(const std::string& path)
{
    std::string out;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return out;
    char buffer[128];
    std::size_t bytes = 0;
    while ((bytes = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        out.append(buffer, bytes);
    std::fclose(file);
    return out;
}

#include "cases/test_downloads.inc"

int main()
{
    testDownloadInterruptStates();
    testSubtitleSidecars();
    testDownloadAudioChoice();
    testHlsDownloadStore();
    testHlsSegmentIntegrity();
    testStorageWriteFailureDoesNotLeakDescriptors();
    testPauseResumeDoNotWaitForTheDisk();
    testErasureAndRedownloadDoNotWaitForTheDisk();
    testConfigureDoesNotWaitForTheDisk();
    testRemovalSurvivesAccountSwitch();
    testStaleTransferNeverTouchesAnotherAccount();
    testAccountLibraryLoadIsAsynchronous();
    testDownloadRestartPersistence();
    testDownloadsUiHelpers();
    testDownloadHierarchy();
    testDownloadSourceReconciliation();
    testDownloadPlanBatchAccounting();
    testHlsSizeEstimates();
    testHlsFailureClassification();
    testStartupReconcileSkip();
    testReconcileShouldSkip();
    testReconcilerPreservesLiveState();
    testTransferPreservesLiveState();
    testDownloadPlanFullCardRejection();
    testCatalogDbHierarchyPlanning();
    testPredictedDownloadTotalBytes();
    testSpeedStallGap();
    testHlsSegmentCompletedIncremental();
    testSegmentRecoveryMatchesIncremental();
    testFreeSpaceCachePolicy();
    testTlsCaBundleCache();
    testTransferSourceIdentityGuard();
    testTransferFinishDecision();
    testWorkerPersistMatchesLiveState();
    testStalePlaylistDiscoverySuppressed();
    testPersistTouchesOnlyAffectedItem();
    testPersistPendingCrashDurabilityAndTmpSweep();
    testIndexNeverNamesMissingManifest();
    testPlaybackToggleTouchesOnlyAffectedItems();
    return miyoofin_test::finish("downloads");
}
