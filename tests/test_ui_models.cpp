#include "test_support.hpp"
#include "../src/ui/ConnectionMonitor.hpp"
#include "../src/ui/HomeTabs.hpp"
#include "../src/ui/HomeSyncState.hpp"
#include "../src/ui/HomeSettingsModel.hpp"
#include "../src/ui/HomeArtworkPlan.hpp"
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
    testBatteryParsePercent();
    testBatteryIconMath();
    testBatteryMonitorReadsAndThrottles();
    testBatteryParseCharging();
    testBatteryChargingProbe();
    testRailWatchStateReachesMovieGrid();
    testEpisodeFallsBackToSeriesPoster();
    testConnectionMonitorStatesAndHysteresis();
    testConnectionMonitorFirstFailureIsOffline();
    testConnectionMonitorIntervalsPokeAndOfflineMode();
    testConnectionMonitorDestructorCancelsBlockedProbe();
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
