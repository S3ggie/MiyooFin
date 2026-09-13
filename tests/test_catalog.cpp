#include "test_support.hpp"
#include "cases/test_catalog_core.inc"
#include "cases/test_catalog_migration.inc"
#include "cases/test_catalog_parity.inc"

int main()
{
    testRemoteExitSignal(); testDisplaySizingFallback(); testCatalogDbLifecycle();
    testCatalogDbQueue(); testCatalogDbPriorityOrdering();
    testCatalogDbCancellationAndGeneration(); testCatalogDbShutdownWithFullQueue();
    testCatalogDbScopeLifecycle(); testCatalogDbInvalidScope();
    testCatalogDbSqliteOwnership(); testCatalogDbSchemaV1();
    testCatalogDbSchemaOpenPolicy(); testCatalogDbMediaItemCodec();
    testCatalogDbMediaItemCollections(); testCatalogDbHierarchyQueries();
    testCatalogDbAtomicHierarchyWrite(); testCatalogDbAuthoritativeReconcile();
    testCatalogDbFreshBootstrapPathStates();
    testCatalogDbFreshAuthoritativeLibraryGeneration();
    testCatalogDbFreshBootstrapUnsupportedFinalPreserved();
    testCatalogDbFreshBootstrapPathError(); testCatalogDbJellyfinHierarchyStaging();
    testHomeCatalogHierarchyIntegration(); testCatalogDbOfflineDownloadReconstruction();
    testCatalogDbOfflineRebuildAfterScopeActivation(); testCatalogDbProjectionParity();
    testCatalogDbLibrarySnapshotSeed(); testCatalogDbTopLevelSyncStagingLifecycle();
    testLibrarySyncTeardownDuringStagedGeneration(); testCatalogDbBoundedMediaPaging();
    testCatalogDbMediaPagePreservesViewMembership();
    testCatalogDbMediaPageUpsertAndPopulation(); testCatalogDbMediaPageTopLevelStaging();
    testHomeSkipsUnsupportedLibraryViews();
    testHomeOptionalRailFailuresDoNotStopCatalogPopulation();
    testCatalogScopeConfiguredBeforeHomePopulation();
    testHomePublishesAfterFirstBoundedPage();
    testHomePreservesAnimeMembershipDuringBoundedReads();
    testHomeUsesCatalogBeforeNetworkRefresh(); testLibraryCacheHomeParityHarness();
    testCatalogDbDownloadFallbackParity();
    testCatalogDbDuplicateMergeAndAuthoritativeDeletionParity();
    return miyoofin_test::finish("catalog");
}
