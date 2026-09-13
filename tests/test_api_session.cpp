#include "test_support.hpp"
#include "cases/test_api_session.inc"

int main()
{
    testNormaliseUrl(); testSession(); testSessionBackwardCompatibility();
    testLocalServerIdentityVerification(); testSystemInfoParsing();
    testSessionEmpty(); testSessionAtomicNoTmpResidue();
    testSessionAtomicReplacePreservesNewContent(); testDeviceIdentity();
    testDeviceIdentityLoadOrCreate(); testAuthTypes(); testMediaItemDefaults();
    testJsonStringField(); testJsonIntFloatBool(); testJsonExtractArray();
    testJsonToMediaItem(); testBuildTabs(); testContinueWatchingRowRefresh();
    testLatestItemsDirectArray(); testUnicodeEscapeDecoding();
    testBitmapFontMapCodePoint(); testBitmapFontMapCodePointLatinAccents();
    testAmpersandAndJsonEscape(); testGenreUnicodeEscapeDecoding();
    testBitmapFontTruncateUtf8(); testBuildLatestUrl(); testBuildLibraryItemsUrl();
    testLibraryItemsBoundedPageHttp(); testChangedHierarchyLightweightProjection();
    return miyoofin_test::finish("api-session");
}
