#include "test_support.hpp"
#include "cases/test_misc_regressions.inc"

int main()
{
    testRouteRequest(); testMovieOrganizationalTitles();
    testMovieAlphabetOrganization(); testMovieOrganizationalSort();
    testMovieAlphabetFocus(); testBoundedPagePosterPlanning();
    testHomeRailPosterPlanning(); testInitialHomeArtworkScheduling();
    testPersistentHttpClientPatterns();
    testShowsFocusPreservedDuringIncrementalRefresh(); testHomeGridPositionHelpers();
    testRecentlyAddedWarmPublication(); testWarmHomeCatalogWindows();
    testWarmMediaWindowsExtraction(); testAnimeSelectionSurvivesSharedWindowTrim();
    testHomeUsesDedicatedAnimeCatalogPage(); testHomeRefreshPreservesActiveGridSelection();
    testHomeBoundedPagesCanRewind(); testHomeUsesSharedGridRewindEdge();
    testHomeUsesSharedDownIntentAcrossPageLoads(); testShowsPresentation();
    testShouldShowClockErrorTrue(); testShouldShowClockErrorFalseModernEpoch();
    testShouldShowClockErrorFalseUnrelatedFailure(); testClockMessageFormat();
    testLiveChangeCheckpointPolicy();
    testHomeRowFocusReconciliation();
    testCardSurfaceCacheEviction(); testNonBlockingFinishPolicy();
    testRefreshMovieFilterHomeNavPreserved();     testLivePublicationNoopEmptyResult();
    testSqliteTempDirPath();
    testPageTransactionShouldRetry();
    testPageTransactionCleanupInvariants();
    return miyoofin_test::finish("misc-regressions");
}
