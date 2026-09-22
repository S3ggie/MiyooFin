#include "test_support.hpp"
#include "cases/test_api_events.inc"
int main()
{
    testLibraryItemsBoundedPageHttp();
    testChangedCatalogMetadataQuery();
    testLibraryChangedEventParsing();
    return miyoofin_test::finish("api_events");
}
