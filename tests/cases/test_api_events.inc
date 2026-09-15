static void testLibraryItemsBoundedPageHttp()
{
    std::printf("[test] bounded Jellyfin library page HTTP handling\n");
    struct Case { const char *name; int start; int limit; int bodyStart; int total; int count; bool more; };
    const Case cases[] = {
        {"first", 0, 3, 0, 7, 3, true}, {"middle", 3, 3, 3, 7, 3, true},
        {"final partial", 6, 3, 6, 7, 1, false},
        {"exact multiple final", 3, 3, 3, 6, 3, false},
        {"zero", 0, 3, 0, 0, 0, false},
    };
    for (const auto &test : cases) {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(listener >= 0); if (listener < 0) continue;
        int reuse = 1; ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(0);
        CHECK(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        socklen_t size = sizeof(address); CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) == 0); CHECK(::listen(listener, 1) == 0);
        std::string request;
        std::thread server([&] {
            const int client = ::accept(listener, nullptr, nullptr); if (client < 0) return;
            char buffer[4096];
            while (request.find("\r\n\r\n") == std::string::npos) { const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0); if (n <= 0) break; request.append(buffer, static_cast<size_t>(n)); }
            std::string body = "{\"StartIndex\":" + std::to_string(test.bodyStart) + ",\"TotalRecordCount\":" + std::to_string(test.total) + ",\"Items\":[";
            for (int i = 0; i < test.count; ++i) { if (i) body += ','; body += "{\"Id\":\"page-" + std::to_string(test.bodyStart+i) + "\",\"Type\":\"Movie\",\"Name\":\"Title " + std::to_string(test.bodyStart+i) + "\",\"RunTimeTicks\":20000000,\"Etag\":\"etag\"}"; }
            body += "]}";
            const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(client, response.data(), response.size(), 0); ::close(client);
        });
        LibraryItemsPage page; std::string error;
        const bool ok = JellyfinApi::getLibraryItemsPage("http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)), "token", "user", "device", "library-id", "Movie", test.start, test.limit, page, error);
        server.join(); ::close(listener);
        CHECK(ok && error.empty()); CHECK(static_cast<int>(page.items.size()) == test.count); CHECK(page.totalRecordCount == test.total); CHECK(page.hasMore == test.more);
        CHECK(request.find("/Users/user/Items?ParentId=library-id&IncludeItemTypes=Movie") != std::string::npos);
        CHECK(request.find("StartIndex=" + std::to_string(test.start)) != std::string::npos); CHECK(request.find("Limit=" + std::to_string(test.limit)) != std::string::npos);
        CHECK(request.find("SeriesName") != std::string::npos && request.find("Etag") != std::string::npos);
    }
    { // HTTP failure is returned without retry or internal paging.
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0); CHECK(listener >= 0); if (listener < 0) return;
        int reuse = 1; ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)); sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(0); CHECK(::bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0); socklen_t size=sizeof(address); ::getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size); ::listen(listener,1);
        int requests = 0; std::thread server([&]{ int c=::accept(listener,nullptr,nullptr); ++requests; const char *r="HTTP/1.1 500 Error\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno"; (void)::send(c,r,strlen(r),0); ::close(c); });
        LibraryItemsPage page; std::string error; CHECK(!JellyfinApi::getLibraryItemsPage("http://127.0.0.1:"+std::to_string(ntohs(address.sin_port)),"t","u","d","p","Movie",0,3,page,error)); server.join(); ::close(listener); CHECK(requests==1 && !error.empty());
    }
    std::atomic<bool> cancelled{true}; LibraryItemsPage page; std::string error;
    CHECK(!JellyfinApi::getLibraryItemsPage("http://127.0.0.1:1","t","u","d","p","Movie",0,3,page,error,&cancelled)); CHECK(error == "Callback aborted");
    std::printf("[test] bounded Jellyfin library page HTTP handling OK\n");
}

// -------------------------------------------------------------------
// Test: changed catalog metadata is bounded, conservative, and resilient
// -------------------------------------------------------------------
static void testChangedCatalogMetadataQuery()
{
    std::printf("[test] Changed catalog metadata query\n");

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;
    int reuse = 1;
    CHECK(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    CHECK(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) == 0);
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                        &addressSize) == 0);
    CHECK(::listen(listener, 2) == 0);

    const auto itemJson = [](int index) {
        return std::string("{\"Id\":\"changed-")
            + std::to_string(index)
            + "\",\"Type\":\"Movie\",\"Name\":\"Changed Movie "
            + std::to_string(index)
            + "\",\"Overview\":\"Updated overview\","
              "\"ProductionYear\":2026,\"CommunityRating\":8.5,"
              "\"Etag\":\"etag-changed\",\"ImageTags\":"
              "{\"Primary\":\"primary-changed\"}}";
    };
    const auto pageBody = [&](int start, int count) {
        std::string body = R"({"Items":[)";
        for (int index = start; index < start + count; ++index) {
            if (index != start) body += ',';
            body += itemJson(index);
        }
        body += "]}";
        return body;
    };
    const std::string firstBody = pageBody(0, 100);
    const std::string finalBody = pageBody(100, 1);
    std::vector<std::string> requests(2);
    std::thread server([&] {
        for (int page = 0; page < 2; ++page) {
            const int client = ::accept(listener, nullptr, nullptr);
            if (client < 0) return;
            char buffer[4096];
            std::string captured;
            while (captured.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                captured.append(buffer, static_cast<size_t>(received));
            }
            requests[static_cast<size_t>(page)] = captured;
            const std::string &body = page == 0 ? firstBody : finalBody;
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(client, response.data(), response.size(), 0);
            ::close(client);
        }
    });

    const auto port = ntohs(address.sin_port);
    std::vector<MediaItem> items;
    std::string error;
    CHECK(JellyfinApi::getChangedCatalogItems(
        "http://127.0.0.1:" + std::to_string(port), "test-token", "user123",
        "device123", 1700000000123LL, items, error));
    server.join();
    ::close(listener);
    CHECK(error.empty());
    CHECK(items.size() == 101);
    if (items.size() == 101) {
        CHECK_EQ(items[0].id, "changed-0");
        CHECK_EQ(items[0].title, "Changed Movie 0");
        CHECK_EQ(items[0].overview, "Updated overview");
        CHECK_EQ(items[0].imageTags.at("Primary"), "primary-changed");
    }
    CHECK(requests[0].find("IncludeItemTypes=Movie,Series") != std::string::npos);
    CHECK(requests[0].find("SortBy=DateLastSaved") != std::string::npos);
    CHECK(requests[0].find("MinDateLastSaved=2023-11-14T22:13:19.0000000Z")
          != std::string::npos);
    CHECK(requests[0].find("Fields=Overview,Genres,CommunityRating,UserData,"
                            "ImageTags,RunTimeTicks,SeriesName,SeriesId,"
                            "SeasonId,ParentIndexNumber,IndexNumber,Etag")
          != std::string::npos);
    CHECK(requests[1].find("StartIndex=100") != std::string::npos);

    const auto singleResponse = [](const std::string &body, int status,
                                   bool expectedSuccess,
                                   std::string &request) {
        const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(socketFd >= 0);
        if (socketFd < 0) return std::string();
        int socketReuse = 1;
        ::setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &socketReuse,
                     sizeof(socketReuse));
        sockaddr_in socketAddress{};
        socketAddress.sin_family = AF_INET;
        socketAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socketAddress.sin_port = htons(0);
        CHECK(::bind(socketFd, reinterpret_cast<sockaddr *>(&socketAddress),
                     sizeof(socketAddress)) == 0);
        socklen_t socketAddressSize = sizeof(socketAddress);
        ::getsockname(socketFd, reinterpret_cast<sockaddr *>(&socketAddress),
                      &socketAddressSize);
        CHECK(::listen(socketFd, 1) == 0);
        std::thread responseServer([&] {
            const int client = ::accept(socketFd, nullptr, nullptr);
            if (client < 0) return;
            char buffer[1024];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                request.append(buffer, static_cast<size_t>(received));
            }
            const std::string response = "HTTP/1.1 " + std::to_string(status)
                + " Response\r\nContent-Type: application/json\r\n"
                  "Content-Length: " + std::to_string(body.size())
                + "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(client, response.data(), response.size(), 0);
            ::close(client);
        });
        const std::string base = "http://127.0.0.1:"
            + std::to_string(ntohs(socketAddress.sin_port));
        std::vector<MediaItem> responseItems;
        std::string responseError;
        const bool ok = JellyfinApi::getChangedCatalogItems(
            base, "token", "user", "device", 1700000000000LL,
            responseItems, responseError);
        responseServer.join();
        ::close(socketFd);
        CHECK(ok == expectedSuccess);
        return responseError;
    };

    std::string emptyRequest;
    // An empty page is a successful no-op.
    {
        const std::string emptyError = singleResponse(
            R"({"Items":[]})", 200, true, emptyRequest);
        CHECK(emptyError.empty());
    }
    std::string malformedRequest;
    const std::string malformedError = singleResponse(
        R"({"Items":[)" , 200, false, malformedRequest);
    CHECK(malformedError.find("Malformed") != std::string::npos);
    std::string failureRequest;
    const std::string failureError = singleResponse(
        "server unavailable", 503, false, failureRequest);
    CHECK(failureError.find("HTTP 503") != std::string::npos);

    std::atomic<bool> cancelled{true};
    std::vector<MediaItem> cancelledItems;
    std::string cancelledError;
    CHECK(!JellyfinApi::getChangedCatalogItems(
        "http://127.0.0.1:1", "token", "user", "device", 1700000000000LL,
        cancelledItems, cancelledError, &cancelled));
    CHECK(cancelledError == "Callback aborted");
    std::printf("[test] Changed catalog metadata query OK\n");
}

#include "../../src/net/JellyfinLibraryEvents.hpp"

static void testLibraryChangedEventParsing()
{
    std::printf("[test] LibraryChanged event parsing\n");
    JellyfinLibraryChangeBatch batch;
    CHECK(JellyfinLibraryEvents::parseMessage(
        R"({"MessageType":"LibraryChanged","Data":"{\"ItemsAdded\":[\"a\",\"a\"],\"ItemsRemoved\":[\"r\"],\"ItemsUpdated\":[\"u\",\"u\"]}"})",
        batch) == JellyfinLibraryEventParse::Parsed);
    CHECK(batch.itemsAdded.size() == 2 && batch.itemsAdded[0] == "a"
          && batch.itemsAdded[1] == "a");
    CHECK(batch.itemsRemoved.size() == 1 && batch.itemsRemoved[0] == "r");
    CHECK(batch.itemsUpdated.size() == 2 && batch.itemsUpdated[0] == "u"
          && batch.itemsUpdated[1] == "u");

    batch = {};
    CHECK(JellyfinLibraryEvents::parseMessage(
        R"({"MessageType":"LibraryChanged","Data":{"ItemsUpdated":["u2"]}})",
        batch) == JellyfinLibraryEventParse::Parsed);
    CHECK(batch.itemsAdded.empty() && batch.itemsRemoved.empty());
    CHECK(batch.itemsUpdated.size() == 1 && batch.itemsUpdated[0] == "u2");

    batch = {};
    CHECK(JellyfinLibraryEvents::parseMessage(
        R"({"MessageType":"KeepAlive","Data":"{}"})", batch)
        == JellyfinLibraryEventParse::Ignored);
    CHECK(JellyfinLibraryEvents::parseMessage("{bad", batch)
        == JellyfinLibraryEventParse::Malformed);
    CHECK(JellyfinLibraryEvents::parseMessage(
        R"({"MessageType":"LibraryChanged","Data":{"ItemsAdded":42}})", batch)
        == JellyfinLibraryEventParse::Malformed);
    std::string oversized(32769, 'x');
    CHECK(JellyfinLibraryEvents::parseMessage(oversized, batch)
        == JellyfinLibraryEventParse::Oversized);
    std::printf("[test] LibraryChanged event parsing OK\n");
}

// -------------------------------------------------------------------
// Test: UserDataChanged event parsing
// -------------------------------------------------------------------
static void testUserDataChangedEventParsing()
{
    std::printf("[test] UserDataChanged event parsing\n");
    JellyfinLibraryChangeBatch batch;

    // Basic UserDataChanged returns Parsed and sets the flag.
    batch = {};
    CHECK(JellyfinLibraryEvents::parseMessage(
        R"({"MessageType":"UserDataChanged","Data":{"ItemId":"abc"}})",
        batch) == JellyfinLibraryEventParse::Parsed);
    CHECK(batch.userDataChanged);
    CHECK(batch.itemsAdded.empty());
    CHECK(batch.itemsRemoved.empty());
    CHECK(batch.itemsUpdated.empty());
    CHECK(!batch.catchUpRequired);

    // UserDataChanged with no Data object is still Parsed.
    batch = {};
    CHECK(JellyfinLibraryEvents::parseMessage(
        R"({"MessageType":"UserDataChanged"})",
        batch) == JellyfinLibraryEventParse::Parsed);
    CHECK(batch.userDataChanged);

    // LibraryChanged still parses ids unchanged.
    batch = {};
    CHECK(JellyfinLibraryEvents::parseMessage(
        R"({"MessageType":"LibraryChanged","Data":{"ItemsAdded":["x"]}})",
        batch) == JellyfinLibraryEventParse::Parsed);
    CHECK(!batch.userDataChanged);
    CHECK(batch.itemsAdded.size() == 1 && batch.itemsAdded[0] == "x");

    // Unrelated type is still Ignored.
    batch = {};
    CHECK(JellyfinLibraryEvents::parseMessage(
        R"({"MessageType":"KeepAlive"})",
        batch) == JellyfinLibraryEventParse::Ignored);
    CHECK(!batch.userDataChanged);

    std::printf("[test] UserDataChanged event parsing OK\n");
}

// -------------------------------------------------------------------
// Test: queue round-trip for userDataChanged-only batches
// -------------------------------------------------------------------
static void testUserDataChangedQueueRoundTrip()
{
    std::printf("[test] UserDataChanged queue round-trip\n");
    JellyfinLibraryEventQueue queue(8);

    // Push a userDataChanged-only batch (no ids).
    JellyfinLibraryChangeBatch udBatch;
    udBatch.userDataChanged = true;
    CHECK(queue.push(udBatch));

    // Pop returns true with userDataChanged set and no ids.
    JellyfinLibraryChangeBatch result;
    CHECK(queue.pop(result));
    CHECK(result.userDataChanged);
    CHECK(result.itemsAdded.empty());
    CHECK(result.itemsRemoved.empty());
    CHECK(result.itemsUpdated.empty());
    CHECK(!result.catchUpRequired);

    // Second pop returns false (queue is drained).
    CHECK(!queue.pop(result));

    // Overflow/catchUpRequired behavior is unchanged.
    JellyfinLibraryEventQueue smallQueue(2);
    JellyfinLibraryChangeBatch fill1;
    fill1.itemsAdded = {"a", "b"};
    CHECK(smallQueue.push(fill1));
    JellyfinLibraryChangeBatch fill2;
    fill2.itemsAdded = {"extra"};
    CHECK(!smallQueue.push(fill2));
    CHECK(smallQueue.overflowed());

    JellyfinLibraryChangeBatch popped;
    CHECK(smallQueue.pop(popped));
    CHECK(popped.catchUpRequired);
    CHECK(popped.itemsAdded.size() == 2);
    CHECK(!popped.userDataChanged);

    // userDataChanged coalesces with item changes.
    JellyfinLibraryEventQueue mergeQueue(8);
    JellyfinLibraryChangeBatch withItems;
    withItems.itemsAdded = {"id1"};
    mergeQueue.push(withItems);
    JellyfinLibraryChangeBatch udOnly;
    udOnly.userDataChanged = true;
    mergeQueue.push(udOnly);
    JellyfinLibraryChangeBatch merged;
    CHECK(mergeQueue.pop(merged));
    CHECK(merged.userDataChanged);
    CHECK(merged.itemsAdded.size() == 1 && merged.itemsAdded[0] == "id1");
    CHECK(!mergeQueue.pop(merged));

    std::printf("[test] UserDataChanged queue round-trip OK\n");
}

// -------------------------------------------------------------------
// Test: ChangedHierarchy uses only the affected-series projection
// -------------------------------------------------------------------
static void testChangedHierarchyLightweightProjection()
{
    std::printf("[test] ChangedHierarchy lightweight projection\n");
    testLibraryChangedEventParsing();
    testUserDataChangedEventParsing();
    testUserDataChangedQueueRoundTrip();
    testChangedCatalogMetadataQuery();

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;

    int reuse = 1;
    CHECK(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    CHECK(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) == 0);
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                        &addressSize) == 0);
    CHECK(::listen(listener, 2) == 0);

    std::vector<std::string> requests(2);
    const auto itemJson = [](int index) {
        if (index == 0) {
            return std::string(R"({"Id":"series-0","Type":"Series",
                "Name":"ignored show name","Overview":"ignored overview",
                "UserData":{"Played":true},"ImageTags":{"Primary":"ignored-tag"}})");
        }
        if (index == 1) {
            return std::string(R"({"Id":"season-1","Type":"Season","SeriesId":"series-0",
                "Name":"ignored season name","Overview":"ignored season overview",
                "UserData":{"Played":true},"ImageTags":{"Primary":"ignored-season-tag"},
                "SeasonId":"ignored-season","RunTimeTicks":123456789})");
        }
        if (index == 2) {
            return std::string(R"({"Id":"episode-2","Type":"Episode","SeriesId":"series-0",
                "Name":"ignored episode name","Overview":"ignored episode overview",
                "UserData":{"Played":true},"ImageTags":{"Primary":"ignored-episode-tag"},
                "SeasonId":"ignored-season","RunTimeTicks":987654321})");
        }
        return "{\"Id\":\"episode-" + std::to_string(index) +
               "\",\"Type\":\"Episode\",\"SeriesId\":\"series-0\"}";
    };
    const auto pageBody = [&](int start, int count) {
        std::string body = R"({"Items":[)";
        for (int index = start; index < start + count; ++index) {
            if (index != start) body += ',';
            body += itemJson(index);
        }
        body += "]}";
        return body;
    };
    const std::string firstBody = pageBody(0, 100);
    const std::string finalBody = pageBody(100, 3);
    std::thread server([&]() {
        for (int page = 0; page < 2; ++page) {
            const int client = ::accept(listener, nullptr, nullptr);
            if (client < 0) return;
            char buffer[4096];
            std::string captured;
            while (captured.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                captured.append(buffer, static_cast<size_t>(received));
            }
            requests[static_cast<size_t>(page)] = captured;
            const std::string &body = page == 0 ? firstBody : finalBody;
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(client, response.data(), response.size(), 0);
            ::close(client);
        }
    });

    const auto port = ntohs(address.sin_port);
    std::vector<MediaItem> items;
    std::string error;
    const bool ok = JellyfinApi::getChangedHierarchyItems(
        "http://127.0.0.1:" + std::to_string(port), "test-token", "user123",
        "device123", 1700000000123LL, items, error);
    server.join();
    ::close(listener);
    CHECK(ok);
    CHECK(error.empty());
    CHECK(items.size() == 103);
    if (items.size() == 103) {
        for (int index = 0; index < 103; ++index) {
            const std::string expectedId = index == 0 ? "series-0" :
                index == 1 ? "season-1" :
                index == 2 ? "episode-2" : "episode-" + std::to_string(index);
            CHECK_EQ(items[static_cast<size_t>(index)].id, expectedId);
        }

        // These are the exact inputs used by HomeScreen's affected-series
        // reduction: Series uses its own ID, while Season/Episode use SeriesId.
        CHECK_EQ(items[0].id, "series-0");
        CHECK_EQ(items[0].type, "show");
        const auto affectedSeriesId = [](const MediaItem &item) {
            return item.type == "show" ? item.id : item.seriesId;
        };
        CHECK_EQ(affectedSeriesId(items[0]), "series-0");
        CHECK(items[0].title.empty());
        CHECK(items[0].overview.empty());
        CHECK(items[0].imageTags.empty());
        CHECK(!items[0].played);

        CHECK_EQ(items[1].id, "season-1");
        CHECK_EQ(items[1].type, "season");
        CHECK_EQ(items[1].seriesId, "series-0");
        CHECK_EQ(affectedSeriesId(items[1]), "series-0");
        CHECK(items[1].title.empty());
        CHECK(items[1].overview.empty());
        CHECK(items[1].imageTags.empty());
        CHECK(!items[1].played);
        CHECK(items[1].seasonId.empty());
        CHECK(items[1].runTimeTicks == 0);

        CHECK_EQ(items[2].id, "episode-2");
        CHECK_EQ(items[2].type, "episode");
        CHECK_EQ(items[2].seriesId, "series-0");
        CHECK_EQ(affectedSeriesId(items[2]), "series-0");
        CHECK(items[2].title.empty());
        CHECK(items[2].overview.empty());
        CHECK(items[2].imageTags.empty());
        CHECK(!items[2].played);
        CHECK(items[2].seasonId.empty());
        CHECK(items[2].runTimeTicks == 0);
    }
    CHECK(requests[0].find("IncludeItemTypes=Series,Season,Episode") != std::string::npos);
    CHECK(requests[0].find("SortBy=DateLastSaved") != std::string::npos);
    CHECK(requests[0].find("SortOrder=Ascending") != std::string::npos);
    CHECK(requests[0].find("MinDateLastSaved=2023-11-14T22:13:20.0000000Z") !=
          std::string::npos);
    CHECK(requests[0].find("StartIndex=0") != std::string::npos);
    CHECK(requests[0].find("Limit=100") != std::string::npos);
    CHECK(requests[0].find("Fields=SeriesId") != std::string::npos);
    CHECK(requests[0].find("Fields=Overview") == std::string::npos);
    CHECK(requests[0].find("UserData") == std::string::npos);
    CHECK(requests[0].find("ImageTags") == std::string::npos);
    CHECK(requests[1].find("StartIndex=100") != std::string::npos);
    CHECK(requests[1].find("Limit=100") != std::string::npos);
    CHECK(requests[1].find("Fields=SeriesId") != std::string::npos);
    CHECK(requests[1].find("Fields=Overview") == std::string::npos);
    CHECK(requests[1].find("UserData") == std::string::npos);
    CHECK(requests[1].find("ImageTags") == std::string::npos);
    std::printf("[test] ChangedHierarchy lightweight projection OK\n");
}

// -------------------------------------------------------------------
// B5a Test: buildImageUrl — Primary image URL
// -------------------------------------------------------------------

