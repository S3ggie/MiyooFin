#include "test_support.hpp"
#include "../src/net/RouteRequest.hpp"
#include "cases/test_session.inc"
int main()
{
    testSession();
    testSessionBackwardCompatibility();
    testLocalServerIdentityVerification();
    testSessionRoutes();
    testSystemInfoParsing();
    testSessionEmpty();
    testSessionAtomicNoTmpResidue();
    testSessionAtomicReplacePreservesNewContent();
    testDeviceIdentity();
    testDeviceIdentityLoadOrCreate();
    testDeviceIdentityFallbackEntropy();
    testAuthTypes();
    return miyoofin_test::finish("session");
}
