#include "test_support.hpp"
#include "cases/test_ui_models_support.hpp"
#include "cases/test_ui_models.inc"

int main()
{
    testPresentationArtworkAdapter();
    testHomeSyncSchedule();
    testShowsSyncProgress();
    testHomeSyncStatusStrings();
    testHomeSettingsModel();
    testHomeDownloadsStateExpandCollapse();
    testHomeDownloadsStateRemovalAndBulkConfirm();
    testHomeDownloadsStateJournalDiscard();
    testDownloadsEmptyStateNeverHidesAStorageProblem();
    testHomeArtworkCacheLruEviction();
    testHomeArtworkCacheProtectedAndTouch();
    testHomeArtworkCacheCardSurfaces();
    testHomeSettingsStateNavigation();
    testHomeSettingsStateConfirmation();
    testRailWatchStateReachesMovieGrid();
    testEpisodeFallsBackToSeriesPoster();
    testHomeTabsProjection();
    testHomeTabRowUpdates();
    testHomeRowIndexByLabel();
    testTransitionTabIndex();
    testHomeArtworkPlan();
    testColdStartPopulationProducesNonEmptyTabs();
    testPosterJobScheduling();
    testHomeRailRefreshDebounce();
    testCollectBoundedSeriesIds();
    return miyoofin_test::finish("ui-models");
}
