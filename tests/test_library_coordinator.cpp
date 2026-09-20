#include "test_support.hpp"
#include "../src/library/LibraryCoordinator.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace miyoofin;

namespace {

library::LibraryCoordinator makeCoordinator()
{
    Session session;
    session.manualOfflineMode = true;
    return library::LibraryCoordinator(
        session, std::make_shared<CatalogDb>(), 0);
}

struct CoordinatorTestScope {
    std::string url;
    std::string user;
    std::string scope;
};

CoordinatorTestScope coordinatorTestScope(const char *name)
{
    CoordinatorTestScope scope;
    scope.url = std::string("https://coordinator-") + name + "-"
        + std::to_string(static_cast<long long>(::getpid())) + ".example";
    scope.user = std::string("coordinator-") + name + "-user";
    scope.scope = LibraryCache::scopeKey(scope.url, scope.user);
    const std::string base = "cache/library/" + scope.scope + "/catalog.sqlite3";
    const std::string files[] = {base, base + "-journal", base + "-wal",
                                 base + "-shm", base + ".migrating",
                                 base + ".migrating-journal",
                                 base + ".migrating-wal",
                                 base + ".migrating-shm"};
    for (const auto &file : files)
        std::remove(file.c_str());
    return scope;
}

void removeCoordinatorTestScope(const CoordinatorTestScope &scope)
{
    const std::string libraryDirectory = "cache/library/" + scope.scope;
    const std::string base = libraryDirectory + "/catalog.sqlite3";
    const std::string files[] = {base, base + "-journal", base + "-wal",
                                 base + "-shm", base + ".migrating",
                                 base + ".migrating-journal",
                                 base + ".migrating-wal",
                                 base + ".migrating-shm"};
    for (const auto &file : files)
        std::remove(file.c_str());
    ::rmdir(libraryDirectory.c_str());
}

int coordinatorTestListener()
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
    CHECK(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) == 0);
    CHECK(::listen(listener, 4) == 0);
    return listener;
}

std::string coordinatorTestUrl(int listener)
{
    sockaddr_in address{};
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                        &addressSize) == 0);
    return "http://127.0.0.1:" +
        std::to_string(ntohs(address.sin_port));
}

int coordinatorAccept(int listener)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval timeout{5, 0};
    if (::select(listener + 1, &readable, nullptr, nullptr, &timeout) <= 0)
        return -1;
    return ::accept(listener, nullptr, nullptr);
}

void coordinatorReadRequest(int client)
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

void coordinatorSendJson(int client, const std::string &body,
                         int status = 200)
{
    const char *statusText = status == 200 ? "OK" : "Internal Server Error";
    const std::string response = "HTTP/1.1 " + std::to_string(status) + " "
        + statusText + "\r\nContent-Type: application/json\r\n"
        + "Content-Length: " + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n" + body;
    (void)::send(client, response.data(), response.size(), 0);
}

bool takeFullUpdate(library::LibraryCoordinator &coordinator,
                    std::uint64_t request,
                    library::FullPopulationUpdate &terminal,
                    bool &sawPage)
{
    for (int i = 0; i < 1000; ++i) {
        library::FullPopulationUpdate update;
        if (coordinator.takeFullPopulationUpdate(request, update)) {
            sawPage = sawPage || update.pageValid;
            if (update.terminal) {
                terminal = std::move(update);
                return true;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return false;
}

void testLibraryCoordinatorIsTheSingleStartupDriver()
{
    std::printf("[test] LibraryCoordinator single startup driver\n");
    auto coordinator = makeCoordinator();
    coordinator.start();
    CHECK(coordinator.running());

    // The first caller reserves the only startup slot; a second Home startup
    // request cannot create a competing operation.
    CHECK(coordinator.startStartupSync(false));
    CHECK(!coordinator.startStartupSync(true));
    library::StartupSyncResult result;
    for (int i = 0; i < 200 && !coordinator.takeStartupSyncResult(result); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);

    auto status = coordinator.status();
    CHECK(!status.startupInFlight && !status.cancelRequested);

    // Cancellation and stop are both intentionally safe to repeat.
    coordinator.cancelStartupSync();
    coordinator.cancelStartupSync();
    coordinator.stop();
    coordinator.stop();
    CHECK(coordinator.stopped() && !coordinator.running());
    CHECK(!coordinator.takeStartupSyncResult(result));

    // With startup disabled there is no worker/result thread to join. Keep a
    // source-level assertion on the enabled seam's lock-safe join ordering so
    // this test still guards the deadlock regression before that path opens.
    const auto source = miyoofin_test::readTestBytes(
        "src/library/LibraryCoordinator.cpp");
    CHECK(miyoofin_test::sourceContains(
        source, "kCoordinatorStartupSyncEnabled = true"));
    CHECK(miyoofin_test::sourceContains(
        source, "if (priorThread.joinable()) priorThread.join();"));
    CHECK(miyoofin_test::sourceContains(
        source, "if (startupThread.joinable()) startupThread.join();"));
    const auto priorMove = miyoofin_test::sourcePos(
        source, "priorThread = std::move(m_startupThread)");
    const auto priorJoin = miyoofin_test::sourcePos(
        source, "priorThread.join()");
    const auto startupLockReacquire = miyoofin_test::sourcePos(
        source, "const auto cancellation = m_startupCancellation");
    CHECK(priorMove < priorJoin && priorJoin < startupLockReacquire);
    const auto homeSync = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreenSync.cpp");
    CHECK(miyoofin_test::sourceContains(
        homeSync, "startStartupSync(initialPagePublished)"));
    CHECK(miyoofin_test::sourceContains(
        homeSync, "takeStartupSyncResult"));
    CHECK(miyoofin_test::sourceContains(
        homeSync, "requestFullPopulation(populationRequest)"));
    CHECK(miyoofin_test::sourceContains(
        homeSync, "takeFullPopulationUpdate"));
    CHECK(miyoofin_test::sourceContains(
        homeSync, "cancelFullPopulation"));
    CHECK(!miyoofin_test::sourceContains(
        homeSync, "decideHomeStartupSync("));
    CHECK(!miyoofin_test::sourceContains(
        homeSync, "JellyfinApi::getViews"));
    CHECK(!miyoofin_test::sourceContains(
        homeSync, "JellyfinApi::getLibraryItemsPage"));
    CHECK(!miyoofin_test::sourceContains(homeSync, "sync->begin("));
    CHECK(!miyoofin_test::sourceContains(homeSync, "sync->stage("));
    CHECK(!miyoofin_test::sourceContains(homeSync, "sync->finalize("));
    const auto startupGuard = miyoofin_test::sourcePos(
        homeSync, "m_initialPopulationInProgress = true");
    const auto coordinatorStart = miyoofin_test::sourcePos(
        homeSync, "startStartupSync(initialPagePublished)");
    CHECK(startupGuard != std::string::npos);
    CHECK(coordinatorStart != std::string::npos);
    CHECK(startupGuard < coordinatorStart);
    std::printf("[test] LibraryCoordinator single startup driver OK\n");
}

void testStopRacingStartupIsSafe()
{
    std::printf("[test] LibraryCoordinator stop vs startup\n");
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto coordinator = makeCoordinator();
        coordinator.start();

        std::atomic_bool go{false};
        std::thread startup([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void)coordinator.startStartupSync(false);
        });
        std::thread stopper([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            coordinator.stop();
        });
        go.store(true, std::memory_order_release);

        startup.join();
        stopper.join();
        CHECK(coordinator.stopped());
        CHECK(!coordinator.running());
    }
    std::printf("[test] LibraryCoordinator stop vs startup OK\n");
}

void testLiveChangesWaitForSerializedSyncSlots()
{
    std::printf("[test] LibraryCoordinator live-change seam\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    CHECK(coordinator.startStartupSync(false));
    JellyfinLibraryChangeBatch batch;
    batch.itemsUpdated.push_back("item-1");
    CHECK(coordinator.requestLiveChange(batch));
    library::LiveChangeIdentity identity;
    CHECK(!coordinator.takeLiveChangeRequest(batch, identity));

    library::StartupSyncResult startupResult;
    for (int i = 0; i < 200 && !coordinator.takeStartupSyncResult(startupResult);
         ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(coordinator.takeLiveChangeRequest(batch, identity));
    CHECK(batch.itemsUpdated.size() == 1);

    // A live result keeps the serialized slot until Home consumes it. A
    // queued event must survive both top-level sync rejections unchanged.
    JellyfinLibraryChangeBatch queuedDuringLive;
    queuedDuringLive.itemsUpdated.push_back("queued-item");
    CHECK(coordinator.requestLiveChange(queuedDuringLive));
    library::LiveChangeIdentity queuedIdentity;
    library::LiveLibraryChangeResult liveResult;
    liveResult.success = true;
    CHECK(coordinator.publishLiveChangeResult(identity, liveResult));
    CHECK(!coordinator.startStartupSync(false));
    CHECK(!coordinator.beginFullSync());
    CHECK(!coordinator.takeLiveChangeRequest(queuedDuringLive,
                                             queuedIdentity));
    CHECK(coordinator.takeLiveChangeResult(identity, liveResult));
    CHECK(coordinator.takeLiveChangeRequest(queuedDuringLive, queuedIdentity));
    CHECK(queuedDuringLive.itemsUpdated.size() == 1);
    liveResult.success = true;
    CHECK(coordinator.publishLiveChangeResult(queuedIdentity, liveResult));
    CHECK(coordinator.takeLiveChangeResult(queuedIdentity, liveResult));

    CHECK(coordinator.beginFullSync());
    CHECK(coordinator.status().inFlight);
    CHECK(coordinator.requestLiveChange(batch));
    CHECK(!coordinator.takeLiveChangeRequest(batch, identity));
    coordinator.finishFullSync();
    CHECK(coordinator.takeLiveChangeRequest(batch, identity));
    CHECK(batch.itemsUpdated.size() == 1);

    // A queued batch must survive while Home's previous live worker still
    // owns the serialized consumer slot.
    JellyfinLibraryChangeBatch nextBatch;
    nextBatch.itemsUpdated.push_back("item-2");
    CHECK(coordinator.requestLiveChange(nextBatch));
    library::LiveChangeIdentity nextIdentity;
    CHECK(!coordinator.takeLiveChangeRequest(nextBatch, nextIdentity));

    liveResult.success = true;
    library::LiveChangeIdentity stale = identity;
    ++stale.request;
    CHECK(!coordinator.publishLiveChangeResult(stale, liveResult));
    CHECK(coordinator.publishLiveChangeResult(identity, std::move(liveResult)));
    CHECK(coordinator.takeLiveChangeResult(identity, liveResult));
    CHECK(liveResult.success);
    CHECK(coordinator.takeLiveChangeRequest(nextBatch, nextIdentity));
    CHECK(nextBatch.itemsUpdated.size() == 1);
    coordinator.discardLiveChangeResults();
    CHECK(!coordinator.publishLiveChangeResult(identity, std::move(liveResult)));
    coordinator.stop();

    // A rejected publication is the completion path for Home's worker when
    // stop/discard clears the active identity; keep the Home-side signal in
    // place so this regression cannot become a silent wait again.
    const auto homeSync = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreenSync.cpp");
    const auto rejectedPublish = miyoofin_test::sourcePos(
        homeSync, "publishLiveChangeResult(");
    const auto doneSignal = miyoofin_test::sourcePos(
        homeSync, "m_liveChangeDone.store(true);");
    CHECK(rejectedPublish != std::string::npos);
    CHECK(doneSignal != std::string::npos);
    std::printf("[test] LibraryCoordinator live-change seam OK\n");
}

void testSafetyReconcilePublishesCoordinatorResult()
{
    std::printf("[test] LibraryCoordinator safety reconcile result\n");
    Session session;
    auto coordinator = library::LibraryCoordinator(
        session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    CHECK(coordinator.requestSafetyReconcile());
    CHECK(!coordinator.requestSafetyReconcile());
    library::SafetyReconcileResult result;
    for (int i = 0; i < 200 && !coordinator.takeSafetyReconcileResult(result);
         ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);
    CHECK(!coordinator.status().safetyReconcileInFlight);

    const auto homeHeader = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreen.hpp");
    const auto homeSync = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreenSync.cpp");
    const auto homeApply = miyoofin_test::readTestBytes(
        "src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(miyoofin_test::sourceContains(
        homeSync, "requestSafetyReconcile()"));
    CHECK(miyoofin_test::sourceContains(
        homeApply, "takeSafetyReconcileResult(result)"));
    CHECK(!miyoofin_test::sourceContains(
        homeHeader, "m_safetyReconcileThread"));
    CHECK(!miyoofin_test::sourceContains(
        homeHeader, "m_safetyReconcileCancellation"));
    CHECK(!miyoofin_test::sourceContains(
        homeHeader, "m_safetyReconcileDone"));
    std::printf("[test] LibraryCoordinator safety reconcile result OK\n");
}

void testSafetyReconcileStopPublishesCompletion()
{
    std::printf("[test] LibraryCoordinator safety reconcile stop\n");
    Session session;
    auto coordinator = library::LibraryCoordinator(
        session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    CHECK(coordinator.requestSafetyReconcile());
    coordinator.stop();

    library::SafetyReconcileResult result;
    CHECK(coordinator.takeSafetyReconcileResult(result));
    CHECK(result.error == CatalogDbErrorCategory::ScopeNotReady);
    coordinator.stop();
    std::printf("[test] LibraryCoordinator safety reconcile stop OK\n");
}

void testHomeRailStopPublishesCompletion()
{
    std::printf("[test] LibraryCoordinator Home rail lifecycle\n");
    Session session;
    session.serverUrl = "http://127.0.0.1:1";
    auto coordinator = library::LibraryCoordinator(
        session, std::make_shared<CatalogDb>(), 0);
    coordinator.start();

    std::uint64_t request = 0;
    CHECK(coordinator.requestHomeRailRefresh(request));
    coordinator.stop();

    // stop() may race the transport worker.  Either way, the worker must
    // publish a terminal (possibly cancelled) result for Home's wait loop.
    library::HomeRailResult result;
    CHECK(coordinator.takeHomeRailResult(request, result));
    CHECK(result.request == request);

    const auto source = miyoofin_test::readTestBytes(
        "src/library/LibraryCoordinator.cpp");
    const auto publish = miyoofin_test::sourcePos(
        source, "if (requestId == m_homeRailRequest)");
    CHECK(publish != std::string::npos);
    CHECK(miyoofin_test::sourceContains(
        source, "m_homeRailResultReady = true;"));
    std::printf("[test] LibraryCoordinator Home rail lifecycle OK\n");
}

void testFullPopulationSuccessCommitsCheckpoint()
{
    std::printf("[test] LibraryCoordinator full population success\n");
    const auto scope = coordinatorTestScope("success");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    const std::string serverUrl = coordinatorTestUrl(listener);
    const std::string viewsBody =
        R"({"Items":[{"Id":"movies","Name":"Movies","CollectionType":"movies"}]})";
    const std::string pageBody =
        R"({"StartIndex":0,"TotalRecordCount":1,"Items":[{"Id":"coordinator-movie","Type":"Movie","Name":"Coordinator Movie"}]})";
    std::thread server([&] {
        const std::string bodies[] = {viewsBody, pageBody};
        for (const auto &body : bodies) {
            const int client = coordinatorAccept(listener);
            if (client < 0)
                return;
            coordinatorReadRequest(client);
            coordinatorSendJson(client, body);
            ::close(client);
        }
    });

    Session session;
    session.serverUrl = serverUrl;
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(
        session, db, epoch);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    server.join();

    CHECK(sawPage);
    CHECK(terminal.success && terminal.committed
          && terminal.checkpointCommitted);
    CHECK(!terminal.cancelled && !terminal.superseded);
    CHECK(terminal.generation > 0 && terminal.mediaCount == 1);
    CHECK(terminal.metadataTotal == 1 && terminal.metadataCompleted == 1);
    const auto state = db->readSyncState(
        false, 0, 0, {0, epoch, {}}).get();
    CHECK(state.success && state.committedGeneration == terminal.generation);
    CHECK(state.lastSuccessfulMs == terminal.checkpointMs
          && state.lastSuccessfulMs > 0);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator full population success OK\n");
}

void testFullPopulationCancellationAbortsStagedGeneration()
{
    std::printf("[test] LibraryCoordinator full population cancellation\n");
    const auto scope = coordinatorTestScope("cancel");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    const std::string serverUrl = coordinatorTestUrl(listener);
    const std::string viewsBody =
        R"({"Items":[{"Id":"movies","Name":"Movies","CollectionType":"movies"}]})";
    std::atomic_bool pageConnected{false};
    std::atomic_bool releasePage{false};
    std::thread server([&] {
        const int viewsClient = coordinatorAccept(listener);
        if (viewsClient < 0)
            return;
        coordinatorReadRequest(viewsClient);
        coordinatorSendJson(viewsClient, viewsBody);
        ::close(viewsClient);
        const int pageClient = coordinatorAccept(listener);
        if (pageClient < 0)
            return;
        pageConnected.store(true, std::memory_order_release);
        while (!releasePage.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ::close(pageClient);
    });

    Session session;
    session.serverUrl = serverUrl;
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(
        session, db, epoch);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    for (int i = 0; i < 500 && !pageConnected.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(pageConnected.load());
    coordinator->cancelFullPopulation();
    releasePage.store(true, std::memory_order_release);
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    server.join();

    CHECK(terminal.terminal && terminal.cancelled);
    CHECK(terminal.error == CatalogDbErrorCategory::Superseded
          && !terminal.success && !terminal.committed);
    CHECK(!terminal.checkpointCommitted);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator full population cancellation OK\n");
}

void testFullPopulationFailureAbortsWithoutCheckpoint()
{
    std::printf("[test] LibraryCoordinator full population failure\n");
    const auto scope = coordinatorTestScope("failure");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    const int listener = coordinatorTestListener();
    if (listener < 0) {
        removeCoordinatorTestScope(scope);
        return;
    }
    std::thread server([&] {
        const int client = coordinatorAccept(listener);
        if (client < 0)
            return;
        coordinatorReadRequest(client);
        coordinatorSendJson(client, R"({"Error":"failed"})", 500);
        ::close(client);
    });
    Session session;
    session.serverUrl = coordinatorTestUrl(listener);
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(
        session, db, epoch);
    coordinator->start();
    std::uint64_t request = 0;
    CHECK(coordinator->requestFullPopulation(request));
    bool sawPage = false;
    library::FullPopulationUpdate terminal;
    CHECK(takeFullUpdate(*coordinator, request, terminal, sawPage));
    server.join();

    CHECK(terminal.terminal && !terminal.success && !terminal.cancelled);
    CHECK(!terminal.committed && !terminal.checkpointCommitted);
    CHECK(!terminal.message.empty());
    const auto state = db->readSyncState(
        false, 0, 0, {0, epoch, {}}).get();
    CHECK(state.success && state.committedGeneration == 0);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    ::close(listener);
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator full population failure OK\n");
}

void testFullPopulationRejectsStaleRequestAndHomeState()
{
    std::printf("[test] LibraryCoordinator stale publication rejection\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    // A stale request identity must never consume the current publication.
    library::FullPopulationUpdate update;
    CHECK(!coordinator.takeFullPopulationUpdate(1, update));

    library::HomeState current;
    current.scopeEpoch = 0;
    current.catalogGeneration = 4;
    current.revision = 8;
    current.contentValid = true;
    CHECK(coordinator.publishHomeState(current));
    current.revision = 7;
    CHECK(!coordinator.publishHomeState(current));
    current.revision = 9;
    current.catalogGeneration = 3;
    CHECK(!coordinator.publishHomeState(current));
    std::shared_ptr<const library::HomeState> published;
    CHECK(coordinator.takeHomeState(published));
    CHECK(published && published->revision == 8
          && published->catalogGeneration == 4);
    coordinator.stop();
    std::printf("[test] LibraryCoordinator stale publication rejection OK\n");
}

void testProvisionalHomePublicationRetainsCommittedContent()
{
    std::printf("[test] provisional Home publication retention\n");
    auto coordinator = makeCoordinator();
    coordinator.start();

    library::HomeState committed;
    committed.scopeEpoch = 0;
    committed.catalogGeneration = 7;
    committed.contentValid = true;
    committed.tabs = {{"Movies", {{"Movies", {{"committed-movie"}}}}}};
    CHECK(coordinator.publishHomeState(committed));
    std::shared_ptr<const library::HomeState> state;
    CHECK(coordinator.takeHomeState(state));
    CHECK(state && state->tabs.front().rows.front().items.front().id
          == "committed-movie");

    // This models the first-page publication: it is consumable as a
    // progress update without replacing the committed Home catalog.
    library::HomeState provisional;
    provisional.scopeEpoch = 0;
    provisional.catalogGeneration = 7;
    provisional.contentValid = false;
    provisional.tabs = {{"Movies", {{"Movies", {{"partial-movie"}}}}}};
    CHECK(coordinator.publishHomeState(provisional));
    CHECK(coordinator.takeHomeState(state));
    CHECK(state && state->tabs.front().rows.front().items.front().id
          == "committed-movie");

    // Later-page failure and cancellation both use the non-authoritative
    // terminal path; neither may leave the partial first page authoritative.
    library::HomeState failed = provisional;
    failed.error = "later page failed";
    CHECK(coordinator.publishHomeState(failed));
    CHECK(coordinator.takeHomeState(state));
    CHECK(state && state->error == "later page failed"
          && state->tabs.front().rows.front().items.front().id
                 == "committed-movie");
    library::HomeState cancelled = provisional;
    cancelled.error = "population cancelled";
    CHECK(coordinator.publishHomeState(cancelled));
    CHECK(coordinator.takeHomeState(state));
    CHECK(state && state->error == "population cancelled"
          && state->tabs.front().rows.front().items.front().id
                 == "committed-movie");
    coordinator.stop();
    std::printf("[test] provisional Home publication retention OK\n");
}

void testCoordinatorSerializesStartupFullSafetyAndLive()
{
    std::printf("[test] LibraryCoordinator serialization gates\n");
    const auto scope = coordinatorTestScope("serialization");
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(scope.url, scope.user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    Session session;
    session.serverUrl = "http://127.0.0.1:1";
    session.userId = scope.user;
    auto coordinator = std::make_unique<library::LibraryCoordinator>(
        session, db, epoch);
    coordinator->start();

    // Hold the startup DB read so all other top-level work is observed while
    // startup owns the serialized slot.
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->startStartupSync(false));
    std::uint64_t request = 0;
    CHECK(!coordinator->requestFullPopulation(request));
    CHECK(!coordinator->requestSafetyReconcile());
    JellyfinLibraryChangeBatch batch;
    batch.itemsUpdated.push_back("startup-queued-live");
    CHECK(coordinator->requestLiveChange(batch));
    library::LiveChangeIdentity identity;
    CHECK(!coordinator->takeLiveChangeRequest(batch, identity));
    coordinator->cancelStartupSync();
    db->setWorkerPausedForTest(false);
    library::StartupSyncResult startupResult;
    for (int i = 0; i < 500
         && !coordinator->takeStartupSyncResult(startupResult); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK(startupResult.cancelled || startupResult.error
          == CatalogDbErrorCategory::ScopeNotReady
          || startupResult.success);

    // A paused safety worker similarly blocks startup, population, and live
    // consumption while retaining the queued event.
    db->setWorkerPausedForTest(true);
    CHECK(coordinator->requestSafetyReconcile());
    CHECK(!coordinator->startStartupSync(false));
    CHECK(!coordinator->requestFullPopulation(request));
    CHECK(!coordinator->takeLiveChangeRequest(batch, identity));
    coordinator->cancelSafetyReconcile();
    db->setWorkerPausedForTest(false);
    coordinator->stop();
    coordinator.reset();
    db.reset();
    removeCoordinatorTestScope(scope);
    std::printf("[test] LibraryCoordinator serialization gates OK\n");
}

} // namespace

int main()
{
    testLibraryCoordinatorIsTheSingleStartupDriver();
    testStopRacingStartupIsSafe();
    testLiveChangesWaitForSerializedSyncSlots();
    testSafetyReconcilePublishesCoordinatorResult();
    testSafetyReconcileStopPublishesCompletion();
    testHomeRailStopPublishesCompletion();
    testFullPopulationSuccessCommitsCheckpoint();
    testFullPopulationCancellationAbortsStagedGeneration();
    testFullPopulationFailureAbortsWithoutCheckpoint();
    testFullPopulationRejectsStaleRequestAndHomeState();
    testProvisionalHomePublicationRetainsCommittedContent();
    testCoordinatorSerializesStartupFullSafetyAndLive();
    return miyoofin_test::finish("library_coordinator");
}
