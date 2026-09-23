#include "test_support.hpp"
#include "../src/library/LibrarySync.hpp"
#include "cases/test_catalog_migration_support.hpp"
#include "cases/test_catalog_parity_support.hpp"
#include "cases/test_catalog_parity_hierarchy.inc"
int main()
{
    testCatalogDbMediaPagePreservesViewMembership();
    testHomeDefersViewPersistenceToCoordinator();
    testHomeOptionalRailFailuresDoNotStopCatalogPopulation();
    testCatalogScopeConfiguredBeforeHomePopulation();
    testHomeFetchOwnershipGuard();
    testHomeDiscardsColdProvisionalFailure();
    testHomeKeepsRailOnlyColdStartLoading();
    testHomePendingCompletionPublicationOrdering();
    testHomeTabNavigation();
    return miyoofin_test::finish("catalog_parity_hierarchy");
}
