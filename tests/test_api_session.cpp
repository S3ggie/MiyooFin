#include "test_support.hpp"
#include "cases/test_session.inc"
#include "cases/test_api_core.inc"
#include "cases/test_api_events.inc"
int main(){testNormaliseUrl();testSession();testSessionBackwardCompatibility();testLocalServerIdentityVerification();testSystemInfoParsing();testSessionEmpty();testSessionAtomicNoTmpResidue();testSessionAtomicReplacePreservesNewContent();testDeviceIdentity();testDeviceIdentityLoadOrCreate();testDeviceIdentityFallbackEntropy();testAuthTypes();testMediaItemDefaults();testJsonStringField();testJsonIntFloatBool();testJsonExtractArray();testJsonToMediaItem();testBuildTabs();testContinueWatchingRowRefresh();testLatestItemsDirectArray();testUnicodeEscapeDecoding();testBitmapFontMapCodePoint();testBitmapFontMapCodePointLatinAccents();testAmpersandAndJsonEscape();testGenreUnicodeEscapeDecoding();testBitmapFontTruncateUtf8();testClassifyAuthError();testBuildLatestUrl();testBuildLibraryItemsUrl();testLibraryItemsBoundedPageHttp();return miyoofin_test::finish("api-session");}

