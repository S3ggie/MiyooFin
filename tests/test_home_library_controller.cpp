#include "test_support.hpp"
#include "../src/ui/screens/HomeLibraryController.hpp"
#include "../src/library/LibraryCoordinator.hpp"
#include "../src/library/LibrarySync.hpp"
#include "cases/test_catalog_migration_support.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace miyoofin;

namespace {

struct ControllerScope
{
    std::string url;
    std::string user;
    CatalogMigrationTestPaths paths;
};

ControllerScope makeControllerScope(const char* name)
{
    ControllerScope scope;
    scope.url = std::string("https://home-controller-") + name + "-" +
                std::to_string(static_cast<long long>(::getpid())) + ".example";
    scope.user = std::string("home-controller-") + name + "-user";
    scope.paths = catalogMigrationTestPaths(scope.url, scope.user);
    removeCatalogMigrationTestPaths(scope.paths);
    return scope;
}

std::string controllerDownloadRoot(const char* name)
{
    return "/tmp/miyoofin-home-controller-" + std::string(name) + "-" +
           std::to_string(static_cast<long long>(::getpid()));
}

MediaItem warmCatalogItem(const std::string& id, const std::string& type, const std::string& title)
{
    MediaItem item;
    item.id = id;
    item.type = type;
    item.title = title;
    item.etag = "warm-etag-" + id;
    return item;
}

std::int64_t controllerWallClockMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// A fresh persisted checkpoint makes the coordinator's startup policy choose
// SkipFresh, so no population walk (and no network) is needed; only the
// optional Home rail is attempted.
void seedFreshWarmCatalog(const std::shared_ptr<CatalogDb>& db, const Session& session,
                          std::uint64_t epoch, const std::string& movieId,
                          const std::string& showId, std::uint64_t generation)
{
    auto sync = std::make_shared<library::LibrarySync>(session, db, epoch);
    CHECK(sync->begin(generation).get().success);
    CatalogDbMediaPageWrite movies;
    movies.items = {warmCatalogItem(movieId, "movie", "Warm Movie")};
    movies.viewId = "warm-movies";
    movies.viewName = "Movies";
    movies.collectionType = "movies";
    movies.syncGeneration = generation;
    CHECK(sync->stage(movies).get().success);
    CatalogDbMediaPageWrite shows = movies;
    shows.items = {warmCatalogItem(showId, "show", "Warm Show")};
    shows.viewId = "warm-shows";
    shows.viewName = "Shows";
    shows.collectionType = "tvshows";
    CHECK(sync->stage(shows).get().success);
    CHECK(sync->finalize(generation).get().success);
    // The coordinator's wall clock has second precision, so keep the
    // checkpoint comfortably in the past yet inside the 15-minute fresh
    // window to force the SkipFresh policy deterministically.
    const auto checkpoint = controllerWallClockMs() - 1000;
    CHECK(sync->writeSyncState(checkpoint, checkpoint, generation).get().success);
}

bool tabsContainItem(const std::vector<TabData>& tabs, const std::string& itemId)
{
    for (const auto& tab : tabs)
        for (const auto& row : tab.rows)
            for (const auto& item : row.items)
                if (item.id == itemId)
                    return true;
    return false;
}

// Busy-wait on the worker's exact terminal flag.  This is a deterministic
// barrier, not a sleep: production auto-joins this completed-but-unjoined
// thread on the next start, so the test must not join it here.  The bound is
// only a safety net against a genuine hang.
bool waitForFetchDone(HomeLibraryController& controller)
{
    for (int spins = 0; spins < 2000000 && !controller.done(); ++spins)
        std::this_thread::yield();
    return controller.done();
}

} // namespace

// The deterministic manual-offline path publishes a complete, content-valid
// downloaded presentation without touching the network: a real end-to-end
// exercise of startFetch -> publish -> takePresentation.
static void testOfflineFetchPublishesDownloadedPresentation()
{
    std::printf("[test] HomeLibraryController offline fetch publishes presentation\n");
    const auto scope = makeControllerScope("offline");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    Session session;
    session.serverUrl = scope.url;
    session.userId = scope.user;
    session.manualOfflineMode = true;
    auto sync = std::make_shared<library::LibrarySync>(session, db, epoch);
    library::LibraryCoordinator coordinator(session, db, epoch);
    DownloadManager downloads(session, controllerDownloadRoot("offline"));
    auto query = coordinator.query();

    HomeLibraryController controller(session, query.get(), &coordinator, &downloads);
    CHECK(controller.startFetch({}, {}, {}, false, true, false, {}));
    controller.joinAllWorkers();

    CHECK(controller.done());
    HomeLibraryController::Presentation presentation;
    CHECK(controller.takePresentation(presentation));
    CHECK(presentation.complete);
    CHECK(presentation.libraryOffline);
    CHECK(presentation.contentValid);
    CHECK(presentation.offlineCacheValid);
    CHECK(!presentation.tabs.empty());
    CHECK(presentation.tabs.size() == presentation.offlineTabs.size());
    for (std::size_t i = 0; i < presentation.tabs.size() && i < presentation.offlineTabs.size();
         ++i)
        CHECK(presentation.tabs[i].name == presentation.offlineTabs[i].name);
    CHECK(!controller.initialPopulationInProgress());

    removeCatalogMigrationTestPaths(scope.paths);
    std::printf("[test] HomeLibraryController offline fetch publishes presentation OK\n");
}

// Optional-rail failure isolation: a warm, content-valid catalog is seeded and
// a fresh checkpoint forces SkipFresh, so the catalog population succeeds
// without any network.  The Home rail endpoint is unreachable, so the optional
// rail fails, yet the published terminal result must remain content-valid and
// retain the seeded item identities.
static void testOptionalRailFailureRetainsWarmCatalogContent()
{
    std::printf("[test] HomeLibraryController retains warm content across optional rail failure\n");
    const auto scope = makeControllerScope("railfail");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    Session session;
    // Closed loopback port: the optional rail refresh fails immediately and
    // deterministically without any external network dependency.
    session.serverUrl = "http://127.0.0.1:1";
    session.userId = scope.user;
    // The catalog scope is keyed by the seeded URL, not the session URL.
    seedFreshWarmCatalog(db, Session{}, epoch, "__rail_movie__", "__rail_show__", 11);

    library::LibraryCoordinator coordinator(session, db, epoch);
    coordinator.start();
    DownloadManager downloads(session, controllerDownloadRoot("railfail"));
    auto query = coordinator.query();
    const auto warmMoviePage = query->movies(-1, 8).get();
    CHECK(warmMoviePage.success && warmMoviePage.items.size() == 1 &&
          warmMoviePage.items[0].id == "__rail_movie__");
    const auto warmState = db->readSyncState(false, 0, 0).get();
    CHECK(warmState.success && warmState.lastSuccessfulMs > 0 &&
          warmState.committedGeneration == 11);

    HomeLibraryController controller(session, query.get(), &coordinator, &downloads);
    CHECK(controller.startFetch({}, {}, {}, false, false, false, {}));
    controller.joinAllWorkers();

    CHECK(controller.done());
    HomeLibraryController::Presentation presentation;
    CHECK(controller.takePresentation(presentation));
    CHECK(presentation.complete);
    // Population was skipped (SkipFresh) and the failed rail is optional, so
    // the terminal result stays content-valid with the warm catalog retained.
    CHECK(presentation.contentValid);
    // SkipFresh performs no commit; the cache was already durable.
    CHECK(!presentation.catalogCommitted);
    CHECK(!presentation.continueValid);
    CHECK(!presentation.recentlyAddedValid);
    CHECK(tabsContainItem(presentation.tabs, "__rail_movie__"));
    CHECK(tabsContainItem(presentation.tabs, "__rail_show__"));
    CHECK(!controller.initialPopulationInProgress());

    coordinator.stop();
    removeCatalogMigrationTestPaths(scope.paths);
    std::printf(
        "[test] HomeLibraryController retains warm content across optional rail failure OK\n");
}

// A cold catalog with no reachable population or rail has no authoritative
// content to keep: the published terminal result must be complete, content
// invalid, and carry an error rather than hanging or throwing out of the
// worker.
static void testColdFetchFailurePublishesTerminalFailure()
{
    std::printf("[test] HomeLibraryController cold failure publishes terminal failure\n");
    const auto scope = makeControllerScope("cold");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    Session session;
    session.serverUrl = "http://127.0.0.1:1";
    session.userId = scope.user;
    library::LibraryCoordinator coordinator(session, db, epoch);
    DownloadManager downloads(session, controllerDownloadRoot("cold"));
    auto query = coordinator.query();

    HomeLibraryController controller(session, query.get(), &coordinator, &downloads);
    CHECK(controller.startFetch({}, {}, {}, false, false, false, {}));
    controller.joinAllWorkers();

    CHECK(controller.done());
    HomeLibraryController::Presentation presentation;
    CHECK(controller.takePresentation(presentation));
    CHECK(presentation.complete);
    CHECK(!presentation.contentValid);
    CHECK(!presentation.catalogCommitted);
    CHECK(!presentation.error.empty());
    CHECK(!controller.initialPopulationInProgress());

    removeCatalogMigrationTestPaths(scope.paths);
    std::printf("[test] HomeLibraryController cold failure publishes terminal failure OK\n");
}

// Lifecycle/re-entry: a first fetch that genuinely reaches the server and
// completes must remain unjoined (the production lifecycle auto-joins it on
// the next start), and the second fetch must publish its own terminal result.
static void testFetchLifecycleAndReentry()
{
    std::printf("[test] HomeLibraryController fetch lifecycle and re-entry\n");
    const auto scope = makeControllerScope("lifecycle");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    // A live loopback server.  It signals when the first fetch's Home-rail
    // request has arrived, then holds that response until the test has
    // observed the in-flight refusal and released it.  That makes both the
    // "genuinely in flight" and the "completed" states deterministic without
    // any sleeps.
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(::listen(listener, 4) == 0);
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
    const std::string serverUrl = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));

    std::mutex handshakeMutex;
    std::condition_variable handshake;
    bool requestSeen = false;
    bool releaseServer = false;
    std::thread server([&] {
        for (;;) {
            const int client = ::accept(listener, nullptr, nullptr);
            if (client < 0)
                return;
            char buffer[4096];
            (void)::recv(client, buffer, sizeof(buffer), 0);
            {
                std::lock_guard<std::mutex> lock(handshakeMutex);
                requestSeen = true;
            }
            handshake.notify_all();
            {
                std::unique_lock<std::mutex> lock(handshakeMutex);
                handshake.wait(lock, [&] { return releaseServer; });
            }
            static const char response[] = "HTTP/1.1 200 OK\r\n"
                                           "Content-Type: application/json\r\n"
                                           "Content-Length: 2\r\n"
                                           "Connection: close\r\n"
                                           "\r\n"
                                           "[]";
            (void)::send(client, response, sizeof(response) - 1, 0);
            ::close(client);
        }
    });

    Session session;
    session.serverUrl = serverUrl;
    session.userId = scope.user;
    // A fresh persisted checkpoint forces the coordinator's SkipFresh policy,
    // so the fetch's only network work is the optional Home rail that reaches
    // the live server above; the seeded content is retained.
    seedFreshWarmCatalog(db, Session{}, epoch, "__lifecycle_movie__", "__lifecycle_show__", 17);
    library::LibraryCoordinator coordinator(session, db, epoch);
    coordinator.start();
    DownloadManager downloads(session, controllerDownloadRoot("lifecycle"));
    auto query = coordinator.query();

    HomeLibraryController controller(session, query.get(), &coordinator, &downloads);
    CHECK(controller.startFetch({}, {}, {}, false, false, false, {}));
    // Handshake: the first fetch has reached the server and is waiting for its
    // response, so it is genuinely in flight while still unjoined.
    {
        std::unique_lock<std::mutex> lock(handshakeMutex);
        handshake.wait(lock, [&] { return requestSeen; });
    }
    // A second start while the first is still in flight is refused.
    CHECK(!controller.startFetch({}, {}, {}, false, false, false, {}));
    CHECK(!controller.done());

    // Release the server and let the first fetch run to completion without
    // ever joining it.
    {
        std::lock_guard<std::mutex> lock(handshakeMutex);
        releaseServer = true;
    }
    handshake.notify_all();
    CHECK(waitForFetchDone(controller));
    CHECK(controller.done());
    CHECK(controller.complete());

    HomeLibraryController::Presentation first;
    CHECK(controller.takePresentation(first));
    CHECK(first.complete);
    const std::uint64_t firstGeneration = first.fetchGeneration;
    // The publication is consumed exactly once.
    CHECK(!controller.takePresentation(first));
    CHECK(!controller.ready());

    // The completed prior fetch was never joined, so this start must exercise
    // the automatic-join re-entry path and then publish a fresh terminal
    // result.
    CHECK(controller.startFetch({}, {}, {}, false, false, false, {}));
    CHECK(waitForFetchDone(controller));
    CHECK(controller.done());

    HomeLibraryController::Presentation second;
    CHECK(controller.takePresentation(second));
    CHECK(second.complete);
    CHECK(second.fetchGeneration > firstGeneration);
    CHECK(!controller.takePresentation(second));
    CHECK(!controller.ready());

    controller.joinAllWorkers();
    coordinator.stop();
    // Release a still-blocked server and wake the blocked accept() before
    // close() so the server thread can observe the closed listener and exit.
    {
        std::lock_guard<std::mutex> lock(handshakeMutex);
        releaseServer = true;
    }
    handshake.notify_all();
    ::shutdown(listener, SHUT_RDWR);
    ::close(listener);
    server.join();
    removeCatalogMigrationTestPaths(scope.paths);
    std::printf("[test] HomeLibraryController fetch lifecycle and re-entry OK\n");
}

int main()
{
    testOfflineFetchPublishesDownloadedPresentation();
    testOptionalRailFailureRetainsWarmCatalogContent();
    testColdFetchFailurePublishesTerminalFailure();
    testFetchLifecycleAndReentry();
    return miyoofin_test::finish("home_library_controller");
}
