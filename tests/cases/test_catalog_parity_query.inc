void testCatalogDbProjectionParity()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-parity-projection-" + suffix
        + ".example";
    const std::string user = "parity-projection-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    MediaItem series = catalogParityItem("__task16_projection_series__",
                                         "show", "Projected Series");
    series.seriesName.clear();
    series.seriesId.clear();
    series.seasonId.clear();
    MediaItem season = catalogParityItem("__task16_projection_season__",
                                         "season", "Projected Season");
    season.seriesId = series.id;
    season.seriesName = series.title;
    season.seasonId.clear();
    MediaItem completeEpisode = catalogParityItem(
        "__task16_projection_complete__", "episode", "Complete Episode");
    completeEpisode.seriesId = series.id;
    completeEpisode.seasonId = season.id;
    completeEpisode.seriesName = series.title;
    completeEpisode.parentIndexNumber = season.indexNumber;
    MediaItem unavailableEpisode = completeEpisode;
    unavailableEpisode.id = "__task16_projection_unavailable__";
    unavailableEpisode.title = "Unavailable Episode";
    unavailableEpisode.indexNumber = 2;

    OfflineCatalogSnapshot catalog;
    catalog.series[series.id] = series;
    catalog.seasonsBySeries[series.id] = {season};
    catalog.episodesBySeason[season.id] = {completeEpisode,
                                            unavailableEpisode};
    LibrarySnapshot library;
    library.shows.push_back({"shows", "Shows", "tvshows", {series}});
    DownloadSnapshot downloads;
    DownloadItem complete;
    complete.itemId = completeEpisode.id;
    complete.itemType = "episode";
    complete.state = DownloadState::Complete;
    complete.seriesId = series.id;
    complete.seasonId = season.id;
    complete.episodeNumber = completeEpisode.indexNumber;
    complete.seasonNumber = season.indexNumber;
    downloads.items.push_back(complete);

    OfflineLibraryProjection projection(library, catalog, downloads);
    const auto projectedSeasons = projection.seasons(series.id);
    const auto projectedEpisodes = projection.episodes(season.id);
    CHECK(projectedSeasons.size() == 1);
    CHECK(projectedEpisodes.size() == 1);
    if (projectedEpisodes.size() == 1) {
        CHECK(catalogParityItemEqual(completeEpisode, projectedEpisodes[0]));
    }

    std::remove(paths.legacyPath.c_str());
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, url, user));
        CHECK(db.upsertSeriesHierarchy(series, {season},
                                       {{season.id, {completeEpisode}}}, 1, 100)
                  .get()
                  .success);
        const auto sqliteSeasons = db.getSeasons(series.id).get();
        const auto sqliteEpisodes = db.getEpisodes(season.id).get();
        CHECK(sqliteSeasons.success && sqliteEpisodes.success);
        checkCatalogParityItems(projectedSeasons, sqliteSeasons.items);
        checkCatalogParityItems(projectedEpisodes, sqliteEpisodes.items);
    }
    removeCatalogMigrationTestPaths(paths);
}

void testCatalogDbDownloadFallbackParity()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-parity-fallback-" + suffix
        + ".example";
    const std::string user = "parity-fallback-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    DownloadSnapshot downloads;
    DownloadItem downloaded;
    downloaded.itemId = "__task16_fallback_episode__";
    downloaded.itemType = "episode";
    downloaded.title = "Fallback Episode 世界";
    downloaded.seriesId = "__task16_fallback_series__";
    downloaded.seriesName = "Fallback Series";
    downloaded.seasonId = "__task16_fallback_season__";
    downloaded.seasonName = "Fallback Season";
    downloaded.episodeNumber = 4;
    downloaded.seasonNumber = 2;
    downloaded.playbackPositionTicks = 456789;
    downloaded.runtimeTicks = 9876543;
    downloaded.state = DownloadState::Complete;
    downloads.items.push_back(downloaded);
    LibrarySnapshot emptyLibrary;
    OfflineCatalogSnapshot missingCatalog;
    OfflineLibraryProjection projection(emptyLibrary, missingCatalog,
                                        downloads);
    const auto projectedSeries = projection.series();
    const auto projectedSeasons = projection.seasons(downloaded.seriesId);
    const auto projectedEpisodes = projection.episodes(downloaded.seasonId);
    CHECK(projectedSeries.size() == 1 && projectedSeasons.size() == 1
          && projectedEpisodes.size() == 1);
    MediaItem fallbackEpisode;
    fallbackEpisode.id = downloaded.itemId;
    fallbackEpisode.type = "episode";
    fallbackEpisode.title = downloaded.title;
    fallbackEpisode.seriesId = downloaded.seriesId;
    fallbackEpisode.seriesName = downloaded.seriesName;
    fallbackEpisode.seasonId = downloaded.seasonId;
    fallbackEpisode.indexNumber = downloaded.episodeNumber;
    fallbackEpisode.parentIndexNumber = downloaded.seasonNumber;
    fallbackEpisode.playbackPositionTicks = downloaded.playbackPositionTicks;
    fallbackEpisode.runTimeTicks = downloaded.runtimeTicks;
    if (projectedEpisodes.size() == 1) {
        CHECK(catalogParityItemEqual(fallbackEpisode, projectedEpisodes[0]));
    }
    MediaItem fallbackSeries;
    fallbackSeries.id = downloaded.seriesId;
    fallbackSeries.type = "show";
    fallbackSeries.title = downloaded.seriesName;
    MediaItem fallbackSeason;
    fallbackSeason.id = downloaded.seasonId;
    fallbackSeason.type = "season";
    fallbackSeason.title = downloaded.seasonName;
    fallbackSeason.seriesId = downloaded.seriesId;
    fallbackSeason.indexNumber = downloaded.seasonNumber;
    if (projectedSeries.size() == 1) {
        CHECK(catalogParityItemEqual(fallbackSeries, projectedSeries[0]));
    }
    if (projectedSeasons.size() == 1) {
        CHECK(catalogParityItemEqual(fallbackSeason, projectedSeasons[0]));
    }

    std::remove(paths.legacyPath.c_str());
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, url, user));
        CHECK(db.upsertSeriesHierarchy(fallbackSeries, {fallbackSeason},
                                       {{fallbackSeason.id, {fallbackEpisode}}},
                                       1, 100)
                  .get()
                  .success);
        checkCatalogParityItems(projectedSeasons,
                                db.getSeasons(fallbackSeries.id).get().items);
        checkCatalogParityItems(projectedEpisodes,
                                db.getEpisodes(fallbackSeason.id).get().items);
    }
    removeCatalogMigrationTestPaths(paths);
}

static void testCatalogDbBoundedMediaPaging()
{
    std::printf("[test] CatalogDb bounded media paging\n");
    const std::string url = "https://sqlite-media-pages.example";
    const std::string user = "media-pages-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    CHECK(LibraryCache::scopeKey(url, user) == "184a8ca0cca21a05");
    CHECK(miyoofin::catalog::scopeKey(url, user) == "184a8ca0cca21a05");

    std::vector<MediaItem> movies;
    const std::vector<std::string> titles = {
        "The apple", "APPLE", "Apple", "The APPLE", "apple",
        "The apple pie", "apple pie", "A", "a", "A!", "A-",
        "The", "", "Same", "Same", "Same", "Zed", "zebra",
        "Ärger", "世界", "éclair", "The 2", "2 Fast", "_hidden",
    };
    for (std::size_t i = 0; i < titles.size(); ++i) {
        movies.push_back(catalogParityItem(
            "__task32_movie_" + std::to_string(i) + "__", "movie",
            titles[i]));
    }
    LibrarySnapshot snapshot;
    snapshot.movies.push_back({"movies", "Movies", "movies", movies});
    MediaItem show = catalogParityItem("__task32_show__", "show", "The Show");
    snapshot.shows.push_back({"shows", "Shows", "tvshows", {show}});

    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get().success);

    auto expected = movies;
    std::sort(expected.begin(), expected.end(), organizationalLess);
    std::vector<std::string> expectedIds;
    for (const auto &item : expected) expectedIds.push_back(item.id);
    for (const std::size_t pageSize : {std::size_t(1), std::size_t(2),
                                       std::size_t(3), std::size_t(7),
                                       std::size_t(24)}) {
        CHECK(collectCatalogPageIds(db, "movie", -1, pageSize, epoch)
              == expectedIds);
    }
    for (const auto &item : movies) {
        CHECK(organizationalSortKey(item.title)
              == miyoofin::catalog::organizationalSortKey(item.title));
        CHECK(alphabetIndex(item.title)
              == miyoofin::catalog::alphabetIndex(item.title));
    }
    const auto repeated = collectCatalogPageIds(db, "movie", -1, 3, epoch);
    CHECK(repeated == collectCatalogPageIds(db, "movie", -1, 3, epoch));

    for (int letter = 0; letter < 26; ++letter) {
        std::vector<std::string> expectedLetter;
        for (const auto &item : expected)
            if (matchesAlphabetFilter(item.title, letter))
                expectedLetter.push_back(item.id);
        CHECK(collectCatalogPageIds(db, "movie", letter, 1, epoch)
              == expectedLetter);
    }

    snapshot.movies[0].items[0].title = "Aardvark";
    CHECK(CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get().success);
    expected = snapshot.movies[0].items;
    std::sort(expected.begin(), expected.end(), organizationalLess);
    expectedIds.clear();
    for (const auto &item : expected) expectedIds.push_back(item.id);
    CHECK(collectCatalogPageIds(db, "movie", -1, 2, epoch) == expectedIds);

    const auto plan = db.runMediaPageQueryPlanForTest("movie", -1);
    CHECK(plan.success && plan.workerOwned && plan.mediaPageUsesSortIndex
          && plan.mediaPageAvoidsTempSort);

    const std::string catalogSource = readTestBytes("src/catalog/CatalogDb.cpp");
    const std::string catalogHeader = readTestBytes("src/catalog/CatalogDb.hpp");
    CHECK(catalogSource.find("JellyfinApi") == std::string::npos);
    CHECK(catalogSource.find("LibraryCache::") == std::string::npos);
    CHECK(catalogSource.find("TitleOrganization") == std::string::npos);
    CHECK(catalogHeader.find("JellyfinApi") == std::string::npos);
    CHECK(catalogHeader.find("LibraryCache") == std::string::npos);
    CHECK(catalogHeader.find("TitleOrganization") == std::string::npos);
    CHECK(catalogSource.find("CatalogPrimitives.hpp") != std::string::npos);

    CatalogDb empty;
    const auto emptyEpoch = empty.configureScope(
        "https://sqlite-media-pages-empty.example", "empty-user");
    CHECK(empty.waitForIdleForTest(std::chrono::seconds(2)));
    const auto emptyPage = empty.readMediaPage(
        "movie", -1, 1, {}, {0, emptyEpoch, {}}).get();
    CHECK(emptyPage.success && emptyPage.items.empty() && !emptyPage.hasMore);

    CatalogDb staleGeneration;
    const auto staleEpoch = staleGeneration.configureScope(
        "https://sqlite-media-pages-stale-generation.example", "stale-user");
    CHECK(staleGeneration.waitForIdleForTest(std::chrono::seconds(2)));
    staleGeneration.setGenerationForTest(1);
    staleGeneration.setWorkerPausedForTest(true);
    auto staleFuture = staleGeneration.readMediaPage(
        "movie", -1, 1, {}, {1, staleEpoch, {}});
    staleGeneration.setGenerationForTest(2);
    staleGeneration.setWorkerPausedForTest(false);
    CHECK(staleGeneration.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(staleFuture.get().superseded);

    CatalogDb staleScope;
    const auto firstEpoch = staleScope.configureScope(
        "https://sqlite-media-pages-stale-scope-a.example", "scope-user");
    CHECK(staleScope.waitForIdleForTest(std::chrono::seconds(2)));
    staleScope.setWorkerPausedForTest(true);
    auto staleScopeFuture = staleScope.readMediaPage(
        "movie", -1, 1, {}, {0, firstEpoch, {}});
    const auto secondEpoch = staleScope.configureScope(
        "https://sqlite-media-pages-stale-scope-b.example", "scope-user");
    staleScope.setWorkerPausedForTest(false);
    CHECK(staleScope.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(staleScope.scopeState().requestedEpoch == secondEpoch);
    CHECK(staleScopeFuture.get().superseded);

    const std::string homeSource = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string controllerSource =
        readTestBytes("src/ui/screens/HomeLibraryController.cpp");
    CHECK(homeSource.find("m_libraryQuery->movies") != std::string::npos);
    CHECK(controllerSource.find("std::thread(&HomeLibraryController::fetchWorker") !=
          std::string::npos);
    const std::string coordinatorSource =
        readTestBytes("src/library/LibraryCoordinator.cpp");
    CHECK(coordinatorSource.find("getLibraryItemsPage") != std::string::npos);
    CHECK(controllerSource.find("seedLibrarySnapshot") == std::string::npos);
    CHECK(controllerSource.find("readLibrarySnapshot") == std::string::npos);
    CHECK(controllerSource.find("home_items") == std::string::npos);
    const std::string refreshSource = readTestBytes("src/ui/screens/HomeScreenRefresh.cpp");
    CHECK(refreshSource.find("home_items") == std::string::npos);
    CHECK(homeSource.find("m_movieMaster=combineMovieViews") == std::string::npos);
    CHECK(homeSource.find("m_showMaster=combineShowViews") == std::string::npos);
    CHECK(homeSource.find("rebuildShowsPresentation();if(m_session.manualOfflineMode)") == std::string::npos);
    removeCatalogMigrationTestPaths(paths);
    std::printf("[test] CatalogDb bounded media paging OK\n");
}

static void testLegacyWholeFilePersistenceRetired()
{
    const std::string libraryCacheSource =
        readTestBytes("src/cache/LibraryCache.cpp");
    const std::string syncStateSource =
        readTestBytes("src/cache/SyncState.cpp");
    const std::string offlineCatalogHeader =
        readTestBytes("src/cache/OfflineCatalog.hpp");
    const std::string makefile = readTestBytes("Makefile");
    const std::string crossMakefile = readTestBytes("Makefile.cross");

    CHECK(libraryCacheSource.find("LibraryCache::save")
          == std::string::npos);
    CHECK(syncStateSource.find("SyncStateStore::save")
          == std::string::npos);
    CHECK(offlineCatalogHeader.find("OfflineCatalog::")
          == std::string::npos);
    CHECK(::access("src/cache/OfflineCatalog.cpp", F_OK) != 0);
    CHECK(makefile.find("src/cache/OfflineCatalog.cpp")
          == std::string::npos);
    CHECK(crossMakefile.find("src/cache/OfflineCatalog.cpp")
          == std::string::npos);
}

static void testCatalogDbMediaPageUpsertAndPopulation()
{
    std::printf("[test] CatalogDb page upsert atomicity and population states\n");
    testLibrarySyncChangedMetadataCatchUp();
    CatalogDb db;
    const auto epoch = db.configureScope("https://page-upsert.example", "user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    const CatalogDbJobMetadata metadata{0, epoch, {}};
    auto item = catalogParityItem("page-item", "movie", "The Page Item");
    CatalogDbMediaPageWrite first;
    first.items = {item}; first.viewId = "movies"; first.viewName = "Movies";
    first.collectionType = "movies"; first.ordinalStart = 4; first.finalPage = false;
    auto written = db.upsertMediaPage(first, metadata).get();
    CHECK(written.success && written.workerOwned && written.rowsWritten == 1);
    CHECK(db.populationStatus().state == CatalogDbPopulationState::Populating);
    auto again = db.upsertMediaPage(first, metadata).get();
    CHECK(again.success && again.rowsWritten == 1);
    item.title = "Updated Page Item";
    CatalogDbMediaPageWrite final = first; final.items = {item}; final.finalPage = true;
    CHECK(db.upsertMediaPage(final, metadata).get().success);
    CHECK(db.populationStatus().state == CatalogDbPopulationState::Ready);
    auto snapshot = CatalogCompatibility::readLibrarySnapshot(
        db, metadata).get();
    CHECK(snapshot.success && snapshot.snapshot.movies.size() == 1
          && snapshot.snapshot.movies[0].items.size() == 1
          && snapshot.snapshot.movies[0].items[0].title == "Updated Page Item");
    CatalogDbMediaPageWrite failing = first;
    failing.items = {catalogParityItem("rollback-a", "movie", "A"),
                     catalogParityItem("rollback-b", "movie", "B")};
    auto failed = db.upsertMediaPageForTest(failing, 1, metadata).get();
    CHECK(!failed.success && failed.error == CatalogDbErrorCategory::SqliteError);
    auto after = db.readMediaPage("movie", -1, 64, {}, metadata).get();
    CHECK(after.success && std::none_of(after.items.begin(), after.items.end(),
        [](const MediaItem &v) { return v.id == "rollback-a" || v.id == "rollback-b"; }));
    auto cancel = std::make_shared<std::atomic_bool>(true);
    auto cancelled = db.upsertMediaPage(final, {0, epoch, cancel}).get();
    CHECK(cancelled.cancelled && !cancelled.success);
    std::printf("[test] CatalogDb page upsert atomicity and population states OK\n");
}

static void testHomeUsesCatalogBeforeNetworkRefresh()
{
    std::printf("[test] Home uses CatalogDb before network refresh\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreen.cpp");
    const std::string syncSource = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(syncSource.find("configureScope") == std::string::npos);
    CHECK(syncSource.find("scopeEpoch == 0") == std::string::npos);
    CHECK(source.find("requestFetch(SDL_GetTicks())") != std::string::npos);
    CHECK(source.find("first_useful_home_ready") == std::string::npos);
    const auto enter = source.find("void HomeScreen::enter()");
    const auto leave = source.find("void HomeScreen::leave()", enter);
    CHECK(enter != std::string::npos && leave != std::string::npos);
    if (enter != std::string::npos && leave != std::string::npos)
        CHECK(source.substr(enter, leave - enter).find(
                  "m_loadState = LoadState::Ready") == std::string::npos);

    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url =
        "https://sqlite-warm-home-" + suffix + ".example";
    const std::string user = "warm-home-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch = db->configureScope(url, user);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = url;
        session.userId = user;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);
        CHECK(sync->begin(1).get().success);

        CatalogDbMediaPageWrite moviePage;
        moviePage.items = {catalogParityItem(
            "__task19_warm_movie__", "movie", "Warm Movie")};
        moviePage.viewId = "warm-movies";
        moviePage.viewName = "Warm Movies";
        moviePage.collectionType = "movies";
        moviePage.syncGeneration = 1;
        CHECK(sync->stage(moviePage).get().success);

        CatalogDbMediaPageWrite showPage;
        showPage.items = {catalogParityItem(
            "__task19_warm_show__", "show", "Warm Show")};
        showPage.viewId = "warm-shows";
        showPage.viewName = "Warm Shows";
        showPage.collectionType = "tvshows";
        showPage.syncGeneration = 1;
        CHECK(sync->stage(showPage).get().success);
        CHECK(sync->finalize(1).get().success);
    }

    {
        auto reopened = std::make_shared<CatalogDb>();
        const auto epoch = reopened->configureScope(url, user);
        CHECK(reopened->waitForIdleForTest(std::chrono::seconds(2)));
        CHECK(reopened->scopeState().openState == CatalogDbOpenState::SupportedV3);
        Session session;
        session.serverUrl = url;
        session.userId = user;
        {
            library::LibraryCoordinator coordinator(session, reopened, epoch);
            const auto query = coordinator.query();
            const auto movies = query->movies(-1, 8).get();
            const auto shows = query->shows(-1, 8).get();
            CHECK(movies.success && movies.items.size() == 1
                  && movies.items[0].id == "__task19_warm_movie__");
            CHECK(shows.success && shows.items.size() == 1
                  && shows.items[0].id == "__task19_warm_show__");
            CHECK(movies.membershipsByItem.at("__task19_warm_movie__").size() == 1);
            CHECK(shows.membershipsByItem.at("__task19_warm_show__").size() == 1);

            auto sync = std::make_shared<library::LibrarySync>(
                session, reopened, epoch);
            CHECK(sync->begin(2).get().success);
            CHECK(sync->abort(2).get().success);
            const auto afterFailure = query->movies(-1, 8).get();
            CHECK(afterFailure.success && afterFailure.items.size() == 1
                  && afterFailure.items[0].id == "__task19_warm_movie__");
        }
    }
    removeCatalogMigrationTestPaths(paths);
    std::printf("[test] Home uses CatalogDb before network refresh OK\n");
}

static void testLibraryQueryDomainBoundary()
{
    std::printf("[test] LibraryQuery domain boundary\n");
    const std::string header = readTestBytes("src/library/LibraryQuery.hpp");
    static_assert(!std::is_constructible<library::LibraryQuery,
                                         std::shared_ptr<CatalogDb>,
                                         std::uint64_t>::value,
                  "ordinary consumers must not construct LibraryQuery");
    CHECK(sourceContains(header, "friend class LibraryCoordinator"));
    for (const char *leaked : {"CatalogDbPageCursor",
                               "CatalogDbMediaPageMembership",
                               "CatalogDbErrorCategory",
                               "CatalogDbMediaPageResult",
                               "CatalogDbHierarchyResult",
                               "CatalogDbJobMetadata"}) {
        CHECK(header.find(leaked) == std::string::npos);
    }
    CHECK(header.find("LibraryPageCursor") != std::string::npos);
    CHECK(header.find("LibraryMembership") != std::string::npos);
    CHECK(header.find("LibraryQueryErrorCategory") != std::string::npos);
    const std::string coordinatorSource =
        readTestBytes("src/library/LibraryCoordinator.cpp");
    CHECK(sourceCount(coordinatorSource, "new LibraryQuery(") == 1);
    CHECK(sourceContains(coordinatorSource,
                         "m_query(std::shared_ptr<LibraryQuery>(new LibraryQuery(db, scopeEpoch)))"));
    const std::string querySource = readTestBytes("src/library/LibraryQuery.cpp");
    CHECK(sourceCount(querySource, "LibraryQuery::LibraryQuery(") == 1);
    for (const auto &entry : std::filesystem::recursive_directory_iterator("src")) {
        if (!entry.is_regular_file() || entry.path().extension() != ".cpp")
            continue;
        const std::string path = entry.path().generic_string();
        if (path == "src/library/LibraryCoordinator.cpp"
            || path == "src/library/LibraryQuery.cpp")
            continue;
        const std::string source = stripSourceComments(readTestBytes(path));
        CHECK(sourceLacks(source, "make_shared<LibraryQuery>"));
        CHECK(sourceLacks(source, "make_shared<library::LibraryQuery>"));
        CHECK(sourceLacks(source, "new LibraryQuery"));
        CHECK(sourceLacks(source, "new library::LibraryQuery"));
        CHECK(sourceLacks(source, "library::LibraryQuery query("));
        CHECK(sourceLacks(source, "miyoofin::library::LibraryQuery query("));
    }
    static_assert(!std::is_same<library::LibraryPageCursor,
                                CatalogDbPageCursor>::value,
                  "LibraryQuery cursor must be a domain type");
    static_assert(!std::is_same<library::LibraryMembership,
                                CatalogDbMediaPageMembership>::value,
                  "LibraryQuery membership must be a domain type");
    static_assert(!std::is_same<library::LibraryQueryErrorCategory,
                                CatalogDbErrorCategory>::value,
                  "LibraryQuery errors must be a domain type");

    const std::string url = "https://library-query-domain-boundary.example";
    const std::string user = "library-query-domain-boundary-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(url, user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));

    LibrarySnapshot snapshot;
    snapshot.movies.push_back({
        "movies", "Movies", "movies",
        {catalogParityItem("__library_query_boundary_a__", "movie", "First"),
         catalogParityItem("__library_query_boundary_b__", "movie", "Second")}});
    CHECK(CatalogCompatibility::seedLibrarySnapshot(
              *db, snapshot, {0, epoch, {}}).get().success);

    Session session;
    session.serverUrl = url;
    session.userId = user;
    {
        library::LibraryCoordinator coordinator(session, db, epoch);
        const auto query = coordinator.query();
        const library::LibraryPageCursor initial;
        const auto first = query->movies(-1, 1, initial).get();
        CHECK(first.success && first.items.size() == 1 && first.hasMore
              && first.next.valid);
        CHECK(first.membershipsByItem.at(first.items.front().id).size() == 1);
        if (!first.items.empty()) {
            const auto &membership = first.membershipsByItem.at(first.items.front().id).front();
            CHECK(membership.viewId == "movies");
            CHECK(membership.viewName == "Movies");
            CHECK(membership.collectionType == "movies");
        }

        const auto second = query->movies(-1, 1, first.next).get();
        CHECK(second.success && second.items.size() == 1 && !second.hasMore
              && second.next.valid);
        CHECK(second.items.front().id != first.items.front().id);

        const auto invalid = query->movies(-1, 0).get();
        CHECK(!invalid.success
              && invalid.error == library::LibraryQueryErrorCategory::SqliteError);
        auto cancellation = std::make_shared<std::atomic_bool>(true);
        const auto cancelled = query->itemsByIds({first.items.front().id}, cancellation).get();
        CHECK(cancelled.cancelled && !cancelled.success
              && cancelled.error == library::LibraryQueryErrorCategory::Superseded);
    }

    db.reset();
    removeCatalogMigrationTestPaths(paths);
    std::printf("[test] LibraryQuery domain boundary OK\n");
}
