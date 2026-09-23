#include "test_support.hpp"
#include "../src/library/LibraryCoordinator.hpp"
#include <condition_variable>
#include <map>

using namespace miyoofin;

namespace {

struct HierarchyScope
{
    std::string url;
    std::string user;
    std::string key;
};

HierarchyScope hierarchyScope(const char* name)
{
    HierarchyScope scope;
    scope.url = std::string("https://hierarchy-") + name + "-" +
                std::to_string(static_cast<long long>(::getpid())) + ".example";
    scope.user = std::string("hierarchy-") + name + "-user";
    scope.key = LibraryCache::scopeKey(scope.url, scope.user);
    const std::string base = "cache/library/" + scope.key + "/catalog.sqlite3";
    const std::string files[] = {base,
                                 base + "-journal",
                                 base + "-wal",
                                 base + "-shm",
                                 base + ".migrating",
                                 base + ".migrating-journal",
                                 base + ".migrating-wal",
                                 base + ".migrating-shm"};
    for (const auto& file : files)
        std::remove(file.c_str());
    return scope;
}

void removeHierarchyScope(const HierarchyScope& scope)
{
    const std::string directory = "cache/library/" + scope.key;
    const std::string base = directory + "/catalog.sqlite3";
    const std::string files[] = {base,
                                 base + "-journal",
                                 base + "-wal",
                                 base + "-shm",
                                 base + ".migrating",
                                 base + ".migrating-journal",
                                 base + ".migrating-wal",
                                 base + ".migrating-shm"};
    for (const auto& file : files)
        std::remove(file.c_str());
    ::rmdir(directory.c_str());
}

int hierarchyListener()
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0)
        return -1;
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(::listen(listener, 8) == 0);
    return listener;
}

std::string hierarchyUrl(int listener)
{
    sockaddr_in address{};
    socklen_t size = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    return "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
}

void readHierarchyRequest(int client)
{
    char buffer[4096];
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break;
        request.append(buffer, static_cast<std::size_t>(received));
    }
}

void sendHierarchyResponse(int client, const std::string& body, int status)
{
    const char* text = status == 200 ? "OK" : "Internal Server Error";
    const std::string response =
        "HTTP/1.1 " + std::to_string(status) + " " + text +
        "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    (void)::send(client, response.data(), response.size(), 0);
}

class HierarchyServer
{
  public:
    HierarchyServer(std::vector<std::pair<int, std::string>> responses, int blockedRequest = -1)
        : m_listener(hierarchyListener()), m_responses(std::move(responses)),
          m_blockedRequest(blockedRequest)
    {
        m_thread = std::thread([this] {
            for (std::size_t i = 0; i < m_responses.size(); ++i) {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(m_listener, &readable);
                timeval timeout{5, 0};
                if (::select(m_listener + 1, &readable, nullptr, nullptr, &timeout) <= 0)
                    return;
                const int client = ::accept(m_listener, nullptr, nullptr);
                if (client < 0)
                    return;
                readHierarchyRequest(client);
                if (static_cast<int>(i) == m_blockedRequest) {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_blocked = true;
                    m_cv.notify_all();
                    m_cv.wait(lock, [this] { return m_release; });
                }
                sendHierarchyResponse(client, m_responses[i].second, m_responses[i].first);
                ::close(client);
            }
        });
    }

    ~HierarchyServer()
    {
        release();
        if (m_thread.joinable())
            m_thread.join();
        if (m_listener >= 0)
            ::close(m_listener);
    }

    std::string url() const
    {
        return hierarchyUrl(m_listener);
    }

    bool waitUntilBlocked(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this] { return m_blocked; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_release = true;
        }
        m_cv.notify_all();
    }

  private:
    int m_listener = -1;
    std::vector<std::pair<int, std::string>> m_responses;
    int m_blockedRequest = -1;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_blocked = false;
    bool m_release = false;
    std::thread m_thread;
};

MediaItem hierarchySeries(const std::string& id)
{
    MediaItem series;
    series.id = id;
    series.type = "show";
    series.title = id;
    return series;
}

MediaItem hierarchySeason(const std::string& id, const std::string& seriesId)
{
    MediaItem season;
    season.id = id;
    season.type = "season";
    season.seriesId = seriesId;
    season.title = id;
    return season;
}

bool takeHierarchyUntilTerminal(library::LibraryCoordinator& coordinator, std::uint64_t request,
                                std::vector<library::HierarchyResult>& results)
{
    for (;;) {
        library::HierarchyResult result;
        if (coordinator.waitHierarchyResult(request, result) != library::WaitStatus::Ready)
            return false;
        const bool terminal = result.terminal;
        results.push_back(std::move(result));
        if (terminal)
            return true;
    }
}

void testHierarchyConsumerCancellationWakesWaiter()
{
    std::printf("[test] hierarchy consumer cancellation wakes waiter\n");
    const auto scope = hierarchyScope("consumer-cancel");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    HierarchyServer server(
        {
            {200, R"({"Items":[]})"},
        },
        0);
    Session session;
    session.serverUrl = server.url();
    session.userId = scope.user;
    auto coordinator = std::make_shared<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();

    std::uint64_t request = 0;
    CHECK(coordinator->requestHierarchy({hierarchySeries("consumer-cancel-series")}, 31, false,
                                        request));
    CHECK(server.waitUntilBlocked(std::chrono::seconds(2)));

    std::atomic_bool consumerCancellation{false};
    std::mutex barrierMutex;
    std::condition_variable barrierWake;
    bool entered = false;
    library::HierarchyResult result;
    library::WaitStatus waitStatus = library::WaitStatus::InvalidRequest;
    std::thread waiter([&] {
        {
            std::lock_guard<std::mutex> lock(barrierMutex);
            entered = true;
            barrierWake.notify_all();
        }
        waitStatus = coordinator->waitHierarchyResult(request, result, &consumerCancellation);
    });
    {
        std::unique_lock<std::mutex> lock(barrierMutex);
        barrierWake.wait(lock, [&] { return entered; });
    }

    consumerCancellation.store(true, std::memory_order_release);
    coordinator->cancelHierarchyRequest(request);
    waiter.join();
    CHECK(waitStatus == library::WaitStatus::Cancelled);
    CHECK(coordinator->running() && !coordinator->stopped());

    server.release();
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeHierarchyScope(scope);
    std::printf("[test] hierarchy consumer cancellation wakes waiter OK\n");
}

void testHierarchyCachedFirstAndPartialFailure()
{
    std::printf("[test] hierarchy cached-first and partial failure\n");
    const auto scope = hierarchyScope("partial");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    const MediaItem series = hierarchySeries("series-partial");
    const MediaItem cachedSeason = hierarchySeason("season-cached", series.id);
    CHECK(db->stageSeriesHierarchy(series, {cachedSeason}, {{cachedSeason.id, {}}}, 1, 1000, false)
              .get()
              .success);

    HierarchyServer server(
        {{200,
          R"({"Items":[{"Id":"season-live","Type":"Season","Name":"Live Season","SeriesId":"series-partial"}]})"},
         {500, R"({"Error":"episode failure"})"}},
        0);
    Session session;
    session.serverUrl = server.url();
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();

    std::uint64_t request = 0;
    CHECK(coordinator->requestHierarchy({series}, 7, false, request));
    CHECK(server.waitUntilBlocked(std::chrono::seconds(2)));

    library::HierarchyResult cached;
    bool sawCachedFirst = false;
    for (int i = 0; i < 200 && !sawCachedFirst; ++i) {
        if (coordinator->takeHierarchyResult(request, cached))
            sawCachedFirst = cached.cacheOnly && !cached.terminal &&
                             cached.cachedSeasons.size() == 1 &&
                             cached.cachedSeasons[0].id == cachedSeason.id;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(sawCachedFirst);

    server.release();
    std::vector<library::HierarchyResult> results;
    CHECK(takeHierarchyUntilTerminal(*coordinator, request, results));
    CHECK(!results.empty() && results.back().terminal);
    CHECK(!results.back().success && !results.back().checkpointCommitted);
    CHECK(!results.back().message.empty());
    const auto state = db->readSyncState(false, 0, 0).get();
    CHECK(state.success && state.committedGeneration == 0);

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeHierarchyScope(scope);
    std::printf("[test] hierarchy cached-first and partial failure OK\n");
}

void testHierarchyCheckpointRequiresCompleteSuccessAndRejectsStaleGeneration()
{
    std::printf("[test] hierarchy complete checkpoint and stale generation\n");
    const auto scope = hierarchyScope("complete");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    HierarchyServer server(
        {{200,
          R"({"Items":[{"Id":"season-a","Type":"Season","Name":"Season A","SeriesId":"series-a"}]})"},
         {200, R"({"Items":[]})"},
         {200,
          R"({"Items":[{"Id":"season-b","Type":"Season","Name":"Season B","SeriesId":"series-b"}]})"},
         {200, R"({"Items":[]})"}});
    Session session;
    session.serverUrl = server.url();
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestHierarchy({hierarchySeries("series-a"), hierarchySeries("series-b")},
                                        12, true, request));

    std::vector<library::HierarchyResult> results;
    CHECK(takeHierarchyUntilTerminal(*coordinator, request, results));
    CHECK(results.size() >= 3);
    CHECK(results.back().terminal && results.back().success && results.back().checkpointCommitted &&
          results.back().lastReconcileMs > 0);
    const auto state = db->readSyncState(false, 0, 0).get();
    CHECK(state.success && state.committedGeneration == 12);
    CHECK(!coordinator->requestHierarchy({hierarchySeries("stale-series")}, 11, false, request));

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeHierarchyScope(scope);
    std::printf("[test] hierarchy complete checkpoint and stale generation OK\n");
}

void testHierarchyStopJoinsActiveWork()
{
    std::printf("[test] hierarchy stop joins active work\n");
    const auto scope = hierarchyScope("stop");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    HierarchyServer server({{200, R"({"Items":[]})"}}, 0);
    Session session;
    session.serverUrl = server.url();
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestHierarchy({hierarchySeries("series-stop")}, 3, false, request));
    CHECK(server.waitUntilBlocked(std::chrono::seconds(2)));

    std::mutex stopMutex;
    std::condition_variable stopCv;
    bool stopEntered = false;
    std::thread stopper([&] {
        {
            std::lock_guard<std::mutex> lock(stopMutex);
            stopEntered = true;
        }
        stopCv.notify_all();
        coordinator->stop();
    });
    {
        std::unique_lock<std::mutex> lock(stopMutex);
        CHECK(stopCv.wait_for(lock, std::chrono::seconds(2), [&] { return stopEntered; }));
    }
    server.release();
    stopper.join();
    CHECK(coordinator->stopped() && !coordinator->running());

    library::HierarchyResult terminal;
    bool sawTerminal = false;
    while (coordinator->takeHierarchyResult(request, terminal)) {
        if (terminal.terminal) {
            sawTerminal = true;
            break;
        }
    }
    CHECK(sawTerminal && terminal.request == request && terminal.cancelled && !terminal.success &&
          !terminal.checkpointCommitted);
    coordinator.reset();
    db.reset();
    removeHierarchyScope(scope);
    std::printf("[test] hierarchy stop joins active work OK\n");
}

void testHierarchyCancellationAllowsHomeReentry()
{
    std::printf("[test] hierarchy cancellation allows Home re-entry\n");
    const auto scope = hierarchyScope("reentry");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    // The first request stands in for a Home screen being torn down while its
    // hierarchy worker is still in the network call.  The second response is
    // the request accepted by the next Home screen before the old worker has
    // finished unwinding.
    HierarchyServer server({{200, R"({"Items":[]})"}, {200, R"({"Items":[]})"}}, 0);
    Session session;
    session.serverUrl = server.url();
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();

    std::uint64_t oldRequest = 0;
    CHECK(coordinator->requestHierarchy({hierarchySeries("series-old")}, 20, false, oldRequest));
    CHECK(server.waitUntilBlocked(std::chrono::seconds(2)));

    coordinator->cancelHierarchy();
    std::uint64_t newRequest = 0;
    CHECK(coordinator->requestHierarchy({hierarchySeries("series-new")}, 20, false, newRequest));
    CHECK(newRequest != oldRequest);
    library::HierarchyResult discarded;
    CHECK(!coordinator->takeHierarchyResult(oldRequest, discarded));

    server.release();
    std::vector<library::HierarchyResult> results;
    CHECK(takeHierarchyUntilTerminal(*coordinator, newRequest, results));
    CHECK(!results.empty() && results.back().terminal && results.back().success &&
          results.back().checkpointCommitted);
    CHECK(!coordinator->takeHierarchyResult(oldRequest, discarded));

    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeHierarchyScope(scope);
    std::printf("[test] hierarchy cancellation allows Home re-entry OK\n");
}

void testHierarchyMutationSerializesLiveChanges()
{
    std::printf("[test] hierarchy mutation serializes live changes\n");
    const auto scope = hierarchyScope("live-serialization");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    HierarchyServer server({{200, R"({"Items":[]})"}, {200, R"({"Items":[]})"}}, 0);
    Session session;
    session.serverUrl = server.url();
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();

    std::uint64_t hierarchyRequest = 0;
    CHECK(coordinator->requestSeriesSeasons(hierarchySeries("series-live-serialization"),
                                            hierarchyRequest));
    CHECK(server.waitUntilBlocked(std::chrono::seconds(2)));

    JellyfinLibraryChangeBatch batch;
    batch.itemsUpdated.push_back("live-item");
    CHECK(coordinator->requestLiveChange(batch));

    library::LiveLibraryChangeResult liveResult;
    bool livePublishedBeforeHierarchyFinished = false;
    for (int i = 0; i < 100; ++i) {
        if (coordinator->takeLiveChangeResult(liveResult)) {
            livePublishedBeforeHierarchyFinished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(!livePublishedBeforeHierarchyFinished);

    server.release();
    library::HierarchyResult hierarchyResult;
    bool hierarchyFinished = false;
    for (int i = 0; i < 1000 && !hierarchyFinished; ++i) {
        if (coordinator->takeHierarchyResult(hierarchyRequest, hierarchyResult)) {
            hierarchyFinished = hierarchyResult.terminal;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    CHECK(hierarchyFinished && hierarchyResult.success);

    bool livePublishedAfterHierarchyFinished = false;
    for (int i = 0; i < 1000 && !livePublishedAfterHierarchyFinished; ++i) {
        livePublishedAfterHierarchyFinished = coordinator->takeLiveChangeResult(liveResult);
        if (!livePublishedAfterHierarchyFinished)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(livePublishedAfterHierarchyFinished);

    coordinator->stop();
    db.reset();
    removeHierarchyScope(scope);
    std::printf("[test] hierarchy mutation serializes live changes OK\n");
}

void testHierarchyStopPublishesQueuedCancellation()
{
    std::printf("[test] hierarchy stop publishes queued cancellation\n");
    const auto scope = hierarchyScope("queued-stop");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    HierarchyServer server({{200, R"({"Items":[]})"}}, 0);
    Session session;
    session.serverUrl = server.url();
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(session, db, epoch);
    coordinator->start();

    std::uint64_t activeRequest = 0;
    CHECK(coordinator->requestSeriesSeasons(hierarchySeries("series-active"), activeRequest));
    CHECK(server.waitUntilBlocked(std::chrono::seconds(2)));

    std::uint64_t queuedRequest = 0;
    CHECK(coordinator->requestSeriesSeasons(hierarchySeries("series-queued"), queuedRequest));

    std::thread stopper([&] { coordinator->stop(); });
    library::HierarchyResult queuedResult;
    bool queuedTerminal = false;
    for (int i = 0; i < 500 && !queuedTerminal; ++i) {
        if (coordinator->takeHierarchyResult(queuedRequest, queuedResult))
            queuedTerminal = queuedResult.terminal;
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(queuedTerminal && queuedResult.cancelled);

    server.release();
    stopper.join();
    CHECK(coordinator->stopped());

    coordinator.reset();
    db.reset();
    removeHierarchyScope(scope);
    std::printf("[test] hierarchy stop publishes queued cancellation OK\n");
}

// The Home hierarchy late-request race (teardown closing the submission gate
// while the fetch worker is mid-request) needs a live SDL HomeScreen and a
// real coordinator interleaving, so it has no deterministic host seam.  The
// coordinator-side hierarchy cancellation/reentry/stop paths remain covered
// behaviourally by the tests above; the former source-shape guard is removed.

} // namespace

int main()
{
    testHierarchyConsumerCancellationWakesWaiter();
    testHierarchyCachedFirstAndPartialFailure();
    testHierarchyCheckpointRequiresCompleteSuccessAndRejectsStaleGeneration();
    testHierarchyStopJoinsActiveWork();
    testHierarchyCancellationAllowsHomeReentry();
    testHierarchyMutationSerializesLiveChanges();
    testHierarchyStopPublishesQueuedCancellation();
    return miyoofin_test::finish("library_hierarchy");
}
