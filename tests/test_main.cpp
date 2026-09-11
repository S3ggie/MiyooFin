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
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>
#include <tuple>
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
#include "../src/app/RemoteExitSignal.hpp"
#include "../src/app/DisplaySizing.hpp"
#include "../src/catalog/CatalogDb.hpp"
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
#include <csignal>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace miyoofin;

static int g_failures = 0;

static std::string readTestBytes(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

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

static void testCatalogDbLifecycle()
{
    for (int i = 0; i < 20; ++i) {
        CatalogDb db;
    }

    {
        CatalogDb db;
        db.enqueueNoopForTest(CatalogDbPriority::BackgroundSync);
    }
}

static void testRemoteExitSignal()
{
    std::printf("[test] remote exit signal\n");
    installRemoteExitSignalHandler();
    CHECK(!consumeRemoteExitRequest());
    CHECK(std::raise(SIGUSR1) == 0);
    CHECK(std::raise(SIGUSR1) == 0);
    CHECK(consumeRemoteExitRequest());
    CHECK(!consumeRemoteExitRequest());
    std::printf("[test] remote exit signal OK\n");
}

static void testDisplaySizingFallback()
{
    std::printf("[test] SDL display sizing fallback\n");
    const auto fallback = displayDimensionsFor(0, 0);
    CHECK(fallback.width == SCREEN_W);
    CHECK(fallback.height == SCREEN_H);

    const auto negative = displayDimensionsFor(-1, 480);
    CHECK(negative.width == SCREEN_W);
    CHECK(negative.height == SCREEN_H);

    const auto reported = displayDimensionsFor(640, 480);
    CHECK(reported.width == 640);
    CHECK(reported.height == 480);
    std::printf("[test] SDL display sizing fallback OK\n");
}

static void testCatalogDbQueue()
{
    CatalogDb db;
    db.setWorkerPausedForTest(true);
    for (std::size_t i = 0; i < CatalogDb::kMaxPendingJobs; ++i) {
        CHECK(db.enqueueNoopForTest(CatalogDbPriority::BackgroundSync)
              == CatalogDbEnqueueResult::Accepted);
    }
    CHECK(db.enqueueNoopForTest(CatalogDbPriority::Maintenance)
          == CatalogDbEnqueueResult::RejectedFull);
    db.setWorkerPausedForTest(false);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(1)));

    const auto reports = db.jobReportsForTest();
    CHECK(reports.size() == CatalogDb::kMaxPendingJobs);
    for (const auto &report : reports) {
        CHECK(report.priority == CatalogDbPriority::BackgroundSync);
        CHECK(report.disposition == CatalogDbJobDisposition::Completed);
    }
}

static void testCatalogDbPriorityOrdering()
{
    CatalogDb db;
    db.setWorkerPausedForTest(true);
    auto firstBackground = std::make_shared<std::atomic_bool>(false);
    auto secondBackground = std::make_shared<std::atomic_bool>(false);
    CHECK(db.enqueueNoopForTest(
              CatalogDbPriority::BackgroundSync, {0, 0, firstBackground})
          == CatalogDbEnqueueResult::Accepted);
    CHECK(db.enqueueNoopForTest(
              CatalogDbPriority::BackgroundSync, {0, 0, secondBackground})
          == CatalogDbEnqueueResult::Accepted);
    CHECK(db.enqueueNoopForTest(CatalogDbPriority::InteractiveRead)
          == CatalogDbEnqueueResult::Accepted);
    CHECK(db.enqueueNoopForTest(CatalogDbPriority::ForegroundMetadataWrite)
          == CatalogDbEnqueueResult::Accepted);
    db.setWorkerPausedForTest(false);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(1)));

    const auto reports = db.jobReportsForTest();
    CHECK(reports.size() == 4);
    if (reports.size() == 4) {
        CHECK(reports[0].priority == CatalogDbPriority::InteractiveRead);
        CHECK(reports[1].priority == CatalogDbPriority::ForegroundMetadataWrite);
        CHECK(reports[2].priority == CatalogDbPriority::BackgroundSync);
        CHECK(reports[3].priority == CatalogDbPriority::BackgroundSync);
        CHECK(reports[2].metadata.cancellation == firstBackground);
        CHECK(reports[3].metadata.cancellation == secondBackground);
    }
}

static void testCatalogDbCancellationAndGeneration()
{
    CatalogDb db;
    db.setWorkerPausedForTest(true);
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    CatalogDbJobMetadata cancelled{1, 0, cancellation};
    CHECK(db.enqueueNoopForTest(CatalogDbPriority::BackgroundSync, cancelled)
          == CatalogDbEnqueueResult::Accepted);
    cancellation->store(true);
    db.setWorkerPausedForTest(false);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(1)));
    auto reports = db.jobReportsForTest();
    CHECK(reports.size() == 1);
    if (reports.size() == 1) {
        CHECK(reports[0].disposition == CatalogDbJobDisposition::Cancelled);
    }

    CatalogDb generationDb;
    generationDb.setWorkerPausedForTest(true);
    generationDb.setGenerationForTest(1);
    CatalogDbJobMetadata oldGeneration{1, 0, {}};
    CHECK(generationDb.enqueueNoopForTest(CatalogDbPriority::BackgroundSync,
                                           oldGeneration)
          == CatalogDbEnqueueResult::Accepted);
    generationDb.setGenerationForTest(2);
    generationDb.setWorkerPausedForTest(false);
    CHECK(generationDb.waitForIdleForTest(std::chrono::seconds(1)));
    reports = generationDb.jobReportsForTest();
    CHECK(reports.size() == 1);
    if (reports.size() == 1) {
        CHECK(reports[0].disposition == CatalogDbJobDisposition::Superseded);
    }
}

static void testCatalogDbShutdownWithFullQueue()
{
    {
        CatalogDb db;
        db.setWorkerPausedForTest(true);
        for (std::size_t i = 0; i < CatalogDb::kMaxPendingJobs; ++i) {
            CHECK(db.enqueueNoopForTest(CatalogDbPriority::Maintenance)
                  == CatalogDbEnqueueResult::Accepted);
        }
    }
}

static void testCatalogDbScopeLifecycle()
{
    CatalogDb db;
    const auto initialState = db.scopeState();
    CHECK(initialState.requestedEpoch == 0);
    CHECK(!initialState.configured && !initialState.ready);
    CHECK(db.enqueueScopedNoopForTest(CatalogDbPriority::InteractiveRead)
          == CatalogDbEnqueueResult::RejectedScopeNotReady);

    db.setWorkerPausedForTest(true);
    const auto epochA = db.configureScope("https://a.example", "user-a");
    const auto epochB = db.configureScope("https://b.example", "user-a");
    const auto epochC = db.configureScope("https://c.example", "user-a");
    CHECK(epochA < epochB && epochB < epochC);
    CHECK(!db.scopeState().ready);
    CHECK(!db.canPublishForTest(epochA));
    db.setWorkerPausedForTest(false);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(1)));
    auto state = db.scopeState();
    CHECK(state.requestedEpoch == epochC);
    CHECK(state.configured && state.ready);
    CHECK(state.status == CatalogDbScopeStatus::Ready);
    CHECK(!db.canPublishForTest(epochA));
    CHECK(db.canPublishForTest(epochC));
    db.setWorkerPausedForTest(true);
    const auto repeatedEpoch = db.configureScope("https://c.example", "user-a");
    CHECK(repeatedEpoch > epochC);
    CHECK(!db.scopeState().ready);
    CHECK(!db.canPublishForTest(epochC));
    db.setWorkerPausedForTest(false);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(1)));
    state = db.scopeState();
    CHECK(state.requestedEpoch == repeatedEpoch);
    CHECK(state.configured && state.ready);
    CHECK(db.canPublishForTest(repeatedEpoch));
    db.setWorkerPausedForTest(true);
    CHECK(db.enqueueScopedNoopForTest(CatalogDbPriority::InteractiveRead)
          == CatalogDbEnqueueResult::Accepted);
    const auto nextEpoch = db.configureScope("https://next.example", "user-a");
    CHECK(nextEpoch > repeatedEpoch);
    CHECK(!db.canPublishForTest(repeatedEpoch));
    db.setWorkerPausedForTest(false);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(1)));
    auto reports = db.jobReportsForTest();
    CHECK(!reports.empty());
    if (!reports.empty()) {
        CHECK(reports.back().metadata.scopeEpoch == repeatedEpoch);
        CHECK(reports.back().disposition == CatalogDbJobDisposition::Superseded);
    }
    state = db.scopeState();
    CHECK(state.requestedEpoch == nextEpoch && state.ready);

    db.setWorkerPausedForTest(true);
    const auto deconfigured = db.deconfigureScope();
    CHECK(deconfigured > nextEpoch);
    CHECK(!db.scopeState().ready);
    CHECK(!db.canPublishForTest(epochC));
    CHECK(db.enqueueScopedNoopForTest(CatalogDbPriority::BackgroundSync)
          == CatalogDbEnqueueResult::RejectedScopeNotReady);
    db.setWorkerPausedForTest(false);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(1)));
    state = db.scopeState();
    CHECK(!state.configured && !state.ready);
    CHECK(state.status == CatalogDbScopeStatus::Unconfigured);
}

static void testCatalogDbInvalidScope()
{
    CatalogDb db;
    db.setWorkerPausedForTest(true);
    const auto epoch = db.configureScope("   ", "user");
    CHECK(!db.scopeState().ready);
    db.setWorkerPausedForTest(false);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(1)));
    const auto state = db.scopeState();
    CHECK(state.requestedEpoch == epoch);
    CHECK(!state.configured && !state.ready);
    CHECK(state.status == CatalogDbScopeStatus::InvalidIdentity);

    db.setWorkerPausedForTest(true);
    const auto blankUserEpoch = db.configureScope("https://valid.example", " \t");
    CHECK(blankUserEpoch > epoch);
    CHECK(!db.scopeState().ready);
    db.setWorkerPausedForTest(false);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(1)));
    CHECK(db.scopeState().status == CatalogDbScopeStatus::InvalidIdentity);
}

static void testCatalogDbSqliteOwnership()
{
    CatalogDb db;
    CHECK(!db.connectionStateForTest().open);

    const auto epochA = db.configureScope("https://sqlite-a.example", "user-a");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    auto state = db.scopeState();
    CHECK(state.requestedEpoch == epochA && state.ready);
    CHECK(state.error == CatalogDbErrorCategory::None);
    auto connection = db.connectionStateForTest();
    CHECK(connection.open && connection.workerOwned);
    CHECK(db.runSqliteDiagnosticsForTest().success);

    const auto diagnostics = db.runSqliteDiagnosticsForTest();
    CHECK(diagnostics.workerOwned);
    CHECK(diagnostics.foreignKeys == "1");
    CHECK(diagnostics.trustedSchema == "0");
    CHECK(diagnostics.journalMode == "delete");
    CHECK(diagnostics.synchronous == "2");
    CHECK(diagnostics.lockingMode == "normal");

    const auto statement = db.runStatementReuseForTest();
    CHECK(statement.success && statement.workerOwned && statement.statementReused);
    CHECK(db.connectionStateForTest().preparedStatements == 1);

    const auto sqliteError = db.runSqlErrorForTest();
    CHECK(!sqliteError.success);
    CHECK(sqliteError.error == CatalogDbErrorCategory::SqliteError);
    CHECK(!sqliteError.message.empty());
    CHECK(db.writeSentinelForTest("scope-a").success);
    auto sentinel = db.readSentinelForTest();
    CHECK(sentinel.success && sentinel.sentinel == "scope-a");

    const auto epochB = db.configureScope("https://sqlite-b.example", "user-a");
    CHECK(epochB > epochA);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    state = db.scopeState();
    CHECK(state.ready && state.requestedEpoch == epochB);
    CHECK(db.connectionStateForTest().preparedStatements == 0);
    CHECK(db.writeSentinelForTest("scope-b").success);
    sentinel = db.readSentinelForTest();
    CHECK(sentinel.success && sentinel.sentinel == "scope-b");

    const auto epochAReopen = db.configureScope("https://sqlite-a.example", "user-a");
    CHECK(epochAReopen > epochB);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    sentinel = db.readSentinelForTest();
    CHECK(sentinel.success && sentinel.sentinel == "scope-a");

    const auto deconfigured = db.deconfigureScope();
    CHECK(deconfigured > epochAReopen);
    CHECK(!db.scopeState().ready);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    state = db.scopeState();
    CHECK(!state.configured && !state.ready);
    CHECK(db.connectionStateForTest().open == false);
    CHECK(db.enqueueScopedNoopForTest(CatalogDbPriority::InteractiveRead)
          == CatalogDbEnqueueResult::RejectedScopeNotReady);
}

static void testCatalogDbSchemaV1()
{
    CatalogDb db;
    const auto epoch = db.configureScope("https://sqlite-schema.example",
                                        "schema-user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db.scopeState().requestedEpoch == epoch && db.scopeState().ready);

    auto schema = db.runSchemaDiagnosticsForTest();
    CHECK(schema.success && schema.workerOwned && schema.exactSchema);
    CHECK(schema.applicationId == 0x4D59464E);
    CHECK(schema.userVersion == 3);
    CHECK(schema.singletonSeeded && schema.foreignKeyCascade);
    CHECK(schema.checkConstraints);

    CHECK(db.writeSchemaMarkerForTest("preserved").success);
    const auto reopened = db.configureScope("https://sqlite-schema.example",
                                            "schema-user");
    CHECK(reopened > epoch);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    schema = db.runSchemaDiagnosticsForTest();
    CHECK(schema.success && schema.exactSchema);
    const auto marker = db.readSchemaMarkerForTest();
    CHECK(marker.success && marker.sentinel == "preserved");
}

static void testCatalogDbSchemaOpenPolicy()
{
    const std::string suffix = std::to_string(static_cast<long long>(getpid()));
    const std::string createdUrl =
        "https://sqlite-policy-created-" + suffix + ".example";
    const std::string createdUser = "policy-created";
    CatalogDb created;
    const auto createdEpoch = created.configureScope(createdUrl, createdUser);
    CHECK(created.waitForIdleForTest(std::chrono::seconds(2)));
    auto state = created.scopeState();
    CHECK(state.requestedEpoch == createdEpoch && state.ready);
    CHECK(state.openState == CatalogDbOpenState::CreatedV3);

    const auto supportedEpoch = created.configureScope(createdUrl, createdUser);
    CHECK(supportedEpoch > createdEpoch);
    CHECK(created.waitForIdleForTest(std::chrono::seconds(2)));
    state = created.scopeState();
    CHECK(state.ready && state.openState == CatalogDbOpenState::SupportedV3);

    const std::string wrongUrl = "https://sqlite-policy-wrong-" + suffix
        + ".example";
    CatalogDb wrong;
    const auto wrongEpoch = wrong.configureScope(wrongUrl, "policy-wrong");
    CHECK(wrong.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(wrong.scopeState().openState == CatalogDbOpenState::CreatedV3);
    CHECK(wrong.setSchemaMetadataForTest(0x4D59464F, 1).success);
    const std::string wrongPath = "cache/library/"
        + LibraryCache::scopeKey(wrongUrl, "policy-wrong") + "/catalog.sqlite3";
    const std::string wrongBefore = readTestBytes(wrongPath);
    const auto wrongRejectedEpoch = wrong.configureScope(wrongUrl, "policy-wrong");
    CHECK(wrongRejectedEpoch > wrongEpoch);
    CHECK(wrong.waitForIdleForTest(std::chrono::seconds(2)));
    state = wrong.scopeState();
    CHECK(!state.ready && state.openState == CatalogDbOpenState::WrongApplicationId);
    CHECK(state.error == CatalogDbErrorCategory::WrongApplicationId);
    CHECK(readTestBytes(wrongPath) == wrongBefore);

    const std::string futureUrl = "https://sqlite-policy-future-" + suffix
        + ".example";
    CatalogDb future;
    const auto futureEpoch = future.configureScope(futureUrl, "policy-future");
    CHECK(future.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(future.setSchemaMetadataForTest(0x4D59464E, 99).success);
    const std::string futurePath = "cache/library/"
        + LibraryCache::scopeKey(futureUrl, "policy-future") + "/catalog.sqlite3";
    const std::string futureBefore = readTestBytes(futurePath);
    const auto futureRejectedEpoch = future.configureScope(futureUrl,
                                                            "policy-future");
    CHECK(futureRejectedEpoch > futureEpoch);
    CHECK(future.waitForIdleForTest(std::chrono::seconds(2)));
    state = future.scopeState();
    CHECK(!state.ready && state.openState == CatalogDbOpenState::UnsupportedVersion);
    CHECK(state.error == CatalogDbErrorCategory::UnsupportedVersion);
    CHECK(readTestBytes(futurePath) == futureBefore);

    const std::string zeroVersionUrl = "https://sqlite-policy-zero-" + suffix
        + ".example";
    CatalogDb zeroVersion;
    const auto zeroEpoch = zeroVersion.configureScope(zeroVersionUrl,
                                                       "policy-zero");
    CHECK(zeroVersion.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(zeroVersion.setSchemaMetadataForTest(0x4D59464E, 0).success);
    const auto zeroRejectedEpoch = zeroVersion.configureScope(zeroVersionUrl,
                                                               "policy-zero");
    CHECK(zeroRejectedEpoch > zeroEpoch);
    CHECK(zeroVersion.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(zeroVersion.scopeState().openState
          == CatalogDbOpenState::UnsupportedVersion);

    const std::string corruptUrl = "https://sqlite-policy-corrupt-" + suffix
        + ".example";
    CatalogDb corrupt;
    const auto corruptEpoch = corrupt.configureScope(corruptUrl,
                                                      "policy-corrupt");
    CHECK(corrupt.waitForIdleForTest(std::chrono::seconds(2)));
    const std::string corruptPath = "cache/library/"
        + LibraryCache::scopeKey(corruptUrl, "policy-corrupt")
        + "/catalog.sqlite3";
    CHECK(corrupt.deconfigureScope() > corruptEpoch);
    CHECK(corrupt.waitForIdleForTest(std::chrono::seconds(2)));
    std::ofstream corruptFile(corruptPath, std::ios::binary | std::ios::trunc);
    corruptFile << "not a SQLite database";
    corruptFile.close();
    CHECK(corrupt.configureScope(corruptUrl, "policy-corrupt")
          > corruptEpoch);
    CHECK(corrupt.waitForIdleForTest(std::chrono::seconds(2)));
    state = corrupt.scopeState();
    CHECK(!state.ready && state.openState == CatalogDbOpenState::CorruptOrIo);
    CHECK(state.error == CatalogDbErrorCategory::CorruptOrIo);

    CHECK(created.runMigrationRollbackForTest().success);
    CHECK(created.connectionStateForTest().preparedStatements > 0);
    CHECK(created.runStatementReuseForTest().success);
}

static void testCatalogDbMediaItemCodec()
{
    CatalogDb db;
    const auto epoch = db.configureScope("https://sqlite-media-item-codec.example",
                                        "codec-user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db.scopeState().requestedEpoch == epoch && db.scopeState().ready);

    const auto result = db.runMediaItemCodecForTest();
    CHECK(result.success && result.workerOwned);
    CHECK(result.codecPopulatedKinds);
    CHECK(result.codecDefaults);
    CHECK(result.codecBoundaries);
    CHECK(result.codecInvalidKind);
    CHECK(result.codecMissingId);
    CHECK(result.codecNullableRelationships);
}

static void testCatalogDbMediaItemCollections()
{
    CatalogDb db;
    const auto epoch = db.configureScope("https://sqlite-media-item-collections.example",
                                        "collections-user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db.scopeState().requestedEpoch == epoch && db.scopeState().ready);

    const auto result = db.runMediaItemCollectionsForTest();
    CHECK(result.success && result.workerOwned);
    CHECK(result.collectionsZero);
    CHECK(result.collectionsMultiple);
    CHECK(result.collectionsUpdateRemoval);
    CHECK(result.collectionsDeleteCascade);
    CHECK(result.collectionsParity);
}

static void testCatalogDbHierarchyQueries()
{
    CatalogDb db;
    const auto epoch = db.configureScope("https://sqlite-hierarchy-query.example",
                                        "hierarchy-user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db.scopeState().requestedEpoch == epoch && db.scopeState().ready);

    const auto fixture = db.seedHierarchyQueryFixturesForTest();
    CHECK(fixture.success && fixture.workerOwned);
    CHECK(fixture.hierarchyFixture && fixture.hierarchyIndexes);

    auto seasons = db.getSeasons("__task10_series__").get();
    CHECK(seasons.success && seasons.workerOwned && !seasons.superseded);
    CHECK(seasons.items.size() == 3);
    if (seasons.items.size() == 3) {
        CHECK(seasons.items[0].id == "__task10_season_a__");
        CHECK(seasons.items[1].id == "__task10_season_z__");
        CHECK(seasons.items[2].id == "__task10_season_b__");
        CHECK(seasons.items[0].genre == "Drama");
        CHECK(seasons.items[0].imageTags["Primary"] == "season-tag");
    }

    auto episodes = db.getEpisodes("__task10_season_a__").get();
    CHECK(episodes.success && episodes.workerOwned && !episodes.superseded);
    CHECK(episodes.items.size() == 3);
    if (episodes.items.size() == 3) {
        CHECK(episodes.items[0].id == "__task10_episode_a__");
        CHECK(episodes.items[1].id == "__task10_episode_z__");
        CHECK(episodes.items[2].id == "__task10_episode_b__");
        CHECK(episodes.items[0].imageTags["Primary"] == "episode-tag");
    }

    auto empty = db.getSeasons("__task10_missing_series__").get();
    CHECK(empty.success && empty.items.empty());

    auto cancelledToken = std::make_shared<std::atomic_bool>(true);
    CatalogDbJobMetadata cancelledMetadata;
    cancelledMetadata.cancellation = cancelledToken;
    auto cancelled = db.getEpisodes("__task10_season_a__", cancelledMetadata).get();
    CHECK(cancelled.cancelled && !cancelled.success && cancelled.items.empty());

    db.setWorkerPausedForTest(true);
    auto stale = db.getSeasons("__task10_series__");
    db.setGenerationForTest(1);
    db.setWorkerPausedForTest(false);
    const auto staleResult = stale.get();
    CHECK(staleResult.superseded && !staleResult.success
          && staleResult.items.empty());

    const auto cleared = db.clearHierarchyQueryFixturesForTest();
    CHECK(cleared.success && cleared.workerOwned);
}

static void testCatalogDbAtomicHierarchyWrite()
{
    CatalogDb db;
    const auto epoch = db.configureScope("https://sqlite-hierarchy-write.example",
                                        "hierarchy-write-user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db.scopeState().requestedEpoch == epoch && db.scopeState().ready);

    MediaItem series;
    series.id = "__task11_series__";
    series.type = "show";
    series.title = "Atomic Series";
    MediaItem seasonOne;
    seasonOne.id = "__task11_season_one__";
    seasonOne.type = "season";
    seasonOne.seriesId = series.id;
    seasonOne.indexNumber = 1;
    seasonOne.title = "Season One";
    MediaItem seasonTwo = seasonOne;
    seasonTwo.id = "__task11_season_two__";
    seasonTwo.indexNumber = 2;
    seasonTwo.title = "Season Two";
    MediaItem episodeOne;
    episodeOne.id = "__task11_episode_one__";
    episodeOne.type = "episode";
    episodeOne.seriesId = series.id;
    episodeOne.seasonId = seasonOne.id;
    episodeOne.indexNumber = 1;
    episodeOne.title = "Episode One";
    MediaItem episodeTwo = episodeOne;
    episodeTwo.id = "__task11_episode_two__";
    episodeTwo.indexNumber = 2;
    episodeTwo.title = "Episode Two";
    MediaItem episodeThree = episodeTwo;
    episodeThree.id = "__task11_episode_three__";
    episodeThree.seasonId = seasonTwo.id;
    episodeThree.indexNumber = 1;
    episodeThree.title = "Episode Three";

    const std::vector<MediaItem> initialSeasons = {seasonOne, seasonTwo};
    const std::map<std::string, std::vector<MediaItem>> initialEpisodes = {
        {seasonOne.id, {episodeOne, episodeTwo}},
        {seasonTwo.id, {episodeThree}},
    };
    auto first = db.upsertSeriesHierarchy(series, initialSeasons,
                                          initialEpisodes, 10, 100).get();
    CHECK(first.success && first.workerOwned && first.rowsWritten == 6);

    auto initialRead = db.getEpisodes(seasonOne.id).get();
    CHECK(initialRead.success && initialRead.items.size() == 2);

    const std::vector<MediaItem> reducedSeasons = {seasonOne};
    const std::map<std::string, std::vector<MediaItem>> reducedEpisodes = {
        {seasonOne.id, {episodeOne}},
    };
    MediaItem updatedSeries = series;
    updatedSeries.title = "Updated Atomic Series";
    auto reduced = db.upsertSeriesHierarchy(updatedSeries, reducedSeasons,
                                             reducedEpisodes, 11, 200).get();
    CHECK(reduced.success && reduced.rowsWritten == 3);
    auto reducedSeasonsRead = db.getSeasons(series.id).get();
    CHECK(reducedSeasonsRead.success && reducedSeasonsRead.items.size() == 1
          && reducedSeasonsRead.items[0].id == seasonOne.id);
    auto reducedEpisodesRead = db.getEpisodes(seasonOne.id).get();
    CHECK(reducedEpisodesRead.success && reducedEpisodesRead.items.size() == 1
          && reducedEpisodesRead.items[0].id == episodeOne.id);
    auto staleSeasonRead = db.getEpisodes(seasonTwo.id).get();
    CHECK(staleSeasonRead.success && staleSeasonRead.items.empty());

    auto repeated = db.upsertSeriesHierarchy(updatedSeries, reducedSeasons,
                                              reducedEpisodes, 11, 200).get();
    CHECK(repeated.success && repeated.rowsWritten == 3);

    MediaItem malformedSeason = seasonOne;
    malformedSeason.seriesId = "__task11_wrong_series__";
    auto malformed = db.upsertSeriesHierarchy(
        updatedSeries, {malformedSeason}, {{malformedSeason.id, {}}}, 12, 300)
        .get();
    CHECK(!malformed.success && !malformed.superseded);
    auto preservedAfterMalformed = db.getSeasons(series.id).get();
    CHECK(preservedAfterMalformed.success
          && preservedAfterMalformed.items.size() == 1);

    auto injected = db.upsertSeriesHierarchyForTest(
        updatedSeries, initialSeasons, initialEpisodes, 13, 400, 2, -1).get();
    CHECK(!injected.success && !injected.cancelled);
    auto preservedAfterFailure = db.getEpisodes(seasonOne.id).get();
    CHECK(preservedAfterFailure.success && preservedAfterFailure.items.size() == 1
          && preservedAfterFailure.items[0].id == episodeOne.id);

    auto cancelled = db.upsertSeriesHierarchyForTest(
        updatedSeries, initialSeasons, initialEpisodes, 14, 500, -1, 2).get();
    CHECK(!cancelled.success && cancelled.cancelled);
    auto preservedAfterCancel = db.getEpisodes(seasonOne.id).get();
    CHECK(preservedAfterCancel.success && preservedAfterCancel.items.size() == 1
          && preservedAfterCancel.items[0].id == episodeOne.id);

    auto empty = db.upsertSeriesHierarchy(updatedSeries, {}, {}, 15, 600).get();
    CHECK(empty.success && empty.rowsWritten == 1);
    auto emptyRead = db.getSeasons(series.id).get();
    CHECK(emptyRead.success && emptyRead.items.empty());
}

static void testCatalogDbAuthoritativeReconcile()
{
    CatalogDb db;
    const auto epoch = db.configureScope("https://sqlite-reconcile.example",
                                        "reconcile-user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db.scopeState().requestedEpoch == epoch && db.scopeState().ready);

    auto makeSeries = [&](const std::string &seriesId,
                          const std::string &seasonId,
                          const std::string &episodeId) {
        MediaItem series;
        series.id = seriesId;
        series.type = "show";
        series.title = seriesId;
        MediaItem season;
        season.id = seasonId;
        season.type = "season";
        season.seriesId = seriesId;
        season.title = seasonId;
        MediaItem episode;
        episode.id = episodeId;
        episode.type = "episode";
        episode.seriesId = seriesId;
        episode.seasonId = seasonId;
        episode.title = episodeId;
        return std::tuple<MediaItem, std::vector<MediaItem>,
                          std::map<std::string, std::vector<MediaItem>>>(
            series, {season}, {{seasonId, {episode}}});
    };
    const auto keep = makeSeries("__task12_keep__", "__task12_keep_season__",
                                 "__task12_keep_episode__");
    const auto remove = makeSeries("__task12_remove__",
                                   "__task12_remove_season__",
                                   "__task12_remove_episode__");
    const auto downloaded = makeSeries(
        "__task12_downloaded_series__", "__task12_downloaded_season__",
        "__task12_downloaded_episode__");
    auto seed = [&](const auto &fixture) {
        return db.upsertSeriesHierarchy(std::get<0>(fixture), std::get<1>(fixture),
                                        std::get<2>(fixture), 1, 100).get();
    };
    CHECK(seed(keep).success);
    CHECK(seed(remove).success);
    CHECK(seed(downloaded).success);

    auto nonAuthoritative = db.reconcileSeries({}, false).get();
    CHECK(nonAuthoritative.success && nonAuthoritative.skipped
          && !nonAuthoritative.authoritative);
    CHECK(db.getSeasons(std::get<0>(remove).id).get().items.size() == 1);

    auto injected = db.reconcileSeriesForTest(
        {std::get<0>(keep), std::get<0>(downloaded)}, true, 1).get();
    CHECK(!injected.success && !injected.cancelled);
    CHECK(db.getSeasons(std::get<0>(remove).id).get().items.size() == 1);

    auto reconciled = db.reconcileSeries(
        {std::get<0>(keep), std::get<0>(downloaded)}, true).get();
    CHECK(reconciled.success && reconciled.authoritative
          && reconciled.seriesUpserted == 2 && reconciled.seriesDeleted == 1);
    auto removed = db.getSeasons(std::get<0>(remove).id).get();
    CHECK(removed.success && removed.items.empty());
    auto downloadedEpisodes = db.getEpisodes(std::get<1>(downloaded)[0].id).get();
    CHECK(downloadedEpisodes.success && downloadedEpisodes.items.size() == 1
          && downloadedEpisodes.items[0].id == "__task12_downloaded_episode__");

    auto noOp = db.reconcileSeries(
        {std::get<0>(keep), std::get<0>(downloaded)}, true).get();
    CHECK(noOp.success && noOp.authoritative && noOp.seriesDeleted == 0);

    const auto cleared = db.reconcileSeries({}, true).get();
    CHECK(cleared.success && cleared.seriesUpserted == 0
          && cleared.seriesDeleted == 2);
    CHECK(db.getSeasons(std::get<0>(keep).id).get().items.empty());
}

#include "cases/test_misc_regressions.inc"
#include "cases/test_ui_foundation.inc"
#include "cases/test_cache_offline.inc"
#include "cases/test_catalog_migration.inc"
#include "cases/test_catalog_parity.inc"
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
    testRemoteExitSignal();
    testDisplaySizingFallback();
    testCatalogDbLifecycle();
    testCatalogDbQueue();
    testCatalogDbPriorityOrdering();
    testCatalogDbCancellationAndGeneration();
    testCatalogDbShutdownWithFullQueue();
    testCatalogDbScopeLifecycle();
    testCatalogDbInvalidScope();
    testCatalogDbSqliteOwnership();
    testCatalogDbSchemaV1();
    testCatalogDbSchemaOpenPolicy();
    testCatalogDbMediaItemCodec();
    testCatalogDbMediaItemCollections();
    testCatalogDbHierarchyQueries();
    testCatalogDbAtomicHierarchyWrite();
    testCatalogDbAuthoritativeReconcile();
    testCatalogDbFreshBootstrapPathStates();
    testCatalogDbFreshBootstrapUnsupportedFinalPreserved();
    testCatalogDbFreshBootstrapPathError();
    testCatalogDbJellyfinHierarchyStaging();
    testHomeCatalogHierarchyIntegration();
    testCatalogDbOfflineDownloadReconstruction();
    testCatalogDbOfflineRebuildAfterScopeActivation();
    testCatalogDbProjectionParity();
    testCatalogDbLibrarySnapshotSeed();
    testCatalogDbBoundedMediaPaging();
    testCatalogDbMediaPagePreservesViewMembership();
    testCatalogDbMediaPageUpsertAndPopulation();
    testHomeSkipsUnsupportedLibraryViews();
    testCatalogScopeConfiguredBeforeHomePopulation();
    testHomePublishesAfterFirstBoundedPage();
    testHomePreservesAnimeMembershipDuringBoundedReads();
    testHomeUsesCatalogBeforeNetworkRefresh();
    testLibraryCacheHomeParityHarness();
    testCatalogDbDownloadFallbackParity();
    testCatalogDbDuplicateMergeAndAuthoritativeDeletionParity();
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
    testLibraryItemsBoundedPageHttp();
    testChangedHierarchyLightweightProjection();
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
    testHlsSizeEstimates(); testHlsFailureClassification(); testCatalogDbHierarchyPlanning();
    testDownloadsUiHelpers();
    testDownloadHierarchy();
    testDownloadSourceReconciliation();

    // B5f3a tests — In-process external playback handoff
    std::printf("\n--- B5f3a external playback handoff tests ---\n");
    testExternalPlaybackFlagInitial();
    testExternalPlaybackFlagSetConsume();
    testExternalPlaybackFlagMultipleSet();
    testExternalPlaybackSourcePropagation();
    testPlaybackRequestStillWorks();
    testPlaybackRunnerInitializesOnionSdlDrivers();
    testExternalPlaybackFullyReleasesSdlBeforeExec();
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
