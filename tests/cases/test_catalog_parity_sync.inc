void testLibrarySyncAppliesLiveCatalogChanges()
{
    std::printf("[test] LibrarySync applies live catalog changes\n");
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    CHECK(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) == 0);
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                        &addressSize) == 0);
    CHECK(::listen(listener, 1) == 0);

    const std::string url = "http://127.0.0.1:" +
        std::to_string(ntohs(address.sin_port));
    const std::string scopeUrl = "http://live-catalog-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    CatalogDb db;
    CHECK(configureCatalogParityDb(db, scopeUrl, "live-user"));
    const auto epoch = db.scopeState().requestedEpoch;
    const auto metadata = CatalogDbJobMetadata{0, epoch, {}};
    MediaItem old = catalogParityItem("live-old", "movie", "Old title");
    MediaItem removed = catalogParityItem("live-removed", "movie", "Gone");
    CatalogDbMediaPageWrite seed;
    seed.items = {old, removed};
    seed.viewId = "live-movies";
    seed.viewName = "Movies";
    seed.collectionType = "movies";
    CHECK(db.upsertMediaPage(seed, metadata).get().success);

    std::string request;
    const std::string body =
        R"({"Items":[{"Id":"live-old","Type":"Movie","Name":"Updated title"},{"Id":"live-new","Type":"Movie","Name":"New title"}]})";
    std::thread server([&]() {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) return;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) break;
            request.append(buffer, static_cast<size_t>(received));
        }
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        (void)::send(client, response.data(), response.size(), 0);
        ::close(client);
    });

    Session session;
    session.serverUrl = url;
    session.accessToken = "live-token";
    session.userId = "live-user";
    session.deviceId = "live-device";
    auto sync = std::make_shared<library::LibrarySync>(
        session, std::shared_ptr<CatalogDb>(&db, [](CatalogDb *) {}), epoch);
    JellyfinLibraryChangeBatch batch;
    batch.itemsAdded = {"live-new", "live-new"};
    batch.itemsUpdated = {"live-old", "live-old"};
    batch.itemsRemoved = {"live-removed", "live-removed"};
    const auto result = sync->applyLibraryChanges(batch).get();
    server.join();
    ::close(listener);
    CHECK(result.success && !result.catchUpRequired);
    CHECK(result.itemsFetched == 2 && result.itemsUpserted == 2);
    CHECK(result.itemsRemoved == 1);
    CHECK(request.find("Ids=") != std::string::npos);
    const auto updated = db.readMediaItemsByIds({"live-old", "live-new"}, metadata).get();
    CHECK(updated.success && updated.items.size() == 2);
    if (updated.success && updated.items.size() == 2) {
        CHECK(std::find_if(updated.items.begin(), updated.items.end(),
                           [](const MediaItem &item) {
                               return item.id == "live-old"
                                   && item.title == "Updated title";
                           }) != updated.items.end());
    }
    const auto deleted = db.readMediaItemsByIds({"live-removed"}, metadata).get();
    CHECK(deleted.success && deleted.items.empty());
    sync.reset();
    std::printf("[test] LibrarySync applies live catalog changes OK\n");
}

void testCatalogDbDuplicateMergeAndAuthoritativeDeletionParity()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-parity-reconcile-" + suffix
        + ".example";
    const std::string user = "parity-reconcile-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    MediaItem series = catalogParityItem("__task16_keep_series__", "show",
                                         "Keep Series");
    series.seriesName.clear();
    series.seriesId.clear();
    series.seasonId.clear();
    MediaItem season = catalogParityItem("__task16_keep_season__", "season",
                                         "Original Season");
    season.seriesId = series.id;
    season.seriesName = series.title;
    season.seasonId.clear();
    MediaItem episode = catalogParityItem("__task16_keep_episode__",
                                          "episode", "Original Episode");
    episode.seriesId = series.id;
    episode.seriesName = series.title;
    episode.seasonId = season.id;
    episode.parentIndexNumber = season.indexNumber;

    MediaItem removedSeries = series;
    removedSeries.id = "__task16_removed_series__";
    removedSeries.title = "Removed Series";
    MediaItem removedSeason = season;
    removedSeason.id = "__task16_removed_season__";
    removedSeason.seriesId = removedSeries.id;
    MediaItem removedEpisode = episode;
    removedEpisode.id = "__task16_removed_episode__";
    removedEpisode.seriesId = removedSeries.id;
    removedEpisode.seasonId = removedSeason.id;
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, url, user));
        CHECK(db.upsertSeriesHierarchy(series,
                                       {season},
                                       {{season.id, {episode}}}, 1, 100)
                  .get()
                  .success);
        CHECK(db.upsertSeriesHierarchy(removedSeries, {removedSeason},
                                       {{removedSeason.id, {removedEpisode}}},
                                       2, 200)
                  .get()
                  .success);
        const auto sqliteBefore = db.reconcileSeries({series}, true).get();
        CHECK(sqliteBefore.success && sqliteBefore.authoritative
              && sqliteBefore.seriesDeleted == 1);
        CHECK(db.getSeasons(removedSeries.id).get().items.empty());
        const auto sqliteKeepSeasons = db.getSeasons(series.id).get();
        const auto sqliteKeepEpisodes = db.getEpisodes(season.id).get();
        CHECK(sqliteKeepSeasons.success && sqliteKeepEpisodes.success);
        checkCatalogParityItems({season},
                                sqliteKeepSeasons.items);
        checkCatalogParityItems({episode},
                                sqliteKeepEpisodes.items);
    }

    const std::string mediaUrl = url + "-media";
    const auto mediaPaths = catalogMigrationTestPaths(mediaUrl, user);
    removeCatalogMigrationTestPaths(mediaPaths);
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, mediaUrl, user));
        const auto generationA = db.beginTopLevelSync(10).get();
        CHECK(generationA.success);
        CatalogDbMediaPageWrite first;
        first.items = {
            catalogParityItem("__task20_removed_item__", "movie",
                              "Removed Item"),
            catalogParityItem("__task20_remaining_item__", "movie",
                              "Remaining Item")};
        first.viewId = "task20-movies";
        first.viewName = "Task 20 Movies";
        first.collectionType = "movies";
        first.syncGeneration = 10;
        CHECK(db.upsertMediaPage(first).get().success);
        CHECK(db.finalizeTopLevelSync(10).get().success);

        const auto generationB = db.beginTopLevelSync(11).get();
        CHECK(generationB.success);
        CatalogDbMediaPageWrite second = first;
        second.items = {first.items[1]};
        second.syncGeneration = 11;
        CHECK(db.upsertMediaPage(second).get().success);
        CHECK(db.finalizeTopLevelSync(11).get().success);
        const auto page = db.readMediaPage("movie", -1, 8).get();
        CHECK(page.success && page.items.size() == 1
              && page.items[0].id == "__task20_remaining_item__");
        CHECK(page.membershipsByItem.size() == 1
              && page.membershipsByItem.at(
                     "__task20_remaining_item__").size() == 1);
    }
    removeCatalogMigrationTestPaths(mediaPaths);

    const std::string deletedViewUrl = url + "-deleted-view";
    const auto deletedViewPaths =
        catalogMigrationTestPaths(deletedViewUrl, user);
    removeCatalogMigrationTestPaths(deletedViewPaths);
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, deletedViewUrl, user));
        MediaItem surviving = catalogParityItem(
            "__task21_surviving_view_item__", "movie", "Surviving View Item");
        MediaItem deletedOnly = catalogParityItem(
            "__task21_deleted_view_item__", "movie", "Deleted View Item");

        CHECK(db.beginTopLevelSync(20).get().success);
        CatalogDbMediaPageWrite survivingView;
        survivingView.items = {surviving};
        survivingView.viewId = "task21-surviving-view";
        survivingView.viewName = "Task 21 Surviving View";
        survivingView.collectionType = "movies";
        survivingView.viewOrdinal = 0;
        survivingView.syncGeneration = 20;
        CHECK(db.upsertMediaPage(survivingView).get().success);
        CatalogDbMediaPageWrite deletedView = survivingView;
        deletedView.items = {surviving, deletedOnly};
        deletedView.viewId = "task21-deleted-view";
        deletedView.viewName = "Task 21 Deleted View";
        deletedView.viewOrdinal = 1;
        CHECK(db.upsertMediaPage(deletedView).get().success);
        CHECK(db.finalizeTopLevelSync(20).get().success);

        CHECK(db.beginTopLevelSync(21).get().success);
        survivingView.syncGeneration = 21;
        CHECK(db.upsertMediaPage(survivingView).get().success);
        CHECK(db.finalizeTopLevelSync(21).get().success);

        const auto descriptors = CatalogCompatibility::readLibrarySnapshot(
            db, {0, db.scopeState().requestedEpoch, {}})
                                     .get();
        CHECK(descriptors.success && descriptors.snapshot.movies.size() == 1);
        if (descriptors.success && descriptors.snapshot.movies.size() == 1) {
            CHECK(descriptors.snapshot.movies[0].id
                  == "task21-surviving-view");
            CHECK(descriptors.snapshot.movies[0].name
                  == "Task 21 Surviving View");
            CHECK(descriptors.snapshot.movies[0].items.size() == 1);
            CHECK(descriptors.snapshot.movies[0].items[0].id == surviving.id);
        }

        const auto page = db.readMediaPage("movie", -1, 8).get();
        CHECK(page.success && page.items.size() == 1
              && page.items[0].id == surviving.id);
        CHECK(page.membershipsByItem.size() == 1
              && page.membershipsByItem.at(surviving.id).size() == 1
              && page.membershipsByItem.at(surviving.id)[0].viewId
                     == "task21-surviving-view");
    }
    removeCatalogMigrationTestPaths(deletedViewPaths);

    const std::string movedItemUrl = url + "-moved-item";
    const auto movedItemPaths = catalogMigrationTestPaths(movedItemUrl, user);
    removeCatalogMigrationTestPaths(movedItemPaths);
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, movedItemUrl, user));
        MediaItem moved = catalogParityItem(
            "__task22_moved_item__", "movie", "Moved Library Item");
        MediaItem stable = catalogParityItem(
            "__task22_stable_item__", "movie", "Stable Library Item");

        CHECK(db.beginTopLevelSync(30).get().success);
        CatalogDbMediaPageWrite viewA;
        viewA.items = {moved};
        viewA.viewId = "task22-view-a";
        viewA.viewName = "Task 22 View A";
        viewA.collectionType = "movies";
        viewA.viewOrdinal = 0;
        viewA.syncGeneration = 30;
        CHECK(db.upsertMediaPage(viewA).get().success);
        CHECK(db.finalizeTopLevelSync(30).get().success);

        CHECK(db.beginTopLevelSync(31).get().success);
        CatalogDbMediaPageWrite viewB = viewA;
        viewB.items = {moved, stable};
        viewB.viewId = "task22-view-b";
        viewB.viewName = "Task 22 View B";
        viewB.syncGeneration = 31;
        CHECK(db.upsertMediaPage(viewB).get().success);
        CHECK(db.finalizeTopLevelSync(31).get().success);

        const auto firstPage = db.readMediaPage("movie", -1, 1).get();
        CHECK(firstPage.success && firstPage.items.size() == 1
              && firstPage.items[0].id == moved.id && firstPage.hasMore);
        CHECK(firstPage.membershipsByItem.count(moved.id) == 1
              && firstPage.membershipsByItem.at(moved.id).size() == 1
              && firstPage.membershipsByItem.at(moved.id)[0].viewId
                     == "task22-view-b");
        if (firstPage.success && firstPage.hasMore) {
            const auto secondPage =
                db.readMediaPage("movie", -1, 1, firstPage.next).get();
            CHECK(secondPage.success && secondPage.items.size() == 1
                  && secondPage.items[0].id == stable.id
                  && !secondPage.hasMore);
        }
    }
    removeCatalogMigrationTestPaths(movedItemPaths);
    removeCatalogMigrationTestPaths(paths);
}

void testCatalogDbLibrarySnapshotSeed()
{
    const std::string url = "https://sqlite-library-seed.example";
    const std::string user = "library-seed-user";
    auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    const auto connection = db.connectionStateForTest();
    CHECK(connection.open && connection.workerOwned);
    const std::string catalogHeader = readTestBytes("src/catalog/CatalogDb.hpp");
    const std::string catalogSource = readTestBytes("src/catalog/CatalogDb.cpp");
    const std::string compatibilityHeader =
        readTestBytes("src/catalog/CatalogCompatibility.hpp");
    const std::string compatibilitySource =
        readTestBytes("src/catalog/CatalogCompatibility.cpp");
    CHECK(catalogHeader.find("LibrarySnapshot") == std::string::npos);
    CHECK(catalogHeader.find("seedLibrarySnapshot") == std::string::npos);
    CHECK(catalogHeader.find("readLibrarySnapshot") == std::string::npos);
    CHECK(catalogSource.find("LibrarySnapshot") == std::string::npos);
    CHECK(catalogSource.find("seedLibrarySnapshot") == std::string::npos);
    CHECK(catalogSource.find("readLibrarySnapshot") == std::string::npos);
    CHECK(compatibilityHeader.find("class CatalogCompatibility")
          != std::string::npos);
    CHECK(compatibilitySource.find("enqueueLibrarySeed")
          != std::string::npos);
    CHECK(compatibilitySource.find("enqueueLibraryRead")
          != std::string::npos);
    CHECK(compatibilitySource.find("sqlite3_") == std::string::npos);
    const char *runtimeSources[] = {
        "src/app/App.cpp", "src/ui/screens/HomeScreen.cpp",
        "src/ui/screens/SeriesScreen.cpp",
        "src/ui/screens/EpisodeBrowserScreen.cpp",
        "src/download/DownloadManager.cpp", "src/library/LibrarySync.cpp",
        "src/library/LibraryQuery.cpp", "src/library/OfflineLibraryQuery.cpp"};
    for (const char *path : runtimeSources) {
        const std::string source = readTestBytes(path);
        CHECK(source.find("seedLibrarySnapshot") == std::string::npos);
        CHECK(source.find("readLibrarySnapshot") == std::string::npos);
    }
    LibrarySnapshot snapshot;
    MediaItem movie = catalogParityItem("__task29_movie__", "movie", "Seed Movie");
    MediaItem secondMovie = catalogParityItem("__task29_movie_2__", "movie", "Seed Movie 2");
    snapshot.movies.push_back({"movies", "Movies", "movies", {movie}});
    snapshot.movies[0].items.push_back(secondMovie);
    snapshot.continueWatching.push_back(movie);
    auto first = CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get();
    CHECK(first.success && first.workerOwned && first.itemsUpserted == 2
          && first.viewsWritten == 1 && first.homeItemsWritten == 1);
    auto read = CatalogCompatibility::readLibrarySnapshot(
        db, {0, epoch, {}}).get();
    CHECK(read.success && read.snapshot.movies.size() == 1
          && read.snapshot.movies[0].collectionType == "movies"
          && read.snapshot.continueWatching.size() == 1
          && read.snapshot.continueWatching[0].id == movie.id);
    auto second = CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get();
    CHECK(second.success && second.itemsUpserted == 2 && second.viewsWritten == 1);
    auto failed = CatalogCompatibility::seedLibrarySnapshotForTest(
        db, snapshot, 1, {0, epoch, {}}).get();
    CHECK(!failed.success && failed.error == CatalogDbErrorCategory::SqliteError);
    auto recovered = CatalogCompatibility::seedLibrarySnapshot(
        db, LibrarySnapshot{}, {0, epoch, {}}).get();
    CHECK(recovered.success && recovered.itemsUpserted == 0);
    auto nextEpoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    auto stale = CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get();
    CHECK(!stale.success && stale.superseded);
    auto empty = CatalogCompatibility::seedLibrarySnapshot(
        db, LibrarySnapshot{}, {0, nextEpoch, {}}).get();
    CHECK(empty.success && empty.itemsUpserted == 0 && empty.viewsWritten == 0);
    removeCatalogMigrationTestPaths(paths);
}

void testCatalogDbTopLevelSyncStagingLifecycle()
{
    const std::string url = "https://sqlite-top-level-stage.example";
    const std::string user = "top-level-stage-user";
    auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    LibrarySnapshot snapshot;
    snapshot.movies.push_back({"movies", "Movies", "movies",
                                {catalogParityItem("__stage_movie__", "movie", "Stage Movie")} });
    CHECK(CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get().success);
    auto before = CatalogCompatibility::readLibrarySnapshot(
        db, {0, epoch, {}}).get();
    auto begin = db.beginTopLevelSync(101, {0, epoch, {}}).get();
    CHECK(begin.success && begin.workerOwned && begin.generation == 101);
    auto newer = db.beginTopLevelSync(102, {0, epoch, {}}).get();
    CHECK(newer.success && newer.generation == 102);
    auto abort = db.abortTopLevelSync(102, {0, epoch, {}}).get();
    CHECK(abort.success && abort.workerOwned);
    auto repeatAbort = db.abortTopLevelSync(102, {0, epoch, {}}).get();
    CHECK(repeatAbort.success);
    auto after = CatalogCompatibility::readLibrarySnapshot(
        db, {0, epoch, {}}).get();
    CHECK(before.success && after.success && after.snapshot.movies.size() == 1
          && after.snapshot.movies[0].items.size() == 1
          && after.snapshot.movies[0].items[0].id == "__stage_movie__");
    CHECK(db.beginTopLevelSync(104, {0, epoch, {}}).get().success);
    CatalogDbMediaPageWrite replacement;
    replacement.items = {catalogParityItem("__replacement_movie__", "movie", "Replacement Movie")};
    replacement.viewId = "movies"; replacement.viewName = "Movies";
    replacement.collectionType = "movies"; replacement.syncGeneration = 104;
    CHECK(db.upsertMediaPage(replacement, {0, epoch, {}}).get().success);
    auto finalized = db.finalizeTopLevelSync(104, {0, epoch, {}}).get();
    CHECK(finalized.success && finalized.workerOwned);
    auto published = CatalogCompatibility::readLibrarySnapshot(
        db, {0, epoch, {}}).get();
    CHECK(published.success && published.snapshot.movies.size() == 1
          && published.snapshot.movies[0].items.size() == 1
          && published.snapshot.movies[0].items[0].id == "__replacement_movie__");
    const auto mediaPage = db.readMediaPage(
        "movie", -1, 8, {}, {0, epoch, {}}).get();
    CHECK(mediaPage.success && mediaPage.items.size() == 1
          && mediaPage.items[0].id == "__replacement_movie__");
    db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    auto stale = db.beginTopLevelSync(103, {0, epoch, {}}).get();
    CHECK(!stale.success && stale.error == CatalogDbErrorCategory::ScopeNotReady);

    const std::string rollbackUrl = url + "-task28";
    const std::string rollbackUser = user + "-task28";
    const auto rollbackPaths =
        catalogMigrationTestPaths(rollbackUrl, rollbackUser);
    removeCatalogMigrationTestPaths(rollbackPaths);
    {
        auto rollbackDb = std::make_shared<CatalogDb>();
        const auto rollbackEpoch =
            rollbackDb->configureScope(rollbackUrl, rollbackUser);
        CHECK(rollbackDb->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = rollbackUrl;
        session.userId = rollbackUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, rollbackDb, rollbackEpoch);

        CHECK(sync->begin(200).get().success);
        CatalogDbMediaPageWrite generationAMovies;
        generationAMovies.items = {
            catalogParityItem("__task28_a_movie_1__", "movie", "A Movie 1"),
            catalogParityItem("__task28_a_movie_2__", "movie", "A Movie 2")};
        generationAMovies.viewId = "task28-a-movies";
        generationAMovies.viewName = "A Movies";
        generationAMovies.collectionType = "movies";
        generationAMovies.viewOrdinal = 0;
        generationAMovies.syncGeneration = 200;
        CHECK(sync->stage(generationAMovies).get().success);
        CatalogDbMediaPageWrite generationAShows = generationAMovies;
        generationAShows.items = {catalogParityItem(
            "__task28_a_show__", "show", "A Show")};
        generationAShows.viewId = "task28-a-shows";
        generationAShows.viewName = "A Shows";
        generationAShows.collectionType = "tvshows";
        generationAShows.viewOrdinal = 1;
        CHECK(sync->stage(generationAShows).get().success);
        CHECK(sync->finalize(200).get().success);

        library::LibraryCoordinator coordinator(session, rollbackDb, rollbackEpoch);
        const auto query = coordinator.query();
        const auto beforeMovies = query->movies(-1, 8).get();
        const auto beforeShows = query->shows(-1, 8).get();
        CHECK(beforeMovies.success && beforeMovies.items.size() == 2);
        CHECK(beforeShows.success && beforeShows.items.size() == 1
              && beforeShows.items[0].id == "__task28_a_show__");

        CHECK(sync->begin(201).get().success);
        CatalogDbMediaPageWrite generationB = generationAMovies;
        generationB.items = {catalogParityItem(
            "__task28_b_movie__", "movie", "B Movie")};
        generationB.viewId = "task28-b-movies";
        generationB.viewName = "B Movies";
        generationB.syncGeneration = 201;
        CHECK(sync->stage(generationB).get().success);
        CHECK(sync->abort(201).get().success);

        const auto afterCancelMovies = query->movies(-1, 8).get();
        const auto afterCancelShows = query->shows(-1, 8).get();
        CHECK(afterCancelMovies.success && afterCancelMovies.items.size() == 2);
        CHECK(afterCancelShows.success && afterCancelShows.items.size() == 1
              && afterCancelShows.items[0].id == "__task28_a_show__");
        CHECK(std::none_of(afterCancelMovies.items.begin(),
                           afterCancelMovies.items.end(),
                           [](const MediaItem &item) {
                               return item.id == "__task28_b_movie__";
                           }));

        CHECK(sync->begin(202).get().success);
        CatalogDbMediaPageWrite generationC = generationAMovies;
        generationC.items = {catalogParityItem(
            "__task28_c_movie__", "movie", "C Movie")};
        generationC.viewId = "task28-c-movies";
        generationC.viewName = "C Movies";
        generationC.syncGeneration = 202;
        CHECK(sync->stage(generationC).get().success);
        CHECK(sync->finalize(202).get().success);
        const auto afterC = query->movies(-1, 8).get();
        CHECK(afterC.success && afterC.items.size() == 1
              && afterC.items[0].id == "__task28_c_movie__");
    }
    removeCatalogMigrationTestPaths(rollbackPaths);
    removeCatalogMigrationTestPaths(paths);
}

void testLibrarySyncTeardownDuringStagedGeneration()
{
    std::printf("[test] LibrarySync teardown during staged generation\n");
    const std::string url =
        "https://sqlite-library-sync-teardown-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string user = "library-sync-teardown-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(url, user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    Session session;
    session.serverUrl = url;
    session.userId = user;
    {
        auto sync = std::make_unique<library::LibrarySync>(
            session, db, epoch);
        CHECK(sync->begin(300).get().success);
        CatalogDbMediaPageWrite committedPage;
        committedPage.items = {catalogParityItem(
            "__task30_committed_movie__", "movie", "Committed Movie")};
        committedPage.viewId = "task30-committed-movies";
        committedPage.viewName = "Committed Movies";
        committedPage.collectionType = "movies";
        committedPage.viewOrdinal = 0;
        committedPage.syncGeneration = 300;
        CHECK(sync->stage(committedPage).get().success);
        CHECK(sync->finalize(300).get().success);
        CHECK(sync->writeSyncState(300, 300, 300).get().success);

        const auto regressed = sync->writeSyncState(299, 299, 299).get();
        CHECK(!regressed.success
              && regressed.error == CatalogDbErrorCategory::ConfigurationFailed);
        const auto afterRegression = db->readSyncState(
            false, 0, 0, {0, epoch, {}}).get();
        CHECK(afterRegression.success
              && afterRegression.lastSuccessfulMs == 300
              && afterRegression.lastReconcileMs == 300
              && afterRegression.committedGeneration == 300);

        auto cancelledCheckpoint = std::make_shared<std::atomic_bool>(true);
        const auto cancelled = sync->writeSyncState(
            400, 400, 400, cancelledCheckpoint).get();
        CHECK(!cancelled.success && cancelled.cancelled);
        const auto afterCancellation = db->readSyncState(
            false, 0, 0, {0, epoch, {}}).get();
        CHECK(afterCancellation.success
              && afterCancellation.lastSuccessfulMs == 300
              && afterCancellation.lastReconcileMs == 300
              && afterCancellation.committedGeneration == 300);

        db->setWorkerPausedForTest(true);
        auto pendingBegin = sync->begin(301);
        CatalogDbMediaPageWrite stagedPage = committedPage;
        stagedPage.items = {catalogParityItem(
            "__task30_unpublished_movie__", "movie", "Unpublished Movie")};
        stagedPage.viewId = "task30-unpublished-movies";
        stagedPage.viewName = "Unpublished Movies";
        stagedPage.syncGeneration = 301;
        auto pendingStage = sync->stage(stagedPage);
        sync.reset();

        db->setWorkerPausedForTest(false);
        const auto beginResult = pendingBegin.get();
        const auto stageResult = pendingStage.get();
        CHECK(beginResult.success && beginResult.generation == 301);
        CHECK(!stageResult.success
              && (stageResult.cancelled || stageResult.superseded));
    }

    db.reset();
    auto reopened = std::make_shared<CatalogDb>();
    const auto reopenedEpoch = reopened->configureScope(url, user);
    CHECK(reopened->waitForIdleForTest(std::chrono::seconds(2)));
    {
        library::LibraryCoordinator coordinator(session, reopened, reopenedEpoch);
        const auto query = coordinator.query();
        const auto movies = query->movies(-1, 8).get();
        CHECK(movies.success && movies.items.size() == 1
              && movies.items[0].id == "__task30_committed_movie__");
        const auto syncState = reopened->readSyncState(
            false, 0, 0, {0, reopenedEpoch, {}}).get();
        CHECK(syncState.success && syncState.lastSuccessfulMs == 300
              && syncState.lastReconcileMs == 300
              && syncState.committedGeneration == 300);
    }
    reopened.reset();
    removeCatalogMigrationTestPaths(paths);

    const std::string otherUrl =
        "https://sqlite-library-sync-scope-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const auto otherPaths = catalogMigrationTestPaths(otherUrl, user);
    removeCatalogMigrationTestPaths(otherPaths);
    auto otherDb = std::make_shared<CatalogDb>();
    const auto otherEpoch = otherDb->configureScope(otherUrl, user);
    CHECK(otherDb->waitForIdleForTest(std::chrono::seconds(2)));
    Session otherSession = session;
    otherSession.serverUrl = otherUrl;
    auto otherSync = std::make_unique<library::LibrarySync>(
        otherSession, otherDb, otherEpoch);
    CHECK(otherSync->writeSyncState(900, 900, 1).get().success);
    const auto otherState = otherDb->readSyncState(
        false, 0, 0, {0, otherEpoch, {}}).get();
    CHECK(otherState.success && otherState.lastSuccessfulMs == 900
          && otherState.committedGeneration == 1);
    otherSync.reset();
    otherDb.reset();
    removeCatalogMigrationTestPaths(otherPaths);
    std::printf("[test] LibrarySync teardown during staged generation OK\n");
}

static void testLibrarySyncChangedMetadataCatchUp()
{
    std::printf("[test] LibrarySync changed metadata catch-up\n");
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    CHECK(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) == 0);
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                        &addressSize) == 0);
    CHECK(::listen(listener, 1) == 0);

    const std::string url = "http://127.0.0.1:" +
        std::to_string(ntohs(address.sin_port));
    const std::string user = "changed-metadata-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(url, user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    Session session;
    session.serverUrl = url;
    session.userId = user;
    auto sync = std::make_shared<library::LibrarySync>(session, db, epoch);
    const MediaItem original = catalogParityItem(
        "__task36_changed_movie__", "movie", "Original Movie");
    const MediaItem unrelated = catalogParityItem(
        "__task36_unrelated_movie__", "movie", "Unrelated Movie");
    CatalogDbMediaPageWrite seed;
    seed.items = {original, unrelated};
    seed.viewId = "task36-movies";
    seed.viewName = "Movies";
    seed.collectionType = "movies";
    seed.finalPage = true;
    CHECK(sync->stage(seed).get().success);

    const std::string body =
        R"({"Items":[{"Id":"__task36_changed_movie__","Type":"Movie",
        "Name":"Updated Movie","Overview":"Updated overview",
        "ProductionYear":2027,"CommunityRating":9.1,
        "Etag":"updated-etag","ImageTags":{"Primary":"updated-primary"}}]})";
    std::thread server([&] {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) return;
        char buffer[4096];
        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
            if (received <= 0) break;
            request.append(buffer, static_cast<size_t>(received));
        }
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        (void)::send(client, response.data(), response.size(), 0);
        ::close(client);
    });

    const auto result = sync->catchUpChangedCatalog(1700000000123LL).get();
    server.join();
    ::close(listener);
    CHECK(result.success && result.itemsUpserted == 1);
    {
        library::LibraryCoordinator coordinator(session, db, epoch);
        const auto query = coordinator.query();
        const auto page = query->movies(-1, 8).get();
        CHECK(page.success && page.items.size() == 2);
        if (page.success && page.items.size() == 2) {
            const auto changed = std::find_if(
                page.items.begin(), page.items.end(), [&](const MediaItem &item) {
                    return item.id == original.id;
                });
            const auto kept = std::find_if(
                page.items.begin(), page.items.end(), [&](const MediaItem &item) {
                    return item.id == unrelated.id;
                });
            CHECK(changed != page.items.end() && changed->title == "Updated Movie");
            CHECK(changed != page.items.end()
                  && changed->imageTags.at("Primary") == "updated-primary");
            CHECK(kept != page.items.end() && kept->title == unrelated.title);
        }
        const auto state = db->readSyncState(false, 0, 0, {0, epoch, {}}).get();
        CHECK(state.success && state.lastSuccessfulMs > 0);
    }
    sync.reset();
    db.reset();
    removeCatalogMigrationTestPaths(paths);
    std::printf("[test] LibrarySync changed metadata catch-up OK\n");
}

static void testLibrarySyncAuthoritativeMembershipReconcile()
{
    std::printf("[test] LibrarySync authoritative membership reconcile\n");
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    CHECK(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) == 0);
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                        &addressSize) == 0);
    CHECK(::listen(listener, 3) == 0);
    const std::string url = "http://127.0.0.1:" +
        std::to_string(ntohs(address.sin_port));
    const std::string user = "membership-reconcile-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(url, user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    Session session;
    session.serverUrl = url;
    session.userId = user;
    auto sync = std::make_shared<library::LibrarySync>(session, db, epoch);

    const MediaItem removedMovie = catalogParityItem(
        "__task37_removed_movie__", "movie", "Removed Movie");
    const MediaItem removedSeries = catalogParityItem(
        "__task37_removed_series__", "show", "Removed Series");
    const MediaItem movedMovie = catalogParityItem(
        "__task37_moved_movie__", "movie", "Moved Movie");
    CatalogDbMediaPageWrite oldMovies;
    oldMovies.items = {removedMovie, movedMovie};
    oldMovies.viewId = "old-movies";
    oldMovies.viewName = "Old Movies";
    oldMovies.collectionType = "movies";
    CHECK(sync->stage(oldMovies).get().success);
    CatalogDbMediaPageWrite oldShows;
    oldShows.items = {removedSeries};
    oldShows.viewId = "old-shows";
    oldShows.viewName = "Old Shows";
    oldShows.collectionType = "tvshows";
    oldShows.viewOrdinal = 1;
    CHECK(sync->stage(oldShows).get().success);

    const std::string viewsBody =
        R"({"Items":[{"Id":"current-movies","Name":"Current Movies",
        "CollectionType":"movies"},{"Id":"current-shows","Name":"Current Shows",
        "CollectionType":"tvshows"}]})";
    const std::string moviesBody =
        R"({"StartIndex":0,"TotalRecordCount":1,"Items":[{"Id":"__task37_moved_movie__",
        "Type":"Movie","Name":"Moved Movie","Overview":"Still here"}]})";
    const std::string showsBody =
        R"({"StartIndex":0,"TotalRecordCount":0,"Items":[]})";
    std::thread server([&] {
        const std::string bodies[] = {viewsBody, moviesBody, showsBody};
        for (const std::string &body : bodies) {
            const int client = ::accept(listener, nullptr, nullptr);
            if (client < 0) return;
            char buffer[4096];
            std::string request;
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                request.append(buffer, static_cast<size_t>(received));
            }
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(client, response.data(), response.size(), 0);
            ::close(client);
        }
    });

    const auto result = sync->reconcileAuthoritativeMembership().get();
    server.join();
    ::close(listener);
    CHECK(result.success && result.pagesRead == 2);
    {
        library::LibraryCoordinator coordinator(session, db, epoch);
        const auto query = coordinator.query();
        const auto movies = query->movies(-1, 8).get();
        const auto shows = query->shows(-1, 8).get();
        CHECK(movies.success && movies.items.size() == 1
              && movies.items[0].id == movedMovie.id);
        CHECK(shows.success && shows.items.empty());
        CHECK(movies.membershipsByItem.at(movedMovie.id).size() == 1);
        CHECK(movies.membershipsByItem.at(movedMovie.id)[0].viewId
              == "current-movies");
    }
    sync.reset();
    db.reset();
    removeCatalogMigrationTestPaths(paths);
    std::printf("[test] LibrarySync authoritative membership reconcile OK\n");
}

static void testCatalogDbMediaPageTopLevelStaging()
{
    std::printf("[test] CatalogDb staged media page writes\n");
    testLibrarySyncAppliesLiveCatalogChanges();
    testLibrarySyncAuthoritativeMembershipReconcile();
    CatalogDb db;
    const auto epoch = db.configureScope("https://page-stage.example", "user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    const CatalogDbJobMetadata metadata{0, epoch, {}};
    CatalogDbMediaPageWrite live;
    live.items = {catalogParityItem("live-stage", "movie", "Live Stage")};
    live.viewId = "movies"; live.viewName = "Movies"; live.collectionType = "movies";
    CHECK(db.upsertMediaPage(live, metadata).get().success);
    CHECK(db.beginTopLevelSync(77, metadata).get().success);
    CatalogDbMediaPageWrite staged = live;
    staged.items = {catalogParityItem("staged-a", "movie", "A"),
                    catalogParityItem("staged-b", "movie", "B")};
    staged.syncGeneration = 77; staged.ordinalStart = 4;
    CHECK(db.upsertMediaPage(staged, metadata).get().success);
    auto before = CatalogCompatibility::readLibrarySnapshot(
        db, metadata).get();
    CHECK(before.success && before.snapshot.movies.size() == 1
          && before.snapshot.movies[0].items[0].id == "live-stage");
    auto stale = staged; stale.syncGeneration = 76;
    CHECK(db.upsertMediaPage(stale, metadata).get().superseded);
    auto failed = staged; failed.items = {catalogParityItem("rollback-stage", "movie", "R")};
    CHECK(!db.upsertMediaPageForTest(failed, 0, metadata).get().success);
    CHECK(db.abortTopLevelSync(77, metadata).get().success);
    auto after = CatalogCompatibility::readLibrarySnapshot(
        db, metadata).get();
    CHECK(after.success && after.snapshot.movies[0].items[0].id == "live-stage");
    std::printf("[test] CatalogDb staged media page writes OK\n");
}

// Regression: verify that concurrent authoritative reconcile calls are
// coalesced so that only one is in flight at a time.  The second call
// must return superseded immediately without starting a top-level sync
// that would steal the slot from the first.
static void testLibrarySyncAuthoritativeReconcileCoalescing()
{
    std::printf("[test] LibrarySync authoritative reconcile coalescing\n");
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;
    int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
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

    const std::string url = "http://127.0.0.1:" +
        std::to_string(ntohs(address.sin_port));
    const std::string scopeUrl = "http://coalesce-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    CatalogDb db;
    CHECK(configureCatalogParityDb(db, scopeUrl, "coalesce-user"));
    const auto epoch = db.scopeState().requestedEpoch;
    const auto metadata = CatalogDbJobMetadata{0, epoch, {}};

    // Seed an initial movie so the DB is not empty.
    CatalogDbMediaPageWrite seed;
    seed.items = {catalogParityItem("coalesce-seed", "movie", "Seed")};
    seed.viewId = "movies"; seed.viewName = "Movies";
    seed.collectionType = "movies";
    CHECK(db.upsertMediaPage(seed, metadata).get().success);

    // Server handles views request (1st connection), then keeps the
    // page-request connection open until released so the first reconcile
    // stays in-flight while the second call is attempted.
    const std::string viewsBody =
        R"({"Items":[{"Id":"movies","Name":"Movies","CollectionType":"movies"}]})";
    const std::string pageBody =
        R"({"Items":[{"Id":"coalesce-seed","Type":"Movie","Name":"Seed"}],"TotalRecordCount":1})";
    std::atomic<bool> pageClientReady{false};
    std::atomic<bool> releasePageServer{false};
    std::thread server([&]() {
        // 1st connection: views
        const int viewsClient = ::accept(listener, nullptr, nullptr);
        if (viewsClient < 0) return;
        char buffer[4096];
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
            const ssize_t rcvd = ::recv(viewsClient, buffer, sizeof(buffer), 0);
            if (rcvd <= 0) break;
            req.append(buffer, static_cast<size_t>(rcvd));
        }
        const std::string viewsResp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(viewsBody.size()) +
            "\r\nConnection: close\r\n\r\n" + viewsBody;
        (void)::send(viewsClient, viewsResp.data(), viewsResp.size(), 0);
        ::close(viewsClient);

        // 2nd connection: page — hold it open until the test signals.
        const int pageClient = ::accept(listener, nullptr, nullptr);
        if (pageClient < 0) return;
        pageClientReady.store(true);
        while (!releasePageServer.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ::close(pageClient);
    });

    Session session;
    session.serverUrl = url;
    session.accessToken = "coalesce-token";
    session.userId = "coalesce-user";
    session.deviceId = "coalesce-device";
    auto sync = std::make_shared<library::LibrarySync>(
        session, std::shared_ptr<CatalogDb>(&db, [](CatalogDb *) {}), epoch);

    // Start the first reconcile on a background thread.
    auto firstCancel = std::make_shared<std::atomic_bool>(false);
    auto firstFuture = std::async(std::launch::async, [&] {
        return sync->reconcileAuthoritativeMembership(firstCancel).get();
    });

    // Wait until the first reconcile is connected and blocked on the
    // page request (server holds the connection open).
    while (!pageClientReady.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // A concurrent reconcile must be coalesced immediately.
    auto secondResult = sync->reconcileAuthoritativeMembership().get();
    CHECK(secondResult.superseded);
    CHECK(secondResult.error == CatalogDbErrorCategory::Superseded);

    // Release the server and first reconcile.
    releasePageServer.store(true);
    firstCancel->store(true);
    (void)firstFuture.get();

    server.join();
    ::close(listener);

    // FIX 2: verify the flag is released — a third reconcile must NOT be
    // superseded once the first has completed.
    const int listener2 = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener2 >= 0);
    if (listener2 < 0) {
        sync.reset();
        std::printf("[test] LibrarySync authoritative reconcile coalescing OK (flag-release skipped)\n");
        return;
    }
    int reuse2 = 1;
    ::setsockopt(listener2, SOL_SOCKET, SO_REUSEADDR, &reuse2, sizeof(reuse2));
    sockaddr_in addr2{};
    addr2.sin_family = AF_INET;
    addr2.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr2.sin_port = htons(0);
    CHECK(::bind(listener2, reinterpret_cast<sockaddr *>(&addr2),
                 sizeof(addr2)) == 0);
    socklen_t addr2Size = sizeof(addr2);
    CHECK(::getsockname(listener2, reinterpret_cast<sockaddr *>(&addr2),
                        &addr2Size) == 0);
    CHECK(::listen(listener2, 2) == 0);

    const std::string url2 = "http://127.0.0.1:" +
        std::to_string(ntohs(addr2.sin_port));

    // Update the session URL so the third reconcile connects to the new server.
    session.serverUrl = url2;
    sync = std::make_shared<library::LibrarySync>(
        session, std::shared_ptr<CatalogDb>(&db, [](CatalogDb *) {}), epoch);

    std::thread server2([&]() {
        // 1st connection: views
        const int viewsClient = ::accept(listener2, nullptr, nullptr);
        if (viewsClient < 0) return;
        char buffer[4096];
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
            const ssize_t rcvd = ::recv(viewsClient, buffer, sizeof(buffer), 0);
            if (rcvd <= 0) break;
            req.append(buffer, static_cast<size_t>(rcvd));
        }
        const std::string viewsResp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(viewsBody.size()) +
            "\r\nConnection: close\r\n\r\n" + viewsBody;
        (void)::send(viewsClient, viewsResp.data(), viewsResp.size(), 0);
        ::close(viewsClient);

        // 2nd connection: page
        const int pageClient = ::accept(listener2, nullptr, nullptr);
        if (pageClient < 0) return;
        std::string req2;
        while (req2.find("\r\n\r\n") == std::string::npos) {
            const ssize_t rcvd = ::recv(pageClient, buffer, sizeof(buffer), 0);
            if (rcvd <= 0) break;
            req2.append(buffer, static_cast<size_t>(rcvd));
        }
        const std::string pageResp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(pageBody.size()) +
            "\r\nConnection: close\r\n\r\n" + pageBody;
        (void)::send(pageClient, pageResp.data(), pageResp.size(), 0);
        ::close(pageClient);
    });

    // Third reconcile: the in-flight flag should have been released by the
    // first worker's FlagGuard, so this must NOT be superseded.
    auto thirdResult = sync->reconcileAuthoritativeMembership().get();
    CHECK(!thirdResult.superseded);

    server2.join();
    ::close(listener2);
    sync.reset();
    std::printf("[test] LibrarySync authoritative reconcile coalescing OK\n");
}

