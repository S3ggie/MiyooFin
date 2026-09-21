#include "test_support.hpp"
#include "cases/test_misc_regressions.inc"

int main()
{
    testNetworkLayerDoesNotDependOnPresentationModels();
    testSourceMatcherTolerance();
    testRouteRequest(); testMovieOrganizationalTitles();
    testMovieAlphabetOrganization(); testMovieOrganizationalSort();
    testMovieAlphabetFocus(); testBoundedPagePosterPlanning();
    testHomeRailPosterPlanning(); testInitialHomeArtworkScheduling();
    testPersistentHttpClientPatterns();
    testShowsFocusPreservedDuringIncrementalRefresh(); testHomeGridPositionHelpers();
    testRecentlyAddedWarmPublication(); testWarmHomeCatalogWindows();
    testWarmMediaWindowsExtraction(); testAnimeSelectionSurvivesSharedWindowTrim();
    testHomeUsesDedicatedAnimeCatalogPage(); testCoordinatorCommittedGenerationStatus(); testHomeRefreshPreservesActiveGridSelection();
    testHomeBoundedPagesCanRewind(); testHomeUsesSharedGridRewindEdge();
    testHomeUsesSharedDownIntentAcrossPageLoads(); testShowsPresentation();
    testShouldShowClockErrorTrue(); testShouldShowClockErrorFalseModernEpoch();
    testShouldShowClockErrorFalseUnrelatedFailure(); testClockMessageFormat();
    testHomeRowFocusReconciliation();
    testHomeRailRefreshCoordinatorPath();
    testCardSurfaceCacheEviction(); testNonBlockingFinishPolicy();
    testRefreshMovieFilterHomeNavPreserved();     testLivePublicationNoopEmptyResult();
    testSqliteTempDirPath();
    testPageTransactionShouldRetry();
    testDiagnosticPageKindClassification();
    testPageTransactionCleanupInvariants();
    testOfflineModeToggleDrivesFetchPath();
    testOfflineSnapshotSignatureStability();
    testOfflineToggleCancelsFetchAndUsesCache();
    return miyoofin_test::finish("misc-regressions");
}
