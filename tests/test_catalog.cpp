#include "test_support.hpp"
#include "cases/test_catalog_core.inc"
#include "cases/test_catalog_migration.inc"
#include "cases/test_catalog_parity_support.hpp"
#include "cases/test_catalog_parity_hierarchy.inc"
#include "cases/test_catalog_parity_sync.inc"
#include "cases/test_catalog_parity_query.inc"

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
    testHomeDiscardsColdProvisionalFailure();
    testHomePreservesAnimeMembershipDuringBoundedReads();
    testHomeUsesCatalogBeforeNetworkRefresh(); testLibraryCacheHomeParityHarness();
    testCatalogDbDownloadFallbackParity();
    testCatalogDbDuplicateMergeAndAuthoritativeDeletionParity();
    testLegacyWholeFilePersistenceRetired();
    return miyoofin_test::finish("catalog");
}
