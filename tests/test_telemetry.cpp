#include "test_support.hpp"
#include "cases/test_telemetry.inc"

int main()
{
    testTelemetryClocks(); testTelemetryConfigAndFacade();
    testTelemetryContextAndGuards(); testTelemetryRing(); testMftV1Codec();
    testMftV2CatalogDbTelemetry(); testTelemetrySegmentAttemptPayload();
    testTelemetryPlaybackEventPayload(); testTelemetrySecretLeakRegression();
    testTelemetryWriter(); testTelemetryServiceThread(); testHttpClientTelemetry();
    testTelemetrySchemaTypes();
    return miyoofin_test::finish("telemetry");
}
