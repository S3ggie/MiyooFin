#include "test_support.hpp"
#include "cases/test_telemetry_core.inc"
int main()
{
    testTelemetryClocks();
    testTelemetryConfigAndFacade();
    testTelemetryContextAndGuards();
    testTelemetryRing();
    return miyoofin_test::finish("telemetry");
}
