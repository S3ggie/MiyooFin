#include "test_support.hpp"
#include "cases/test_downloads_support.hpp"

#include "cases/test_downloads.inc"

int main()
{
    testDownloadInterruptStates();
    testSubtitleSidecars();
    testDownloadAudioChoice();
    testHlsDownloadStore();
    testHlsSegmentIntegrity();
    testDownloadRestartPersistence();
    testDownloadsUiHelpers();
    testDownloadHierarchy();
    testDownloadSourceReconciliation();
    testDownloadPlanBatchAccounting();
    testHlsSizeEstimates();
    testHlsFailureClassification();
    testDownloadPlanFullCardRejection();
    testCatalogDbHierarchyPlanning();
    testPredictedDownloadTotalBytes();
    testSpeedStallGap();
    testHlsSegmentCompletedIncremental();
    testSegmentRecoveryMatchesIncremental();
    testFreeSpaceCachePolicy();
    testTlsCaBundleCache();
    return miyoofin_test::finish("downloads");
}
