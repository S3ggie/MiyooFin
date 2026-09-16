#include "test_support.hpp"
#include "cases/test_api_core.inc"
int main(){testNormaliseUrl();testMediaItemDefaults();testJsonStringField();testJsonIntFloatBool();testJsonExtractArray();testJsonToMediaItem();testBuildTabs();testContinueWatchingRowRefresh();testLatestItemsDirectArray();testUnicodeEscapeDecoding();testBitmapFontMapCodePoint();testBitmapFontMapCodePointLatinAccents();testAmpersandAndJsonEscape();testGenreUnicodeEscapeDecoding();testBitmapFontTruncateUtf8();testClassifyAuthError();return miyoofin_test::finish("api_core");}
