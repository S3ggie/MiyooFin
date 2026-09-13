#include "test_support.hpp"

static MediaItem titledMovie(const std::string &id, const std::string &title)
{
    MediaItem item;
    item.id = id;
    item.title = title;
    item.type = "movie";
    return item;
}

#include "cases/test_playback_ui.inc"

int main()
{
    testPlaybackRequestMovie(); testPlaybackRequestEpisode();
    testPlaybackRequestEmptyId(); testPlaybackRequestEmptyType();
    testPlaybackRequestRemove(); testPlaybackResultParsing();
    testPlaybackResultDelay(); testOfflinePlaybackJournal();
    testExternalPlaybackFlagInitial(); testExternalPlaybackFlagSetConsume();
    testExternalPlaybackFlagMultipleSet(); testExternalPlaybackSourcePropagation();
    testPlaybackRequestStillWorks(); testPlaybackRunnerInitializesOnionSdlDrivers();
    testExternalPlaybackFullyReleasesSdlBeforeExec();
    testScreenStackPreservedDuringExternalPlayback();
    testScreenRetirementDoesNotBlockPop(); testMovieDetailsOpensBeforeArtworkPreparation();
    testMovieDetailsUsesGridArtworkImmediately(); testDpadHoldRepeatTiming();
    testDesktopKeyboardMapping(); testHomeTopTabHitTesting();
    testPlaybackPercentFromTicks(); testPlaybackPercentFallbackToProgress();
    testPlaybackPercentClamping(); testPlaybackPercentZeroRuntime();
    testPlaybackPercentCompletedItem(); testFormatPlaybackTimeBasic();
    testFormatPlaybackTimeHours(); testFormatPlaybackTimeZeroRuntime();
    testFormatPlaybackTimeClampedPosition(); testFormatPlaybackTimeZeroPosition();
    return miyoofin_test::finish("playback");
}
