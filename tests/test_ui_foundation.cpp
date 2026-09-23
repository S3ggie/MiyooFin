#include "test_support.hpp"
#include "cases/test_ui_foundation.inc"

int main()
{
    testServerEntryKeyboardCaps();
    testSettingsAddressEntryCancel();
    testLoginKeyboardCaps();
    testOnScreenKeyboardGrid();
    testOnScreenKeyboardSpace();
    testServerEntrySpace();
    testLoginUsernameSpace();
    testLoginPasswordSpace();
    testSharedKeyboardLayoutConsistency();
    testKeyboardVerticalNavActionRow();
    testUiDiagnostics();
    testLoginBackRequestsServerEntry();
    testLoginKeyboardBackDeletes();
    testThreadRestartGuards();
    testConnectScreenWorkerLifecycle();
    testNoDetachedProductionThreads();

    return miyoofin_test::finish("ui-foundation");
}
