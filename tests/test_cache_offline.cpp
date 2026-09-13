#include "test_support.hpp"
#include "cases/test_cache_offline.inc"

int main()
{
    testLibraryCacheNew(); testLibraryCacheV3SaveLoad();
    testLibraryCacheV2BackCompat(); testLibraryCacheV1BackCompat();
    testLibraryCacheUnknownVersion(); testLibraryCacheFetchDecision();
    testSyncState(); testOfflineCatalog(); testOfflineLibraryProjection();
    testOfflineLibraryQuery(); testOfflineHomeMediaPage(); testSettingsRowActions();
    testManualOfflineProjectionDeferredUntilSnapshotReady();
    testLanServerAddressClassificationAndSettingsLayout();
    testSeriesCachedSeasonHandoff(); testSeasonPosterScheduling();
    testNewGridAndSchedule(); testCacheRemoveNew();
    return miyoofin_test::finish("cache-offline");
}
