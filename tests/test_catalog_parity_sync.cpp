#include "test_support.hpp"
#include "../src/library/LibrarySync.hpp"
#include "cases/test_catalog_migration_support.hpp"
#include "cases/test_catalog_parity_support.hpp"
#include "cases/test_catalog_parity_sync.inc"
int main(){testLibrarySyncAppliesLiveCatalogChanges();testCatalogDbDuplicateMergeAndAuthoritativeDeletionParity();testCatalogDbLibrarySnapshotSeed();testCatalogDbTopLevelSyncStagingLifecycle();testLibrarySyncTeardownDuringStagedGeneration();testLibrarySyncChangedMetadataCatchUp();testLibrarySyncAuthoritativeMembershipReconcile();testCatalogDbMediaPageTopLevelStaging();testLibrarySyncAuthoritativeReconcileCoalescing();return miyoofin_test::finish("catalog_parity_sync");}
