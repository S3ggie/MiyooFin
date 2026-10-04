#include "test_support.hpp"
#include "cases/test_downloads_support.hpp"

#include "cases/test_downloads_recovery.inc"

int main()
{
    testFailedStorageWritesStayOwedAndRetry();
    testUnavailableStorageIsNeverAnEmptyLibrary();
    testEstablishedStorageIsNeverMistakenForAFirstRun();
    testStorageReturningBeforeTheFirstIndexWriteIsMerged();
    testStorageReturningInTheIndexCreateGapIsMerged();
    testLateScanStaysOwedWhileStorageIsGone();
    testMismatchedStorageIsNeverMutated();
    return miyoofin_test::finish("downloads_recovery");
}
