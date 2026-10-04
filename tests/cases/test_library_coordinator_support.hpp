#ifndef MIYOOFIN_TEST_LIBRARY_COORDINATOR_SUPPORT_HPP
#define MIYOOFIN_TEST_LIBRARY_COORDINATOR_SUPPORT_HPP

// Shared fixtures for the test_library_coordinator* groups (loopback Jellyfin stand-in, scoped
// catalog fixtures, waits). Included by every group; helpers are inline so a group that does not
// use one does not warn about it.

#include "../test_support.hpp"
#include "../../src/library/LibraryCoordinator.hpp"
#include "../../src/net/JellyfinLibraryEvents.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace miyoofin;

namespace {

inline library::LibraryCoordinator makeCoordinator()
{
    Session session;
    session.manualOfflineMode = true;
    return library::LibraryCoordinator(session, std::make_shared<CatalogDb>(), 0);
}

struct CoordinatorTestScope
{
    std::string url;
    std::string user;
    std::string scope;
    std::uint64_t epoch = 0;
};

inline CoordinatorTestScope coordinatorTestScope(const char* name)
{
    CoordinatorTestScope scope;
    scope.url = std::string("https://coordinator-") + name + "-" +
                std::to_string(static_cast<long long>(::getpid())) + ".example";
    scope.user = std::string("coordinator-") + name + "-user";
    scope.scope = LibraryCache::scopeKey(scope.url, scope.user);
    const std::string base = "cache/library/" + scope.scope + "/catalog.sqlite3";
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

inline void removeCoordinatorTestScope(const CoordinatorTestScope& scope)
{
    const std::string libraryDirectory = "cache/library/" + scope.scope;
    const std::string base = libraryDirectory + "/catalog.sqlite3";
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
    ::rmdir(libraryDirectory.c_str());
}

inline std::int64_t coordinatorTestNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::unique_ptr<library::LibraryCoordinator>
makeMaintenanceCoordinator(const char* name, std::shared_ptr<CatalogDb>& db,
                           CoordinatorTestScope& scope, bool manualOffline = false,
                           const std::string& serverUrl = "http://127.0.0.1:1")
{
    scope = coordinatorTestScope(name);
    db = std::make_shared<CatalogDb>();
    scope.epoch = db->configureScope(scope.url, scope.user);
    CHECK(scope.epoch != 0);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(10)));

    Session session;
    session.serverUrl = serverUrl;
    session.userId = scope.user;
    session.manualOfflineMode = manualOffline;
    return std::make_unique<library::LibraryCoordinator>(session, db, scope.epoch);
}

inline void seedMaintenanceCheckpoint(const std::shared_ptr<CatalogDb>& db, std::uint64_t epoch,
                                      std::int64_t timestamp)
{
    const auto seeded = db->writeSyncState(timestamp, timestamp, 1, {0, epoch, {}}).get();
    CHECK(seeded.success);
}

inline library::StartupSyncResult takeStartupResult(library::LibraryCoordinator& coordinator)
{
    library::StartupSyncResult result;
    const auto status = coordinator.waitStartupSyncResult(result);
    if (status != library::WaitStatus::Ready) {
        bool took = false;
        for (int i = 0; i < 500 && !took; ++i) {
            took = coordinator.takeStartupSyncResult(result);
            if (!took)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(took);
    }
    return result;
}

inline int coordinatorTestListener()
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
    CHECK(::listen(listener, 4) == 0);
    return listener;
}

inline std::string coordinatorTestUrl(int listener)
{
    sockaddr_in address{};
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize) == 0);
    return "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
}

inline int coordinatorAccept(int listener)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeval timeout{5, 0};
    if (::select(listener + 1, &readable, nullptr, nullptr, &timeout) <= 0)
        return -1;
    return ::accept(listener, nullptr, nullptr);
}

inline void coordinatorReadRequest(int client)
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

inline void coordinatorSendJson(int client, const std::string& body, int status = 200)
{
    const char* statusText = status == 200 ? "OK" : "Internal Server Error";
    const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + statusText +
                                 "\r\nContent-Type: application/json\r\n" +
                                 "Content-Length: " + std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + body;
    (void)::send(client, response.data(), response.size(), 0);
}

inline bool takeFullUpdate(library::LibraryCoordinator& coordinator, std::uint64_t request,
                           library::FullPopulationUpdate& terminal, bool& sawPage)
{
    for (;;) {
        library::FullPopulationUpdate update;
        if (coordinator.waitFullPopulationUpdate(request, update) != library::WaitStatus::Ready) {
            for (int i = 0; i < 500; ++i) {
                if (coordinator.takeFullPopulationUpdate(request, update))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!update.terminal && !update.pageValid)
                return false;
        }
        sawPage = sawPage || update.pageValid;
        if (update.terminal) {
            terminal = std::move(update);
            return true;
        }
    }
}

struct HomeRailResponse
{
    std::string body;
    int status = 200;
};

inline bool takeHomeRailResult(library::LibraryCoordinator& coordinator, std::uint64_t request,
                               library::HomeRailResult& result)
{
    return coordinator.waitHomeRailResult(request, result) == library::WaitStatus::Ready;
}

inline void serveHomeRailResponses(int listener, const std::vector<HomeRailResponse>& responses)
{
    for (const auto& response : responses) {
        const int client = coordinatorAccept(listener);
        if (client < 0)
            return;
        coordinatorReadRequest(client);
        coordinatorSendJson(client, response.body, response.status);
        ::close(client);
    }
}

// First line of the captured UI diagnostics log that contains `marker` and every field.
inline std::string diagnosticLine(const std::string& diagnostics, const std::string& marker,
                                  const std::vector<std::string>& fields = {})
{
    std::size_t search = 0;
    while (search < diagnostics.size()) {
        const auto begin = diagnostics.find(marker, search);
        if (begin == std::string::npos)
            return std::string();
        const auto end = diagnostics.find('\n', begin);
        const auto line =
            diagnostics.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        bool matches = true;
        for (const auto& field : fields)
            matches = matches && line.find(field) != std::string::npos;
        if (matches)
            return line;
        if (end == std::string::npos)
            return std::string();
        search = end + 1;
    }
    return std::string();
}

} // namespace

#endif
