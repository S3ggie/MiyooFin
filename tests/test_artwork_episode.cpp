#include "test_support.hpp"
#include "cases/test_artwork_episode.inc"

int main()
{
    testBuildImageUrlPrimary(); testBuildImageUrlThumb(); testImageTypeAndCacheKeys();
    testCacheFilename(); testCacheWriteRead(); testJpegDecodeValid();
    testJpegDecodeInvalid(); testBinaryHttpResponse(); testNoPrimaryTagNoArtwork();
    testArtworkIdentityKey(); testArtworkLoadGuard(); testCachedJpegDecodeRoundtrip();
    testFailedLoadLeavesEmpty(); testArtworkUrlDimensions(); testMovieArtworkBox();
    testShowArtworkBox(); testEpisodeArtworkBox(); testOtherArtworkBox();
    testMetadataXFollowsBoxWidth(); testMovieRowCard(); testShowRowCard();
    testEpisodeRowCard(); testMixedWidthPositions(); testScrollRightKeepsVisible();
    testScrollLeftDecreases(); testScrollNeverNegative(); testScrollIsPixelNotIndex();
    testMovieRowKeyPrimary(); testEpisodeRowKeyPrimary(); testNoPrimaryTagEmptyKey();
    testSameKeyNotLoadedTwice(); testAllVisibleCandidatesScheduledPerCycle();
    testSeasonIndexNumber(); testSeasonTypeNormalization(); testEpisodeJsonParsing();
    testEpisodeTypeNormalization(); testTicksToMinutes(); testEpisodeDefaults();
    testFindEpisodeIndexFound(); testFindEpisodeIndexNotFound();
    testFindEpisodeIndexEmpty(); testFindEpisodeIndexEmptyList();
    testEpisodePrefetchScheduler(); testEpisodeArtworkPreemption();
    testEpisodePrefetchPlaybackResume();
    testRowCardScrollOffsetFocused(); testRowCardScrollOffsetUnfocused();
    testRowCardScrollOffsetReportedScenario();
    testRowCardScrollOffsetEqualityContract();
    return miyoofin_test::finish("artwork-episode");
}
