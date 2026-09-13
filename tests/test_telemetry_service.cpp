#include "test_support.hpp"
#include "cases/test_telemetry_service.inc"
int main(){ testTelemetryWriter(); testTelemetryServiceThread(); testHttpClientTelemetry(); return miyoofin_test::finish("telemetry_service"); }

