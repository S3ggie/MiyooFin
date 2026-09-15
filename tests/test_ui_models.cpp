#include "test_support.hpp"
#include "../src/ui/HomeTabs.hpp"
#include "../src/ui/HomeSyncState.hpp"
#include "../src/ui/HomeSettingsModel.hpp"
#include "../src/ui/HomeArtworkPlan.hpp"
#include "cases/test_ui_models.inc"

int main()
{
    testHomeSyncSchedule(); testShowsSyncProgress(); testHomeSyncStatusStrings();
    testHomeSettingsModel(); testHomeTabsProjection(); testHomeTabRowUpdates();
    testHomeRowIndexByLabel();
    testTransitionTabIndex(); testHomeArtworkPlan();
    testColdStartPopulationProducesNonEmptyTabs();
    testPosterJobScheduling(); testHomeRailRefreshDebounce();
    testCollectBoundedSeriesIds();
    return miyoofin_test::finish("ui-models");
}
