// Checkpoint B3+B4+B5a+B5b+B5c1+B5d1+B5d2a+B5e1a+B5e2a+B5e3b+B5f2+B5f3a — tests for authentication, session
// persistence, device identity, URL normalisation, B4 JSON parsing/tab
// building, B5a artwork infrastructure, B5b selected artwork loading,
// B5c1 per-type artwork box dimensions, B5d1 row card geometry + scrolling,
// B5d2a row artwork loading state, B5e1a season parsing groundwork,
// B5e2a episode parsing groundwork, B5e3b initial episode focus,
// B5f2 playback request writing, and B5f3a in-process external playback
// handoff semantics.
// All tests are pure logic (no network calls).
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <curl/curl.h>
#include <string>
#include "miyoofin/version.hpp"
#include "../src/net/JellyfinApi.hpp"
#include "../src/net/ArtworkUrl.hpp"
#include "../src/net/Session.hpp"
#include "../src/net/DeviceIdentity.hpp"
#include "../src/data/MediaItem.hpp"
#include "../src/ui/BitmapFont.hpp"
#include "../src/ui/screens/HomeScreen.hpp"
#include "../src/ui/screens/ServerEntryScreen.hpp"
#include "../src/ui/screens/LoginScreen.hpp"
#include "../src/ui/OnScreenKeyboard.hpp"
#include "../src/image/ImageDecoder.hpp"
#include "../src/cache/ImageCache.hpp"
#include "../src/cache/LibraryCache.hpp"
#include "../src/cache/SyncState.hpp"
#include "../src/cache/OfflineCatalog.hpp"
#include "../src/cache/OfflineLibraryProjection.hpp"
#include "../src/net/HttpClient.hpp"
#include "../src/net/RouteRequest.hpp"
#include "../src/net/RouteStatus.hpp"
#include "../src/net/ServerAddress.hpp"
#include "../src/net/ClockCheck.hpp"
#include "../src/ui/ArtworkLayout.hpp"
#include "../src/ui/MovieTitle.hpp"
#include "../src/ui/ShowsBrowser.hpp"
#include "../src/ui/screens/EpisodeBrowserScreen.hpp"
#include "../src/ui/screens/SeriesScreen.hpp"
#include "../src/ui/screens/MovieDetailsScreen.hpp"
#include "../src/app/ScreenStack.hpp"
#include "../src/app/UiDiagnostics.hpp"
#include "../src/playback/PlaybackRequest.hpp"
#include "../src/playback/OfflinePlaybackJournal.hpp"
#include "../src/download/DownloadTypes.hpp"
#include "../src/download/DownloadManager.hpp"
#include "../src/download/DownloadSupport.hpp"
#include "../src/download/DownloadReconcile.hpp"
#include "../src/download/DownloadUi.hpp"
#include "../src/input/InputManager.hpp"
#include <unistd.h>
#include <sys/stat.h>

using namespace miyoofin;

static int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::printf("  FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
                        __FILE__, __LINE__, std::string(b).c_str(), std::string(a).c_str()); \
            ++g_failures; \
        } \
    } while (0)

#include "cases/test_misc_regressions.inc"
#include "cases/test_ui_foundation.inc"
#include "cases/test_cache_offline.inc"
#include "cases/test_downloads.inc"
#include "cases/test_telemetry.inc"
// -------------------------------------------------------------------
// Test 1: URL normalisation (from B2, kept)
// -------------------------------------------------------------------
#include "cases/test_api_session.inc"
#include "cases/test_artwork_episode.inc"
#include "cases/test_playback_ui.inc"
int main()
{
    testRouteRequest();
    testServerEntryKeyboardCaps();
    testSettingsAddressEntryCancel();
    testLoginKeyboardCaps();
    testOnScreenKeyboardGrid();
    testOnScreenKeyboardSpace();
    testServerEntrySpace();
    testLoginUsernameSpace();
    testLoginPasswordSpace();
    testSharedKeyboardLayoutConsistency();
    testKeyboardVerticalNavActionRow();
    testUiDiagnostics();
    testTelemetrySchemaTypes();
    std::printf("\n--- Movie title organization tests ---\n");
    testMovieOrganizationalTitles();
    testMovieAlphabetOrganization();
    testMovieOrganizationalSort();
    testMovieAlphabetFocus();
    testShowsPresentation();

    std::printf("\n--- local-first cache/grid tests ---\n");
    testLibraryCacheNew(); testLibraryCacheV3SaveLoad(); testLibraryCacheV2BackCompat(); testLibraryCacheV1BackCompat(); testLibraryCacheUnknownVersion(); testLibraryCacheFetchDecision(); testSyncState(); testOfflineCatalog(); testOfflineLibraryProjection(); testSettingsRowActions(); testLanServerAddressClassificationAndSettingsLayout(); testSeriesCachedSeasonHandoff(); testSeasonPosterScheduling(); testNewGridAndSchedule(); testCacheRemoveNew();
    std::printf("MiyooFin Checkpoint B3+B4+B5a+B5b+B5c1+B5d1+B5d2a+B5e1a+B5e2a+B5e3b+B5f2+B5f3a tests\n");
    std::printf("==============================================================================\n\n");

    // B3 tests
    testNormaliseUrl();
    testSession();
    testSessionEmpty();
    testSessionBackwardCompatibility();
    testSessionAtomicNoTmpResidue();
    testSessionAtomicReplacePreservesNewContent();
    testSystemInfoParsing();
    testLocalServerIdentityVerification();
    testDeviceIdentity();
    testDeviceIdentityLoadOrCreate();
    testAuthTypes();

    // B4 tests
    testMediaItemDefaults();
    testJsonStringField();
    testJsonIntFloatBool();
    testJsonExtractArray();
    testJsonToMediaItem();
    testBuildTabs();
    testContinueWatchingRowRefresh();
    testLatestItemsDirectArray();
    testBuildLatestUrl();
    testBuildLibraryItemsUrl();
    testUnicodeEscapeDecoding();
    testBitmapFontMapCodePoint();
    testBitmapFontMapCodePointLatinAccents();
    testAmpersandAndJsonEscape();
    testGenreUnicodeEscapeDecoding();
    testBitmapFontTruncateUtf8();

    // B5a tests — Artwork infrastructure
    std::printf("\n--- B5a artwork infrastructure tests ---\n");
    testBuildImageUrlPrimary();
    testBuildImageUrlThumb();
    testImageTypeAndCacheKeys();
    testCacheFilename();
    testCacheWriteRead();
    testJpegDecodeValid();
    testJpegDecodeInvalid();
    testBinaryHttpResponse();

    // B5b tests — Selected artwork loading
    std::printf("\n--- B5b selected artwork tests ---\n");
    testNoPrimaryTagNoArtwork();
    testArtworkIdentityKey();
    testArtworkLoadGuard();
    testCachedJpegDecodeRoundtrip();
    testFailedLoadLeavesEmpty();
    testArtworkUrlDimensions();

    // B5c1 tests — Per-type artwork box dimensions
    std::printf("\n--- B5c1 per-type artwork box tests ---\n");
    testMovieArtworkBox();
    testShowArtworkBox();
    testEpisodeArtworkBox();
    testOtherArtworkBox();
    testMetadataXFollowsBoxWidth();

    // B5d1 tests — Row card geometry and scrolling
    std::printf("\n--- B5d1 row card geometry + scrolling tests ---\n");
    testMovieRowCard();
    testShowRowCard();
    testEpisodeRowCard();
    testMixedWidthPositions();
    testScrollRightKeepsVisible();
    testScrollLeftDecreases();
    testScrollNeverNegative();
    testScrollIsPixelNotIndex();

    // B5d2a tests — Row artwork loading state
    std::printf("\n--- B5d2a row artwork loading state tests ---\n");
    testMovieRowKeyPrimary();
    testEpisodeRowKeyPrimary();
    testNoPrimaryTagEmptyKey();
    testSameKeyNotLoadedTwice();
    testAllVisibleCandidatesScheduledPerCycle();

    // B5e1a tests — Season parsing groundwork
    std::printf("\n--- B5e1a season parsing tests ---\n");
    testSeasonIndexNumber();
    testSeasonTypeNormalization();

    // B5e2a tests — Episode parsing groundwork
    std::printf("\n--- B5e2a episode parsing tests ---\n");
    testEpisodeJsonParsing();
    testEpisodeTypeNormalization();
    testTicksToMinutes();
    testEpisodeDefaults();

    // B5e3b tests — Initial episode focus
    std::printf("\n--- B5e3b initial episode focus tests ---\n");
    testFindEpisodeIndexFound();
    testFindEpisodeIndexNotFound();
    testFindEpisodeIndexEmpty();
    testFindEpisodeIndexEmptyList();

    // B5g1b tests — bounded predictive episode thumbnail prefetch
    std::printf("\n--- B5g1b bounded prefetch scheduler tests ---\n");
    testEpisodePrefetchScheduler();
    testEpisodeArtworkPreemption();
    testEpisodePrefetchPlaybackResume();

    // B5f2 tests — Playback request
    std::printf("\n--- B5f2 playback request tests ---\n");
    testPlaybackRequestMovie();
    testPlaybackRequestEpisode();
    testPlaybackRequestEmptyId();
    testPlaybackRequestEmptyType();
    testPlaybackRequestRemove();
    testPlaybackResultParsing();
    testPlaybackResultDelay();
    testOfflinePlaybackJournal();

    std::printf("\n--- Download manager state tests ---\n");
    testHlsDownloadStore();
    testDownloadRestartPersistence();
    testDownloadInterruptStates();
    testDownloadPlanBatchAccounting();
    testHlsSizeEstimates(); testHlsFailureClassification();
    testDownloadsUiHelpers();
    testDownloadHierarchy();
    testDownloadSourceReconciliation();

    // B5f3a tests — In-process external playback handoff
    std::printf("\n--- B5f3a external playback handoff tests ---\n");
    testExternalPlaybackFlagInitial();
    testExternalPlaybackFlagSetConsume();
    testExternalPlaybackFlagMultipleSet();
    testPlaybackRequestStillWorks();
    testScreenStackPreservedDuringExternalPlayback();
    testScreenRetirementDoesNotBlockPop();
    testMovieDetailsOpensBeforeArtworkPreparation();
    testMovieDetailsUsesGridArtworkImmediately();

    // Central D-pad hold-to-repeat input timing
    std::printf("\n--- D-pad hold-to-repeat tests ---\n");
    testDpadHoldRepeatTiming();

    // Playback progress display helpers
    std::printf("\n--- Playback progress display tests ---\n");
    testPlaybackPercentFromTicks();
    testPlaybackPercentFallbackToProgress();
    testPlaybackPercentClamping();
    testPlaybackPercentZeroRuntime();
    testPlaybackPercentCompletedItem();
    testFormatPlaybackTimeBasic();
    testFormatPlaybackTimeHours();
    testFormatPlaybackTimeZeroRuntime();
    testFormatPlaybackTimeClampedPosition();
    testFormatPlaybackTimeZeroPosition();

    // Issue #1: Clock check tests — friendly HTTPS error when system clock is wrong
    std::printf("\n--- Clock check tests (Issue #1) ---\n");
    testShouldShowClockErrorTrue();
    testShouldShowClockErrorFalseModernEpoch();
    testShouldShowClockErrorFalseUnrelatedFailure();
    testClockMessageFormat();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All B3+B4+B5a+B5b+B5c1+B5d1+B5d2a+B5e1a+B5e2a+B5e3b+B5f2+B5f3a+progress tests passed.\n");
        return 0;
    }

    std::printf("%d test(s) FAILED.\n", g_failures);
    return 1;
}
