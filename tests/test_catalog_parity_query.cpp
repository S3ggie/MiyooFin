#include "test_support.hpp"
#include "../src/library/LibrarySync.hpp"
#include "cases/test_catalog_migration_support.hpp"
#include "cases/test_catalog_parity_support.hpp"
#include <filesystem>
#include <type_traits>
#include "cases/test_catalog_parity_sync.inc"
#include "cases/test_catalog_parity_query.inc"
int main()
{
    testCatalogDbProjectionParity();
    testCatalogDbDownloadFallbackParity();
    testCatalogDbBoundedMediaPaging();
    testCatalogDbMediaPageUpsertAndPopulation();
    testHomeUsesCatalogBeforeNetworkRefresh();
    testLibraryQueryDomainBoundary();
    return miyoofin_test::finish("catalog_parity_query");
}
