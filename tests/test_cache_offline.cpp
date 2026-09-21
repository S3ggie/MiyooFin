#include "test_support.hpp"
#include "cases/test_cache_offline.inc"

int main()
{
    testLegacyCacheTintBytes(); testLibraryCacheFetchDecision(); testOfflineLibraryProjection();
    testOfflineLibraryQuery(); testOfflineHomeMediaPage(); testSettingsRowActions();
    testManualOfflineProjectionDeferredUntilSnapshotReady();
    testLanServerAddressClassificationAndSettingsLayout();
    testSeriesCachedSeasonHandoff(); testSeasonPosterScheduling();
    testNewGridAndSchedule(); testCacheRemoveNew();
    return miyoofin_test::finish("cache-offline");
}
