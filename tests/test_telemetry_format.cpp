#include "test_support.hpp"
#include "cases/test_telemetry_format.inc"
int main(){ testMftV1Codec(); testMftV2CatalogDbTelemetry(); testTelemetrySegmentAttemptPayload(); testTelemetryPlaybackEventPayload(); testTelemetrySecretLeakRegression(); return miyoofin_test::finish("telemetry_format"); }

