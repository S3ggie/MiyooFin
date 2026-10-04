#include "test_support.hpp"
#include "cases/test_downloads_support.hpp"

#include "cases/test_downloads_storage.inc"

int main()
{
    testStorageWriteFailureDoesNotLeakDescriptors();
    testPauseResumeDoNotWaitForTheDisk();
    testErasureAndRedownloadDoNotWaitForTheDisk();
    testConfigureDoesNotWaitForTheDisk();
    testRemovalSurvivesAccountSwitch();
    testStaleTransferNeverTouchesAnotherAccount();
    testAccountLibraryLoadIsAsynchronous();
    return miyoofin_test::finish("downloads_storage");
}
