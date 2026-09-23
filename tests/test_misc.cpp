#include "test_support.hpp"
#include "cases/test_misc_regressions.inc"

int main()
{
    testNetworkLayerDoesNotDependOnPresentationModels();
    testSourceMatcherTolerance();
    testRouteRequest();
    testMovieOrganizationalTitles();
    testMovieAlphabetOrganization();
    testMovieOrganizationalSort();
    testMovieAlphabetFocus();
    testBoundedPagePosterPlanning();
    testHomeRailPosterPlanning();
    testShowsFocusPreservedDuringIncrementalRefresh();
    testHomeGridPositionHelpers();
    testRecentlyAddedWarmPublication();
    testWarmMediaWindowsExtraction();
    testAnimeSelectionSurvivesSharedWindowTrim();
    testShowsPresentation();
    testShouldShowClockErrorTrue();
    testShouldShowClockErrorFalseModernEpoch();
    testShouldShowClockErrorFalseUnrelatedFailure();
    testClockMessageFormat();
    testHomeRowFocusReconciliation();
    testNonBlockingFinishPolicy();
    testLivePublicationNoopEmptyResult();
    testSqliteTempDirPath();
    testPageTransactionShouldRetry();
    testOfflineSnapshotSignatureStability();
    return miyoofin_test::finish("misc-regressions");
}
