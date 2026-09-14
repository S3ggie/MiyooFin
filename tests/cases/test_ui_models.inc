// Pure Home/UI model coverage: sync scheduling, settings rows, tab
// projection, and artwork planning. These models drive screen behavior but
// are independent of SDL rendering, so they are tested directly here.

static MediaItem makeHomeItem(const std::string &id, const std::string &title,
                              const std::string &type = "")
{
    MediaItem item;
    item.id = id;
    item.title = title;
    item.type = type;
    return item;
}

static void testHomeSyncSchedule()
{
    std::printf("[test] Home library sync schedule coalesce/throttle\n");
    LibrarySyncSchedule schedule;
    CHECK(!schedule.inFlight);
    CHECK(schedule.request(1000));
    CHECK(schedule.inFlight);
    CHECK(schedule.hasAttempted);
    CHECK(schedule.lastAttempt == 1000u);

    // Navigation while a request is in flight coalesces instead of queueing work.
    CHECK(!schedule.request(2000));
    CHECK(schedule.pending);
    CHECK(!schedule.complete(3000, true));
    CHECK(!schedule.inFlight);
    CHECK(!schedule.pending);
    CHECK(schedule.hasSucceeded);
    CHECK(schedule.lastSuccess == 3000u);

    // A successful sync is fresh for FRESH_MS.
    CHECK(!schedule.request(3000 + LibrarySyncSchedule::FRESH_MS - 1));
    CHECK(!schedule.inFlight);
    CHECK(schedule.request(3000 + LibrarySyncSchedule::FRESH_MS));
    CHECK(schedule.inFlight);

    // A failed attempt waits out RETRY_DELAY_MS before retrying.
    LibrarySyncSchedule retry;
    CHECK(retry.request(500));
    CHECK(!retry.complete(500, false));
    CHECK(!retry.hasSucceeded);
    CHECK(!retry.request(500 + LibrarySyncSchedule::RETRY_DELAY_MS - 1));
    CHECK(retry.request(500 + LibrarySyncSchedule::RETRY_DELAY_MS));
    std::printf("[test] Home library sync schedule coalesce/throttle OK\n");
}

static void testShowsSyncProgress()
{
    std::printf("[test] Shows sync progress percent clamping\n");
    ShowsSyncProgress progress;
    CHECK(progress.percent() == 100); // unknown total reports complete

    progress.completed = 3;
    progress.total = 10;
    CHECK(progress.percent() == 30);
    CHECK(progress.percent(50) == 50);   // monotonic against previous
    CHECK(progress.percent(80) == 80);
    progress.completed = 13;             // over-complete is bounded
    CHECK(progress.percent() == 100);
    progress.completed = 0;
    CHECK(progress.percent(100) == 100); // never regresses
    CHECK(progress.percent(0) == 0);
    std::printf("[test] Shows sync progress percent clamping OK\n");
}

static void testHomeSyncStatusStrings()
{
    std::printf("[test] Home sync status strings\n");
    CHECK(!homeChangeNeedsPublication(false, false, false));
    CHECK(homeChangeNeedsPublication(true, false, false));
    CHECK(homeChangeNeedsPublication(false, true, false));
    CHECK(homeChangeNeedsPublication(false, false, true));

    CHECK_EQ(homeSyncStatus(true), "HOME SYNCING...");
    CHECK_EQ(homeSyncStatus(false), "");

    CHECK_EQ(librarySyncStatus(-1, true, false, false, false), "");
    CHECK_EQ(librarySyncStatus(3, true, false, false, false), "");
    CHECK_EQ(librarySyncStatus(0, true, true, true, false), "OFFLINE");
    CHECK_EQ(librarySyncStatus(0, false, true, false, false), "SYNCING...");

    const ShowsSyncProgress mid{2, 4};
    CHECK_EQ(librarySyncStatus(1, true, false, false, false, mid), "SYNC 50%");
    CHECK_EQ(librarySyncStatus(1, true, false, true, false), "SYNCING...");
    CHECK_EQ(librarySyncStatus(1, true, false, false, true), "SYNCED");
    CHECK_EQ(librarySyncStatus(1, true, false, false, false), "SYNCING...");

    // hierarchyActive keeps the percentage visible after metadata settles.
    const ShowsSyncProgress done{4, 4};
    CHECK_EQ(librarySyncStatus(2, true, false, false, true, done, true), "SYNC 100%");

    CHECK_EQ(artworkSyncStatus(false, true, mid), "");
    CHECK_EQ(artworkSyncStatus(true, false, mid), "");
    CHECK_EQ(artworkSyncStatus(true, true, {}), "");
    CHECK_EQ(artworkSyncStatus(true, true, mid), "ART 50%");
    std::printf("[test] Home sync status strings OK\n");
}

static void testHomeSettingsModel()
{
    std::printf("[test] Home settings address rows and actions\n");
    CHECK(homeSettingsBaseRowCount() == 9);

    Session lan;
    lan.serverUrl = "http://192.168.1.5:8096";
    const auto lanRows = homeSettingsAddressRows(lan);
    CHECK(lanRows.size() == 2);
    CHECK_EQ(lanRows[0].section, "LAN Server");
    CHECK_EQ(lanRows[0].value, lan.serverUrl);
    CHECK(lanRows[0].action == HomeSettingsRowAction::ChangeServer);
    CHECK_EQ(lanRows[1].section, "Public Address");
    CHECK_EQ(lanRows[1].value, "Not Set");
    CHECK(lanRows[1].action == HomeSettingsRowAction::PublicAddress);
    CHECK(homeSettingsRowCount(lan) == 10);
    CHECK(homeSettingsRowAction(0, lan) == HomeSettingsRowAction::OfflineMode);
    CHECK(homeSettingsRowAction(1, lan) == HomeSettingsRowAction::ChangeServer);
    CHECK(homeSettingsRowAction(2, lan) == HomeSettingsRowAction::PublicAddress);
    CHECK(homeSettingsRowAction(9, lan) == HomeSettingsRowAction::Logout);
    CHECK(homeSettingsRowAction(5, lan) == HomeSettingsRowAction::None);

    Session pub;
    pub.serverUrl = "https://jellyfin.example.com";
    pub.localServerUrl = "http://192.168.1.5:8096";
    const auto pubRows = homeSettingsAddressRows(pub);
    CHECK(pubRows.size() == 2);
    CHECK_EQ(pubRows[0].section, "Public Server");
    CHECK_EQ(pubRows[1].section, "Local Address");
    CHECK(pubRows[1].action == HomeSettingsRowAction::LocalAddress);
    CHECK(homeSettingsRowAction(2, pub) == HomeSettingsRowAction::LocalAddress);

    Session empty;
    const auto emptyRows = homeSettingsAddressRows(empty);
    CHECK_EQ(emptyRows[0].value, "Not connected");
    CHECK_EQ(emptyRows[1].value, "Not Set");
    CHECK(homeSettingsRowAction(0) == HomeSettingsRowAction::OfflineMode);
    CHECK(homeSettingsRowAction(2) == HomeSettingsRowAction::LocalAddress);
    CHECK(homeSettingsRowAction(8) == HomeSettingsRowAction::Logout);
    CHECK(homeSettingsRowAction(3) == HomeSettingsRowAction::None);
    std::printf("[test] Home settings address rows and actions OK\n");
}

static void testHomeTabsProjection()
{
    std::printf("[test] Home tab projection and media windows\n");
    LibrarySnapshot snap;
    CachedLibraryView movies;
    movies.id = "m1";
    movies.name = "Movies";
    movies.collectionType = "movies";
    movies.items = {makeHomeItem("m1", "Beta", "movie"),
                    makeHomeItem("m2", "Alpha", "movie"),
                    makeHomeItem("m1", "Beta duplicate", "movie")};
    CachedLibraryView shows;
    shows.id = "s1";
    shows.name = "Shows";
    shows.items = {makeHomeItem("sh1", "Show", "show")};
    snap.movies = {movies};
    snap.shows = {shows};
    snap.continueWatching = {makeHomeItem("cw1", "Continue", "movie")};
    snap.recentlyAdded = {makeHomeItem("ra1", "Recent", "movie")};

    const auto tabs = tabsFromSnapshot(snap);
    const auto names = tabNames(tabs);
    CHECK(names.size() == 5);
    CHECK_EQ(names[0], "Home");
    CHECK_EQ(names[1], "Movies");
    CHECK_EQ(names[2], "Shows");
    CHECK_EQ(names[3], "Downloads");
    CHECK_EQ(names[4], "Settings");
    CHECK(tabs[0].rows.size() == 2);
    CHECK_EQ(tabs[0].rows[0].label, "Continue Watching");
    CHECK_EQ(tabs[0].rows[1].label, "Recently Added");
    CHECK_EQ(tabs[1].rows[0].label, "Movies");
    CHECK(tabs[1].rows[0].items.size() == 2); // duplicate id collapsed
    CHECK_EQ(tabs[2].rows[0].label, "Shows");
    CHECK(tabs[2].rows[0].items.size() == 1);

    const HomeMediaWindows windows = mediaWindowsFromTabs(tabs);
    CHECK(windows.movies.size() == 2);
    CHECK(windows.shows.size() == 1);

    const auto offline = offlineTabsFromSnapshot(snap);
    const auto offlineNames = tabNames(offline);
    CHECK(offlineNames.size() == 4);
    CHECK_EQ(offlineNames[0], "Movies");
    CHECK_EQ(offlineNames[3], "Settings");

    const auto emptyTabs = tabsFromSnapshot(LibrarySnapshot{});
    CHECK(emptyTabs[0].rows.size() == 1);
    CHECK_EQ(emptyTabs[0].rows[0].label, "");
    CHECK_EQ(emptyTabs[1].rows[0].label, "Movies");
    CHECK(emptyTabs[1].rows[0].items.empty());
    CHECK_EQ(emptyTabs[2].rows[0].label, "No shows found");
    std::printf("[test] Home tab projection and media windows OK\n");
}

static void testHomeTabRowUpdates()
{
    std::printf("[test] Home rail insertion and removal order\n");
    std::vector<TabData> tabs = {{"Home", {{"Recently Added", {makeHomeItem("ra1", "R")}}}}};
    updateContinueWatchingRow(tabs, {makeHomeItem("cw1", "C")});
    CHECK(tabs[0].rows.size() == 2);
    CHECK_EQ(tabs[0].rows[0].label, "Continue Watching");
    CHECK_EQ(tabs[0].rows[1].label, "Recently Added");

    updateRecentlyAddedRow(tabs, {makeHomeItem("ra2", "R2")});
    CHECK(tabs[0].rows.size() == 2);
    CHECK(tabs[0].rows[1].items.size() == 1);
    CHECK_EQ(tabs[0].rows[1].items[0].id, "ra2");

    // Removing the only rail restores the empty placeholder row.
    std::vector<TabData> onlyCw = {{"Home", {{"Continue Watching", {makeHomeItem("cw1", "C")}}}}};
    updateContinueWatchingRow(onlyCw, {});
    CHECK(onlyCw[0].rows.size() == 1);
    CHECK_EQ(onlyCw[0].rows[0].label, "");

    // Without a Home tab both updates are no-ops.
    std::vector<TabData> noHome = {{"Movies", {{"Movies", {}}}}};
    updateContinueWatchingRow(noHome, {makeHomeItem("x", "X")});
    updateRecentlyAddedRow(noHome, {makeHomeItem("y", "Y")});
    CHECK(noHome.size() == 1);
    CHECK_EQ(noHome[0].name, "Movies");
    CHECK(noHome[0].rows[0].items.empty());
    std::printf("[test] Home rail insertion and removal order OK\n");
}

static void testTransitionTabIndex()
{
    std::printf("[test] Home tab transition index selection\n");
    LibrarySnapshot snap;
    snap.movies = {{"m1", "Movies", "movies", {makeHomeItem("m1", "M", "movie")}}};
    const auto from = tabsFromSnapshot(snap);
    const int showsIndex = 2;
    CHECK_EQ(from[showsIndex].name, "Shows");

    const std::vector<TabData> reordered = {{"Settings", {}}, {"Shows", {}}, {"Home", {}}};
    CHECK(transitionTabIndex(from, showsIndex, reordered) == 1);

    const std::vector<TabData> noShows = {{"Home", {}}, {"Movies", {}}};
    CHECK(transitionTabIndex(from, showsIndex, noShows) == 1); // falls back to Movies

    const std::vector<TabData> onlyHome = {{"Home", {}}, {"Settings", {}}};
    CHECK(transitionTabIndex(from, 100, onlyHome) == 1); // clamps out-of-range selection
    CHECK(transitionTabIndex(from, -5, onlyHome) == 0);
    std::printf("[test] Home tab transition index selection OK\n");
}

static void testHomeArtworkPlan()
{
    std::printf("[test] Home artwork key and poster planning\n");
    MediaItem movie = makeHomeItem("movie-1", "Movie", "movie");
    movie.imageTags["Primary"] = "tag-a";
    CHECK_EQ(homeArtworkKey(movie), "movie-1:Primary:tag-a:64x96");

    MediaItem episode = makeHomeItem("ep-1", "Episode", "episode");
    episode.imageTags["Thumb"] = "thumb-a";
    episode.imageTags["Primary"] = "primary-a";
    CHECK_EQ(homeArtworkKey(episode), "ep-1:Thumb:thumb-a:128x72");

    MediaItem noArt = makeHomeItem("none-1", "None", "movie");
    CHECK_EQ(homeArtworkKey(noArt), "");

    MediaItem season = makeHomeItem("season-1", "Season 1", "season");
    season.imageTags["Primary"] = "s-tag";
    MediaItem untyped = makeHomeItem("season-2", "Season 2", "");
    untyped.imageTags["Primary"] = "s-tag-2";
    MediaItem wrongType = makeHomeItem("movie-2", "Movie 2", "movie");
    wrongType.imageTags["Primary"] = "m-tag";
    MediaItem missingTag = makeHomeItem("season-3", "Season 3", "season");
    const auto seasonJobs = planSeasonPosterJobs({season, untyped, wrongType, missingTag});
    CHECK(seasonJobs.size() == 2);
    CHECK_EQ(seasonJobs[0].itemId, "season-1");
    CHECK_EQ(seasonJobs[0].imageTag, "s-tag");
    CHECK(seasonJobs[0].width == 74 && seasonJobs[0].height == 111);
    CHECK_EQ(seasonJobs[1].itemId, "season-2");

    // Rails deduplicate across both source rows.
    const auto railJobs = planHomeRailPosterJobs({movie, episode}, {movie});
    CHECK(railJobs.size() == 2);

    // Snapshot planning deduplicates shared keys across views and rails.
    LibrarySnapshot snap;
    snap.movies = {{"m1", "Movies", "movies", {movie, movie}}};
    snap.continueWatching = {movie};
    snap.recentlyAdded = {noArt};
    const auto snapshotJobs = planHomePosterJobs(snap);
    CHECK(snapshotJobs.size() == 1);
    CHECK_EQ(snapshotJobs[0].itemId, "movie-1");
    std::printf("[test] Home artwork key and poster planning OK\n");
}
