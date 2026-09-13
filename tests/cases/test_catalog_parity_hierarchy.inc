static void testCatalogDbMediaPagePreservesViewMembership()
{
    std::printf("[test] CatalogDb media pages preserve TV view membership\n");
    const std::string url = "https://sqlite-media-membership.example";
    const std::string user = "media-membership-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    MediaItem anime = catalogParityItem("__task33_anime_series__", "show",
                                        "Anime Series");
    anime.genres = {"Animation"};
    LibrarySnapshot snapshot;
    snapshot.shows = {
        {"shows", "Shows", "tvshows", {anime}},
        {"anime-view", "Anime", "tvshows", {anime}},
    };

    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get().success);

    const auto page = db.readMediaPage(
        "show", -1, 8, {}, {0, epoch, {}}).get();
    CHECK(page.success && page.items.size() == 1);
    const auto memberships = page.membershipsByItem.find(anime.id);
    CHECK(memberships != page.membershipsByItem.end());
    if (memberships != page.membershipsByItem.end()) {
        CHECK(memberships->second.size() == 2);
        const bool hasAnimeView = std::any_of(
            memberships->second.begin(), memberships->second.end(),
            [](const CatalogDbMediaPageMembership &membership) {
                return membership.viewName == "Anime"
                    && membership.collectionType == "tvshows";
            });
        CHECK(hasAnimeView);
    }
    removeCatalogMigrationTestPaths(paths);

    const std::string multiViewUrl =
        "https://sqlite-multiple-tv-views-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const auto multiViewPaths = catalogMigrationTestPaths(multiViewUrl,
                                                          "media-membership-user");
    removeCatalogMigrationTestPaths(multiViewPaths);
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, multiViewUrl,
                                       "media-membership-user"));
        MediaItem shared = catalogParityItem(
            "__task23_shared_series__", "show", "Shared Series");
        MediaItem firstOnly = catalogParityItem(
            "__task23_first_only_series__", "show", "First Library Series");
        MediaItem secondOnly = catalogParityItem(
            "__task23_second_only_series__", "show", "Second Library Series");

        CHECK(db.beginTopLevelSync(40).get().success);
        CatalogDbMediaPageWrite firstView;
        firstView.items = {shared, firstOnly};
        firstView.viewId = "task23-first-tv-view";
        firstView.viewName = "Task 23 First TV View";
        firstView.collectionType = "tvshows";
        firstView.viewOrdinal = 0;
        firstView.syncGeneration = 40;
        CHECK(db.upsertMediaPage(firstView).get().success);
        CatalogDbMediaPageWrite secondView = firstView;
        secondView.items = {shared, secondOnly};
        secondView.viewId = "task23-second-tv-view";
        secondView.viewName = "Task 23 Second TV View";
        secondView.viewOrdinal = 1;
        CHECK(db.upsertMediaPage(secondView).get().success);
        CHECK(db.finalizeTopLevelSync(40).get().success);

        const auto page = db.readMediaPage("show", -1, 8).get();
        CHECK(page.success && page.items.size() == 3 && !page.hasMore);
        std::set<std::string> ids;
        for (const auto &item : page.items) ids.insert(item.id);
        CHECK(ids.size() == 3 && ids.count(shared.id) == 1
              && ids.count(firstOnly.id) == 1 && ids.count(secondOnly.id) == 1);
        CHECK(page.membershipsByItem.at(shared.id).size() == 2);
        CHECK(page.membershipsByItem.at(firstOnly.id).size() == 1);
        CHECK(page.membershipsByItem.at(secondOnly.id).size() == 1);
        const auto &sharedMemberships = page.membershipsByItem.at(shared.id);
        CHECK(sharedMemberships[0].viewId == firstView.viewId
              && sharedMemberships[1].viewId == secondView.viewId);
        CHECK(page.membershipsByItem.at(secondOnly.id)[0].viewId
              == secondView.viewId);
    }
    removeCatalogMigrationTestPaths(multiViewPaths);

    const std::string animeUrl =
        "https://sqlite-anime-membership-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string animeUser = "anime-membership-user";
    const auto animePaths = catalogMigrationTestPaths(animeUrl, animeUser);
    removeCatalogMigrationTestPaths(animePaths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch = db->configureScope(animeUrl, animeUser);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = animeUrl;
        session.userId = animeUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);

        MediaItem animeByView = catalogParityItem(
            "__task24_anime_by_view__", "show", "Anime By View");
        animeByView.genres = {"Action"};
        animeByView.imageTags["Primary"] = "task24-view-primary";
        MediaItem animeByGenre = catalogParityItem(
            "__task24_anime_by_genre__", "show", "Anime By Genre");
        animeByGenre.genres = {"Anime"};
        animeByGenre.imageTags["Primary"] = "task24-genre-primary";
        MediaItem control = catalogParityItem(
            "__task24_control_show__", "show", "Control Show");
        control.genres = {"Drama"};
        control.imageTags["Primary"] = "task24-control-primary";
        MediaItem shared = catalogParityItem(
            "__task24_normal_and_anime__", "show", "Normal And Anime");
        shared.genres = {"Drama"};
        shared.imageTags["Primary"] = "task24-shared-primary";

        CHECK(sync->begin(50).get().success);
        CatalogDbMediaPageWrite shows;
        shows.items = {animeByGenre, control, shared};
        shows.viewId = "task24-shows";
        shows.viewName = "Shows";
        shows.collectionType = "tvshows";
        shows.viewOrdinal = 0;
        shows.syncGeneration = 50;
        CHECK(sync->stage(shows).get().success);
        CatalogDbMediaPageWrite anime = shows;
        anime.items = {animeByView, shared};
        anime.viewId = "task24-anime";
        anime.viewName = "Anime Library";
        anime.viewOrdinal = 1;
        CHECK(sync->stage(anime).get().success);
        CHECK(sync->finalize(50).get().success);

        library::LibraryQuery query(db, epoch);
        const auto page = query.shows(-1, 8).get();
        CHECK(page.success && page.items.size() == 4 && !page.hasMore);
        std::map<std::string, MediaItem> items;
        for (const auto &item : page.items) items[item.id] = item;
        CHECK(items.size() == 4);
        CHECK(items.at(animeByView.id).imageTags.at("Primary")
              == "task24-view-primary");
        CHECK(items.at(animeByGenre.id).imageTags.at("Primary")
              == "task24-genre-primary");
        CHECK(items.at(control.id).imageTags.at("Primary")
              == "task24-control-primary");
        CHECK(items.at(shared.id).imageTags.at("Primary")
              == "task24-shared-primary");

        const auto animeFor = [&](const MediaItem &item) {
            const auto &memberships = page.membershipsByItem.at(item.id);
            return std::any_of(
                memberships.begin(), memberships.end(),
                [&](const CatalogDbMediaPageMembership &membership) {
                    return isAnimeSeries(membership.viewName, item);
                });
        };
        CHECK(animeFor(items.at(animeByView.id)));
        CHECK(animeFor(items.at(animeByGenre.id)));
        CHECK(!animeFor(items.at(control.id)));
        CHECK(animeFor(items.at(shared.id)));
        CHECK(page.membershipsByItem.at(shared.id).size() == 2);

        const auto animePage = query.anime(-1, 8).get();
        CHECK(animePage.success && animePage.items.size() == 3
              && !animePage.hasMore);
        std::set<std::string> animeIds;
        for (const auto &item : animePage.items) animeIds.insert(item.id);
        CHECK(animeIds.size() == 3
              && animeIds.count(animeByView.id) == 1
              && animeIds.count(animeByGenre.id) == 1
              && animeIds.count(shared.id) == 1
              && animeIds.count(control.id) == 0);
    }
    removeCatalogMigrationTestPaths(animePaths);

    const std::string playlistUrl =
        "https://sqlite-playlist-interleave-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string playlistUser = "playlist-interleave-user";
    const auto playlistPaths =
        catalogMigrationTestPaths(playlistUrl, playlistUser);
    removeCatalogMigrationTestPaths(playlistPaths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch = db->configureScope(playlistUrl, playlistUser);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = playlistUrl;
        session.userId = playlistUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);
        MediaItem movie = catalogParityItem(
            "__task25_supported_movie__", "movie", "Supported Movie");
        MediaItem playlistOnly = catalogParityItem(
            "__task25_playlist_only__", "movie", "Playlist Only Item");
        MediaItem show = catalogParityItem(
            "__task25_later_tv_show__", "show", "Later TV Show");

        CHECK(sync->begin(60).get().success);
        CatalogDbMediaPageWrite movies;
        movies.items = {movie};
        movies.viewId = "task25-movies";
        movies.viewName = "Movies";
        movies.collectionType = "movies";
        movies.viewOrdinal = 0;
        movies.syncGeneration = 60;
        CHECK(sync->stage(movies).get().success);
        CatalogDbMediaPageWrite playlist = movies;
        playlist.items = {movie, playlistOnly};
        playlist.viewId = "task25-playlist";
        playlist.viewName = "Watch Playlist";
        playlist.collectionType = "playlists";
        playlist.viewOrdinal = 1;
        CHECK(sync->stage(playlist).get().success);
        CatalogDbMediaPageWrite tv = movies;
        tv.items = {show};
        tv.viewId = "task25-tv";
        tv.viewName = "TV Shows";
        tv.collectionType = "tvshows";
        tv.viewOrdinal = 2;
        CHECK(sync->stage(tv).get().success);
        CHECK(sync->finalize(60).get().success);

        library::LibraryQuery query(db, epoch);
        const auto moviePage = query.movies(-1, 8).get();
        const auto showPage = query.shows(-1, 8).get();
        CHECK(moviePage.success && moviePage.items.size() == 1
              && moviePage.items[0].id == movie.id);
        CHECK(showPage.success && showPage.items.size() == 1
              && showPage.items[0].id == show.id);
        const auto &movieMemberships = moviePage.membershipsByItem.at(movie.id);
        const bool hasSupportedMovieMembership = std::any_of(
            movieMemberships.begin(), movieMemberships.end(),
            [&](const CatalogDbMediaPageMembership &membership) {
                return membership.viewId == movies.viewId
                    && membership.collectionType == "movies";
            });
        CHECK(hasSupportedMovieMembership);
        CHECK(moviePage.membershipsByItem.count(playlistOnly.id) == 0);
        CHECK(showPage.membershipsByItem.count(playlistOnly.id) == 0);
    }
    removeCatalogMigrationTestPaths(playlistPaths);
    std::printf("[test] CatalogDb media pages preserve TV view membership OK\n");
}

void testLibraryCacheHomeParityHarness()
{
    const std::string syncSource =
        readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(syncSource.find("m_metadataActive.store(true)")
          != std::string::npos);
    CHECK(syncSource.find("m_metadataCompleted.fetch_add")
          != std::string::npos);
    const std::string episodeNavigationSource =
        readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    CHECK(episodeNavigationSource.find("season.seriesId = series.id;")
          != std::string::npos);
    const std::string hierarchySource =
        readTestBytes("src/ui/screens/HomeScreenHierarchy.cpp");
    CHECK(hierarchySource.find("getSeasons(") == std::string::npos);
    CHECK(hierarchySource.find("getEpisodes(") == std::string::npos);
    CHECK(hierarchySource.find("submitCatalogHierarchy") == std::string::npos);
    CHECK(hierarchySource.find("m_libraryQuery->seasons")
          != std::string::npos);
    CHECK(hierarchySource.find("m_libraryQuery->episodes")
          != std::string::npos);
    CHECK(hierarchySource.find("m_librarySync->refreshSeasons")
          != std::string::npos);
    CHECK(hierarchySource.find("m_librarySync->refreshEpisodes")
          != std::string::npos);
    const std::string lifecycleSource =
        readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(lifecycleSource.find("submitCatalogHierarchy")
          == std::string::npos);
    CHECK(lifecycleSource.find("m_catalogDb->stageSeriesHierarchy")
          == std::string::npos);

    LibrarySnapshot snapshot;
    MediaItem a = catalogParityItem("__task30_a__", "movie", "The apple");
    MediaItem b = catalogParityItem("__task30_b__", "movie", "APPLE");
    MediaItem c = catalogParityItem("__task30_c__", "movie", "Apple");
    snapshot.movies = {{"v1", "Primary Movies", "movies", {a, b}},
                       {"v2", "Secondary Movies", "movies", {c}}};
    snapshot.shows = {{"shows", "Shows", "tvshows", {a}}};
    snapshot.continueWatching = {c, a};
    snapshot.recentlyAdded = {b, c};
    const auto tabs = tabsFromSnapshot(snapshot);
    CHECK(tabs.size() == 5 && tabs[0].rows.size() == 2);
    CHECK(tabs[0].rows[0].label == "Continue Watching"
          && tabs[0].rows[0].items[0].id == c.id);
    CHECK(tabs[0].rows[1].label == "Recently Added"
          && tabs[0].rows[1].items[1].id == c.id);
    CHECK(tabs[1].rows[0].items.size() == 3);
    CHECK(tabs[1].rows[0].items[0].id == b.id
          && tabs[1].rows[0].items[1].id == c.id
          && tabs[1].rows[0].items[2].id == a.id);
    CHECK(tabs[2].rows.size() == 1 && tabs[2].rows[0].label == "Shows"
          && tabs[2].rows[0].items[0].id == a.id);
    CHECK(offlineTabsFromSnapshot(snapshot).size() == 4);
}

static void testHomeSkipsUnsupportedLibraryViews()
{
    std::printf("[test] Home skips unsupported library views before persistence\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(source.find("supportedLibraryView") != std::string::npos);
    CHECK(source.find("view.collectionType == \"movies\"") != std::string::npos);
    CHECK(source.find("view.collectionType == \"tvshows\"") != std::string::npos);
    CHECK(source.find("library_view_skipped unsupported_collection") != std::string::npos);
    CHECK(source.find("views.erase(std::remove_if") != std::string::npos);
    CHECK(source.find("getLibraryItemsPage") != std::string::npos);
    CHECK(source.find("m_librarySync->stage") != std::string::npos);
    CHECK(source.find("beginTopLevelSync") == std::string::npos);
    CHECK(source.find("finalizeTopLevelSync") == std::string::npos);
    CHECK(source.find("abortTopLevelSync") == std::string::npos);
    std::printf("[test] Home skips unsupported library views before persistence OK\n");
}

static void testHomeOptionalRailFailuresDoNotStopCatalogPopulation()
{
    std::printf("[test] Home optional rail failures are isolated\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(source.find("bool optionalRailFailed") != std::string::npos);
    CHECK(source.find("bool catalogRefreshFailed") != std::string::npos);
    CHECK(source.find("optionalRailFailed=true") != std::string::npos);
    CHECK(source.find("catalogRefreshFailed = true") != std::string::npos);
    CHECK(source.find("if (catalogRefreshFailed) break;") != std::string::npos);
    CHECK(source.find("completeTelemetry(catalogRefreshFailed ? Outcome::Failure")
          != std::string::npos);
    CHECK(source.find("if (optionalRailFailed) break") == std::string::npos);

    const int resumeListener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(resumeListener >= 0);
    if (resumeListener >= 0) {
        int reuse = 1;
        ::setsockopt(resumeListener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        CHECK(::bind(resumeListener,
                     reinterpret_cast<sockaddr *>(&address),
                     sizeof(address)) == 0);
        socklen_t addressSize = sizeof(address);
        CHECK(::getsockname(resumeListener,
                            reinterpret_cast<sockaddr *>(&address),
                            &addressSize) == 0);
        CHECK(::listen(resumeListener, 1) == 0);
        std::thread resumeServer([&] {
            const int client = ::accept(resumeListener, nullptr, nullptr);
            if (client >= 0) {
                char buffer[2048];
                (void)::recv(client, buffer, sizeof(buffer), 0);
                ::close(client);
            }
        });
        std::vector<MediaItem> resumeItems;
        std::string resumeError;
        const bool resumeOk = JellyfinApi::getResumeItems(
            "http://127.0.0.1:" +
                std::to_string(ntohs(address.sin_port)),
            "token", "user", "device", 12, resumeItems, resumeError);
        resumeServer.join();
        ::close(resumeListener);
        CHECK(!resumeOk && !resumeError.empty());
    }

    const int latestListener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(latestListener >= 0);
    if (latestListener >= 0) {
        int reuse = 1;
        ::setsockopt(latestListener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        CHECK(::bind(latestListener,
                     reinterpret_cast<sockaddr *>(&address),
                     sizeof(address)) == 0);
        socklen_t addressSize = sizeof(address);
        CHECK(::getsockname(latestListener,
                            reinterpret_cast<sockaddr *>(&address),
                            &addressSize) == 0);
        CHECK(::listen(latestListener, 1) == 0);
        const std::string body =
            R"({"Items":[{"Id":"__task26_recent__","Type":"Movie","Name":"Recent Item"}]})";
        std::thread latestServer([&] {
            const int client = ::accept(latestListener, nullptr, nullptr);
            if (client >= 0) {
                char buffer[2048];
                (void)::recv(client, buffer, sizeof(buffer), 0);
                const std::string response =
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
                (void)::send(client, response.data(), response.size(), 0);
                ::close(client);
            }
        });
        std::vector<MediaItem> latestItems;
        std::string latestError;
        const bool latestOk = JellyfinApi::getLatestItems(
            "http://127.0.0.1:" +
                std::to_string(ntohs(address.sin_port)),
            "token", "user", "device", 16, latestItems, latestError);
        latestServer.join();
        ::close(latestListener);
        CHECK(latestOk && latestError.empty() && latestItems.size() == 1
              && latestItems[0].id == "__task26_recent__");
    }

    const std::string catalogUrl =
        "https://sqlite-continue-watching-failure-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string catalogUser = "continue-watching-failure-user";
    const auto paths = catalogMigrationTestPaths(catalogUrl, catalogUser);
    removeCatalogMigrationTestPaths(paths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch = db->configureScope(catalogUrl, catalogUser);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = catalogUrl;
        session.userId = catalogUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);
        CHECK(sync->begin(70).get().success);
        CatalogDbMediaPageWrite movies;
        movies.items = {catalogParityItem(
            "__task26_catalog_movie__", "movie", "Catalog Movie")};
        movies.viewId = "task26-movies";
        movies.viewName = "Movies";
        movies.collectionType = "movies";
        movies.syncGeneration = 70;
        CHECK(sync->stage(movies).get().success);
        CatalogDbMediaPageWrite shows = movies;
        shows.items = {catalogParityItem(
            "__task26_catalog_show__", "show", "Catalog Show")};
        shows.viewId = "task26-shows";
        shows.viewName = "Shows";
        shows.collectionType = "tvshows";
        CHECK(sync->stage(shows).get().success);
        CHECK(sync->finalize(70).get().success);

        library::LibraryQuery query(db, epoch);
        const auto moviePage = query.movies(-1, 8).get();
        const auto showPage = query.shows(-1, 8).get();
        CHECK(moviePage.success && moviePage.items.size() == 1
              && moviePage.items[0].id == "__task26_catalog_movie__");
        CHECK(showPage.success && showPage.items.size() == 1
              && showPage.items[0].id == "__task26_catalog_show__");
    }
    removeCatalogMigrationTestPaths(paths);

    const auto runRailFixture = [](bool latest, bool respond,
                                   std::vector<MediaItem> &items,
                                   std::string &error) {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) return false;
        int reuse = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        if (::bind(listener, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) != 0) {
            ::close(listener);
            return false;
        }
        socklen_t addressSize = sizeof(address);
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                          &addressSize) != 0
            || ::listen(listener, 1) != 0) {
            ::close(listener);
            return false;
        }
        const std::string body = latest
            ? R"({"Items":[{"Id":"__task27_latest__","Type":"Movie","Name":"Latest Item"}]})"
            : R"({"Items":[{"Id":"__task27_resume__","Type":"Movie","Name":"Resume Item"}]})";
        std::thread server([&] {
            const int client = ::accept(listener, nullptr, nullptr);
            if (client >= 0) {
                char buffer[2048];
                (void)::recv(client, buffer, sizeof(buffer), 0);
                if (respond) {
                    const std::string response =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body;
                    (void)::send(client, response.data(), response.size(), 0);
                }
                ::close(client);
            }
        });
        const std::string url = "http://127.0.0.1:" +
            std::to_string(ntohs(address.sin_port));
        const bool ok = latest
            ? JellyfinApi::getLatestItems(url, "token", "user", "device",
                                           16, items, error)
            : JellyfinApi::getResumeItems(url, "token", "user", "device",
                                          12, items, error);
        server.join();
        ::close(listener);
        return ok;
    };
    std::vector<MediaItem> resumed;
    std::string resumeError;
    CHECK(runRailFixture(false, true, resumed, resumeError)
          && resumeError.empty() && resumed.size() == 1
          && resumed[0].id == "__task27_resume__");
    std::vector<MediaItem> latest;
    std::string latestError;
    CHECK(!runRailFixture(true, false, latest, latestError)
          && !latestError.empty());

    const std::string recentCatalogUrl =
        "https://sqlite-recently-added-failure-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string recentCatalogUser = "recently-added-failure-user";
    const auto recentPaths =
        catalogMigrationTestPaths(recentCatalogUrl, recentCatalogUser);
    removeCatalogMigrationTestPaths(recentPaths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch =
            db->configureScope(recentCatalogUrl, recentCatalogUser);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = recentCatalogUrl;
        session.userId = recentCatalogUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);
        CHECK(sync->begin(80).get().success);
        CatalogDbMediaPageWrite movies;
        movies.items = {catalogParityItem(
            "__task27_catalog_movie__", "movie", "Catalog Movie")};
        movies.viewId = "task27-movies";
        movies.viewName = "Movies";
        movies.collectionType = "movies";
        movies.syncGeneration = 80;
        CHECK(sync->stage(movies).get().success);
        CatalogDbMediaPageWrite shows = movies;
        shows.items = {catalogParityItem(
            "__task27_catalog_show__", "show", "Catalog Show")};
        shows.viewId = "task27-shows";
        shows.viewName = "Shows";
        shows.collectionType = "tvshows";
        CHECK(sync->stage(shows).get().success);
        CHECK(sync->finalize(80).get().success);

        library::LibraryQuery query(db, epoch);
        const auto moviePage = query.movies(-1, 8).get();
        const auto showPage = query.shows(-1, 8).get();
        CHECK(moviePage.success && moviePage.items.size() == 1
              && moviePage.items[0].id == "__task27_catalog_movie__");
        CHECK(showPage.success && showPage.items.size() == 1
              && showPage.items[0].id == "__task27_catalog_show__");
    }
    removeCatalogMigrationTestPaths(recentPaths);
    std::printf("[test] Home optional rail failures are isolated OK\n");
}

static void testCatalogScopeConfiguredBeforeHomePopulation()
{
    std::printf("[test] Catalog scope precedes Home population\n");
    const std::string appSource = readTestBytes("src/app/App.cpp");
    const auto configure = appSource.find("configureCatalogScopeForSession();");
    const auto home = appSource.find("goToHome();", configure);
    CHECK(configure != std::string::npos && home != std::string::npos);
    CHECK(appSource.find("catalog_scope_deferred_until_media_navigation")
          == std::string::npos);
    std::printf("[test] Catalog scope precedes Home population OK\n");
}

static void testHomePublishesAfterFirstBoundedPage()
{
    std::printf("[test] Home publishes after first bounded page\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string lifecycle = readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(source.find("m_fetchReady") != std::string::npos);
    CHECK(source.find("m_fetchComplete") != std::string::npos);
    CHECK(source.find("first bounded page") != std::string::npos);
    CHECK(source.find("m_fetchReady.store(true)") != std::string::npos);
    CHECK(lifecycle.find("if (m_fetchReady.load())") != std::string::npos);
    CHECK(source.find("m_fetchDone.store(true);\n                    }")
          == std::string::npos);
    CHECK(source.find("if (!movies.items.empty() || !shows.items.empty())")
          != std::string::npos);
    CHECK(source.find("if (movies.success || shows.success)")
          == std::string::npos);
    CHECK(source.find("m_fetchError = viewsErr.empty()") != std::string::npos);
    CHECK(source.find("m_fetchError = pageErr.empty()") != std::string::npos);
    CHECK(source.find("if (views.empty())") != std::string::npos);
    std::printf("[test] Home publishes after first bounded page OK\n");
}

static void testHomePreservesAnimeMembershipDuringBoundedReads()
{
    std::printf("[test] Home preserves Anime membership during bounded reads\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(source.find("m_animeItemIds") != std::string::npos);
    CHECK(source.find("isAnimeSeries") != std::string::npos);
    CHECK(source.find("CachedLibraryView classificationView") == std::string::npos);
    CHECK(source.find("view.name") != std::string::npos);
    std::printf("[test] Home preserves Anime membership during bounded reads OK\n");
}


