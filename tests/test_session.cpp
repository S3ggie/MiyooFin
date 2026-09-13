#include "test_support.hpp"
#include "cases/test_session.inc"
int main(){testSession();testSessionBackwardCompatibility();testLocalServerIdentityVerification();testSystemInfoParsing();testSessionEmpty();testSessionAtomicNoTmpResidue();testSessionAtomicReplacePreservesNewContent();testDeviceIdentity();testDeviceIdentityLoadOrCreate();testAuthTypes();return miyoofin_test::finish("session");}
