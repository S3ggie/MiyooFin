#include "test_support.hpp"
#include "../src/library/LibrarySync.hpp"
#include "cases/test_catalog_migration_support.hpp"
#include "cases/test_catalog_parity_support.hpp"
#include "cases/test_catalog_parity_hierarchy.inc"
int main()
{
    testCatalogDbMediaPagePreservesViewMembership();
    testHomeSkipsUnsupportedLibraryViews();
    testHomeOptionalRailFailuresDoNotStopCatalogPopulation();
    testCatalogScopeConfiguredBeforeHomePopulation();
    testHomePublishesAfterFirstBoundedPage();
    testHomeDiscardsColdProvisionalFailure();
    testHomeDoesNotPublishRailOnlyPresentation();
    testHomePendingCompletionPublicationOrdering();
    testHomePreservesAnimeMembershipDuringBoundedReads();
    return miyoofin_test::finish("catalog_parity_hierarchy");
}
