#include "test_support.hpp"
#include "../src/app/RemoteControl.hpp"
#include "../src/library/LibrarySync.hpp"
#include "../src/catalog/CatalogCompatibility.hpp"
#include <filesystem>
#include "cases/test_catalog_core.inc"
#include "cases/test_catalog_migration.inc"
#include "cases/test_catalog_parity_support.hpp"

int main()
{
    testRemoteExitSignal();
    testRemoteControlParsing();
    testDisplaySizingFallback();
    testCatalogDbLifecycle();
    testCatalogDbQueue();
    testCatalogDbPriorityOrdering();
    testCatalogDbCancellationAndGeneration();
    testCatalogDbShutdownWithFullQueue();
    testCatalogDbShutdownDrainsPendingPromises();
    testCatalogDbScopeLifecycle();
    testCatalogDbInvalidScope();
    testCatalogDbSqliteOwnership();
    testCatalogDbSchemaV1();
    testCatalogDbSchemaOpenPolicy();
    testCatalogDbMediaItemCodec();
    testCatalogDbMediaItemCollections();
    testCatalogDbHierarchyQueries();
    testCatalogDbAtomicHierarchyWrite();
    testCatalogDbAuthoritativeReconcile();
    testCatalogDbFreshBootstrapPathStates();
    testCatalogDbFreshAuthoritativeLibraryGeneration();
    testCatalogDbFreshBootstrapUnsupportedFinalPreserved();
    testCatalogDbFreshBootstrapPathError();
    testCatalogDbJellyfinHierarchyStaging();
    testHomeCatalogHierarchyIntegration();
    testCatalogDbOfflineDownloadReconstruction();
    testCatalogDbOfflineRebuildAfterScopeActivation();
    return miyoofin_test::finish("catalog");
}
