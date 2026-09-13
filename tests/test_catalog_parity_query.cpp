#include "test_support.hpp"
#include "cases/test_catalog_migration_support.hpp"
#include "cases/test_catalog_parity_support.hpp"
#include "cases/test_catalog_parity_sync.inc"
#include "cases/test_catalog_parity_query.inc"
int main(){testCatalogDbProjectionParity();testCatalogDbDownloadFallbackParity();testCatalogDbBoundedMediaPaging();testLegacyWholeFilePersistenceRetired();testCatalogDbMediaPageUpsertAndPopulation();testHomeUsesCatalogBeforeNetworkRefresh();return miyoofin_test::finish("catalog_parity_query");}

