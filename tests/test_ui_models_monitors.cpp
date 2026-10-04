#include "test_support.hpp"
#include "cases/test_ui_models_support.hpp"
#include "cases/test_ui_models_monitors.inc"

int main()
{
    testBatteryParsePercent();
    testBatteryIconMath();
    testBatteryMonitorReadsAndThrottles();
    testBatteryParseCharging();
    testBatteryChargingProbe();
    testConnectionMonitorStatesAndHysteresis();
    testConnectionMonitorFirstFailureIsOffline();
    testConnectionMonitorIntervalsPokeAndOfflineMode();
    testConnectionMonitorDestructorCancelsBlockedProbe();
    return miyoofin_test::finish("ui-models-monitors");
}
