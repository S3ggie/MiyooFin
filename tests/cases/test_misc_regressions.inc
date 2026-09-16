#include "../../src/net/JellyfinLibraryEvents.hpp"
#include "../../src/update/AppDir.hpp"
#include "../../src/catalog/CatalogDbInternal.hpp"

static void testHomeIncrementalPublicationPolicy()
{
    std::printf("[test] Home incremental publication policy\n");
    CHECK(homeChangeNeedsPublication(false, true, false));
    CHECK(homeChangeNeedsPublication(false, false, true));
    CHECK(homeChangeNeedsPublication(true, false, false));
    CHECK(!homeChangeNeedsPublication(false, false, false));
    CHECK_EQ(std::string(homeSyncStatus(true)), "HOME SYNCING...");
    CHECK_EQ(std::string(homeSyncStatus(false)), "");
    const std::string render = readTestBytes("src/ui/screens/HomeScreenRender.cpp");
    CHECK(render.find("homeSyncStatus(true)") != std::string::npos);
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(sync.find("updateLiveLibraryChanges()") != std::string::npos);
    CHECK(sync.find("catchUpChangedCatalog") != std::string::npos);
    CHECK(sync.find("reconcileAuthoritativeMembership") != std::string::npos);
    CHECK(apply.find("m_lastSafetyReconcileMs") != std::string::npos);
    CHECK(apply.find("m_cachedSnapshot.continueWatching") != std::string::npos);
    std::printf("[test] Home incremental publication policy OK\n");
}

static void testLibraryChangedEventQueueAndReconnectPolicy()
{
    std::printf("[test] LibraryChanged queue and reconnect policy\n");
    JellyfinLibraryEventQueue queue(3);
    JellyfinLibraryChangeBatch first;
    first.itemsAdded = {"same", "one"};
    first.itemsUpdated = {"same"};
    first.itemsRemoved = {"gone"};
    CHECK(queue.push(first));
    JellyfinLibraryChangeBatch overflow;
    overflow.itemsAdded = {"extra"};
    CHECK(!queue.push(overflow));
    CHECK(queue.overflowed());

    JellyfinLibraryChangeBatch result;
    CHECK(queue.pop(result));
    CHECK(result.itemsAdded.size() == 2);
    CHECK(result.itemsUpdated.size() == 1 && result.itemsUpdated[0] == "same");
    CHECK(result.itemsRemoved.size() == 1 && result.itemsRemoved[0] == "gone");
    CHECK(!queue.pop(result));

    JellyfinLibraryEvents::Backoff backoff(100, 800);
    CHECK(backoff.nextDelayMs() == 100);
    CHECK(JellyfinLibraryEvents::buildSocketUrl(
        "https://server.example/base/", "a b", "device/id") ==
        "wss://server.example/base/socket?api_key=a%20b&deviceId=device%2Fid");
    CHECK(backoff.nextDelayMs() == 200);
    CHECK(backoff.nextDelayMs() == 400);
    CHECK(backoff.nextDelayMs() == 800);
    CHECK(backoff.nextDelayMs() == 800);
    backoff.reset();
    CHECK(backoff.nextDelayMs() == 100);

    std::atomic<bool> cancelled{true};
    CHECK(JellyfinLibraryEvents::waitForRetry(cancelled, 1000) ==
          JellyfinLibraryEventRun::Cancelled);
    Session session;
    session.serverUrl = "https://server.example";
    session.accessToken = "token";
    session.deviceId = "device";
    JellyfinLibraryEvents events(session);
    CHECK(events.run(queue, std::make_shared<std::atomic_bool>(true)) ==
          JellyfinLibraryEventRun::Cancelled);
    std::printf("[test] LibraryChanged queue and reconnect policy OK\n");
}

static void testRouteRequest()
{
    std::printf("[test] LAN route selection and fallback\n");
    RouteStatus::record(ApiRoute::Unknown);
    CHECK(RouteStatus::latest()==ApiRoute::Unknown);
    Session s; s.serverUrl="https://public";
    std::string error; int calls=0;
    CHECK(RouteRequest(s).run([&](const std::string &base){++calls;return base=="https://public";},error));
    CHECK(RouteStatus::latest()==ApiRoute::Public);
    CHECK(calls==1 && RouteRequest(s).primary()=="https://public");
    s.localServerUrl="http://lan"; calls=0; error.clear();
    CHECK(RouteRequest(s).run([&](const std::string &base){++calls;return base=="http://lan";},error));
    CHECK(calls==1 && RouteRequest(s).primary()=="http://lan");
    CHECK(RouteStatus::latest()==ApiRoute::Lan);
    calls=0; error.clear();
    CHECK(RouteRequest(s).run([&](const std::string &base){++calls;if(base=="http://lan"){error="Transport: Couldn't connect";return false;}return base=="https://public";},error));
    CHECK(calls==2);
    CHECK(RouteStatus::latest()==ApiRoute::Public);
    calls=0; error.clear();
    CHECK(!RouteRequest(s).run([&](const std::string &){
        ++calls;
        error="Transport: Couldn't connect";
        return false;
    },error));
    CHECK(calls==2);
    CHECK(RouteStatus::latest()==ApiRoute::Public);
    calls=0; error.clear();
    CHECK(!RouteRequest(s).run([&](const std::string &){++calls;error="Unauthorized";return false;},error));
    CHECK(calls==1);
    calls=0; error.clear();
    CHECK(!RouteRequest(s).run([&](const std::string &){++calls;error="Transport: Callback aborted";return false;},error));
    CHECK(calls==1); // cancellation must never launch a public retry
    CHECK(RouteStatus::latest()==ApiRoute::Public);
    const std::string scope=LibraryCache::scopeKey(s.serverUrl,"user");
    CHECK_EQ(scope,LibraryCache::scopeKey("https://public","user"));
    CHECK_EQ(RouteRequest::replaceBase("http://lan/a","http://lan","https://public"),"https://public/a");
    Session lanCanonical; lanCanonical.serverUrl="http://192.168.1.212:8096";
    lanCanonical.publicServerUrl="https://public.example.com";
    const std::string canonical=lanCanonical.serverUrl;
    const std::string lanScope=LibraryCache::scopeKey(lanCanonical.serverUrl,"user");
    calls=0; error.clear();
    CHECK(RouteRequest(lanCanonical).run([&](const std::string &base){++calls;if(base==canonical){error="Transport: Couldn't connect";return false;}return base=="https://public.example.com";},error));
    CHECK(calls==2);
    CHECK_EQ(lanCanonical.serverUrl,canonical);
    CHECK_EQ(LibraryCache::scopeKey(lanCanonical.serverUrl,"user"),lanScope);

    RouteStatus::record(ApiRoute::Unknown);
    Session lanOnly; lanOnly.serverUrl="http://192.168.1.212:8096";
    CHECK(RouteRequest(lanOnly).run([&](const std::string &base){return base=="http://192.168.1.212:8096";},error));
    CHECK(RouteStatus::latest()==ApiRoute::Lan);

    RouteStatus::record(ApiRoute::Unknown);
    CHECK_EQ(std::string(HomeScreen::lastApiRouteValue()),"UNKNOWN");
    RouteStatus::record(ApiRoute::Lan);
    CHECK_EQ(std::string(HomeScreen::lastApiRouteValue()),"LAN");
    RouteStatus::record(ApiRoute::Public);
    CHECK_EQ(std::string(HomeScreen::lastApiRouteValue()),"PUBLIC");
}


static MediaItem titledMovie(const std::string &id, const std::string &title)
{
    MediaItem item;
    item.id = id;
    item.title = title;
    item.type = "movie";
    return item;
}

static void testMovieOrganizationalTitles()
{
    std::printf("[test] movie organizational titles\n");
    CHECK_EQ(movieOrganizationalTitle("The Amazing Spider-Man"), "Amazing Spider-Man");
    CHECK_EQ(movieOrganizationalTitle("The Batman"), "Batman");
    CHECK_EQ(movieOrganizationalTitle("the matrix"), "matrix");
    CHECK_EQ(movieOrganizationalTitle("THE MATRIX"), "MATRIX");
    CHECK_EQ(movieOrganizationalTitle("The Thing"), "Thing");
    CHECK_EQ(movieOrganizationalTitle("The"), "The");
    CHECK_EQ(movieOrganizationalTitle("There Will Be Blood"), "There Will Be Blood");
    CHECK_EQ(movieOrganizationalTitle("Theater Camp"), "Theater Camp");
    CHECK_EQ(movieOrganizationalTitle("The "), "The ");
    CHECK_EQ(movieOrganizationalTitle(""), "");
}

static void testMovieAlphabetOrganization()
{
    std::printf("[test] movie alphabet organization\n");
    CHECK(movieMatchesAlphabetFilter("The Amazing Spider-Man", 0));
    CHECK(movieMatchesAlphabetFilter("The Batman", 1));
    CHECK(movieMatchesAlphabetFilter("The Dark Knight", 3));
    CHECK(movieMatchesAlphabetFilter("The Lord of the Rings", 11));
    CHECK(movieMatchesAlphabetFilter("The", 19));
    CHECK(movieMatchesAlphabetFilter("The Thing", 19));
    CHECK(movieMatchesAlphabetFilter("There Will Be Blood", 19));
    CHECK(movieMatchesAlphabetFilter("Theater Camp", 19));
    CHECK(!movieMatchesAlphabetFilter("The Amazing Spider-Man", 19));
}

static void testMovieOrganizationalSort()
{
    std::printf("[test] movie organizational sort\n");
    std::vector<MediaItem> movies = {
        titledMovie("1", "The Batman"), titledMovie("2", "Alien"),
        titledMovie("3", "The Amazing Spider-Man"), titledMovie("4", "Avatar"),
        titledMovie("5", "The Dark Knight"), titledMovie("6", "Batman"),
        titledMovie("7", "The Lord of the Rings")};
    std::sort(movies.begin(), movies.end(), movieOrganizationalLess);
    const std::vector<std::string> expected = {
        "Alien", "The Amazing Spider-Man", "Avatar", "Batman", "The Batman",
        "The Dark Knight", "The Lord of the Rings"};
    for (size_t i = 0; i < expected.size(); ++i) CHECK_EQ(movies[i].title, expected[i]);
}

static void testMovieAlphabetFocus()
{
    std::printf("[test] movie alphabet focus\n");
    CHECK(movieAlphabetFocus("The Amazing Spider-Man") == 0);
    CHECK(movieAlphabetFocus("The Batman") == 1);
    CHECK(movieAlphabetFocus("The") == 19);
    CHECK(movieAlphabetFocus("There Will Be Blood") == 19);
    CHECK(movieAlphabetFocus("1917") == 0);
}

static MediaItem titledShow(const std::string &id, const std::string &title, const std::string &genre="") { MediaItem i; i.id=id;i.title=title;i.type="show";if(!genre.empty())i.genres={genre};return i; }
static void testBoundedPagePosterPlanning()
{
    std::printf("[test] bounded media page poster planning\n");
    MediaItem show=titledShow("show-page","Show page");
    show.imageTags["Primary"]="show-tag";
    MediaItem anime=titledShow("anime-page","Anime page");
    anime.imageTags["Primary"]="anime-tag";
    MediaItem movie; movie.id="movie-page"; movie.title="Movie page";
    movie.type="movie"; movie.imageTags["Primary"]="movie-tag";
    MediaItem missing=titledShow("missing-page","No poster");
    MediaItem empty=titledShow("empty-page","Empty tag");
    empty.imageTags["Primary"]="";
    const auto jobs=planMediaPagePosterJobs({show,anime,movie,missing,empty});
    CHECK(jobs.size()==3);
    CHECK(jobs[0].itemId=="anime-page" && jobs[1].itemId=="movie-page"
          && jobs[2].itemId=="show-page");
    CHECK(jobs[0].width==64 && jobs[0].height==96
          && jobs[1].width==64 && jobs[1].height==96
          && jobs[2].width==64 && jobs[2].height==96);
    MediaItem theBatman=titledMovie("the-batman", "The Batman");
    theBatman.imageTags["Primary"]="the-batman-tag";
    MediaItem alien=titledMovie("alien", "Alien");
    alien.imageTags["Primary"]="alien-tag";
    MediaItem theAmazing=titledMovie("the-amazing", "The Amazing Spider-Man");
    theAmazing.imageTags["Primary"]="the-amazing-tag";
    const auto ordered=planMediaPagePosterJobs({theBatman, alien, theAmazing});
    CHECK(ordered.size()==3);
    CHECK(ordered[0].itemId=="alien"
          && ordered[1].itemId=="the-amazing"
          && ordered[2].itemId=="the-batman");
    std::printf("[test] bounded media page poster planning OK\n");
}

static void testHomeRailPosterPlanning()
{
    std::printf("[test] Home rail poster planning\n");
    MediaItem cw=titledMovie("cw-poster", "Continue");
    cw.imageTags["Primary"]="cw-tag";
    MediaItem recent=titledMovie("recent-poster", "Recent");
    recent.imageTags["Primary"]="recent-tag";
    MediaItem duplicate=cw;
    MediaItem missing=titledMovie("no-poster", "No poster");
    const auto jobs=planHomeRailPosterJobs({cw}, {recent, duplicate, missing});
    CHECK(jobs.size()==2);
    CHECK(jobs[0].itemId=="cw-poster" && jobs[1].itemId=="recent-poster");
    CHECK(jobs[0].width==64 && jobs[0].height==96
          && jobs[1].width==64 && jobs[1].height==96);
    std::printf("[test] Home rail poster planning OK\n");
}

static void testInitialHomeArtworkScheduling()
{
    std::printf("[test] initial Home artwork scheduling\n");
    const std::string source=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(source.find("queuePosterJobs(planHomeRailPosterJobs(cw, ra), true)")
          != std::string::npos);
    CHECK(source.find("if (start == 0)\n                        queuePosterJobs(planMediaPagePosterJobs(page.items), true)")
          == std::string::npos);
    CHECK(source.find("queuePosterJobs(planMediaPagePosterJobs(page.items), start == 0)")
          != std::string::npos);
    const std::string coordinator=readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(coordinator.find("m_posterThreads.emplace_back") != std::string::npos);
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(apply.find("queuePosterJobs(planMediaPagePosterJobs(result.items), true)")
          != std::string::npos);
    CHECK(source.find("m_artworkPlanningComplete.store(false)")
          != std::string::npos);
    CHECK(source.find("m_artworkPlanningComplete.store(artworkPlanningComplete)")
          != std::string::npos);
    const std::string hierarchy=readTestBytes("src/ui/screens/HomeScreenHierarchy.cpp");
    CHECK(hierarchy.find("HttpClient client;") != std::string::npos);
    CHECK(hierarchy.find("for (;;) {\n        PosterJob job;") != std::string::npos);
    const std::string http=readTestBytes("src/net/HttpClient.hpp");
    CHECK(http.find("CURL *m_curl") != std::string::npos);
    CHECK(http.find("~HttpClient") != std::string::npos);
    std::printf("[test] initial Home artwork scheduling OK\n");
}

static void testPersistentHttpClientPatterns()
{
    std::printf("[test] persistent HttpClient patterns in worker threads\n");
    // HomeScreen fetch worker reuses one client across all metadata calls
    const std::string sync=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(sync.find("HttpClient fetchClient;") != std::string::npos);
    CHECK(sync.find("JellyfinApi::getViews(base, token, uid, devId, views,\n                                             viewsErr, fetchClient, cancellation.get())") != std::string::npos
          || sync.find("JellyfinApi::getViews(base, token, uid, devId, views,viewsErr, fetchClient, cancellation.get())") != std::string::npos
          || sync.find("fetchClient, cancellation.get())") != std::string::npos);
    CHECK(sync.find("HttpClient railClient;") != std::string::npos);
    // LibrarySync reconcile reuses one client across views + pages loop
    const std::string inc=readTestBytes("src/library/LibrarySyncIncremental.cpp");
    CHECK(inc.find("HttpClient reconcileClient;") != std::string::npos);
    // EpisodeBrowser artwork worker reuses one client
    const std::string ep=readTestBytes("src/ui/screens/EpisodeBrowserArtwork.cpp");
    CHECK(ep.find("HttpClient artworkClient;") != std::string::npos);
    CHECK(ep.find("artworkClient.setTimeoutSec(3)") != std::string::npos);
    // SeriesScreen artwork worker reuses one client
    const std::string ss=readTestBytes("src/ui/screens/SeriesScreenWorker.cpp");
    CHECK(ss.find("HttpClient artworkClient;") != std::string::npos);
    CHECK(ss.find("artworkClient.setTimeoutSec(8)") != std::string::npos);
    // JellyfinApiLibrary exposes HttpClient& overloads
    const std::string api=readTestBytes("src/net/JellyfinApiLibrary.cpp");
    CHECK(api.find("HttpClient &client,") != std::string::npos);
    CHECK(api.find("return getViews(baseUrl, accessToken, userId, deviceId, views, error,\n                    client, cancelled)") != std::string::npos
          || api.find("return getViews(baseUrl, accessToken, userId, deviceId, views, error,client, cancelled)") != std::string::npos
          || api.find("client, cancelled)") != std::string::npos);
    std::printf("[test] persistent HttpClient patterns in worker threads OK\n");
}

static void testShowsFocusPreservedDuringIncrementalRefresh()
{
    std::printf("[test] Shows/Anime focus survives incremental refresh\n");
    using Focus = HomeScreen::ShowsFocusState;
    CHECK(HomeScreen::showsFocusAfterRefresh(Focus::AnimeGrid, true, true)
          == Focus::AnimeGrid);
    CHECK(HomeScreen::showsFocusAfterRefresh(Focus::ShowsGrid, true, true)
          == Focus::ShowsGrid);
    CHECK(HomeScreen::showsFocusAfterRefresh(Focus::AnimeGrid, false, true)
          == Focus::AnimeGrid);
    CHECK(HomeScreen::showsFocusAfterRefresh(Focus::ShowsGrid, true, false)
          == Focus::ShowsGrid);
    CHECK(HomeScreen::showsFocusAfterRefresh(Focus::AnimeGrid, false, false)
          == Focus::AlphabetRail);
    CHECK(HomeScreen::showsFocusAfterRefresh(Focus::AlphabetRail, true, true)
          == Focus::ShowsGrid);
    CHECK(HomeScreen::showsFocusAfterRefresh(Focus::AlphabetRail, false, true)
          == Focus::AnimeGrid);
    std::printf("[test] Shows/Anime focus survives incremental refresh OK\n");
}

static void testHomeGridPositionHelpers()
{
    std::printf("[test] Home grid position survives incremental refresh\n");
    std::vector<MediaItem> items;
    for (const char *id : {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m"}) {
        MediaItem item; item.id=id; item.title=id; items.push_back(item);
    }
    CHECK(HomeScreen::restoreSelectionIndex(items,"j",0)==9);
    CHECK(HomeScreen::restoreSelectionIndex(items,"missing",9)==9);
    CHECK(HomeScreen::restoreSelectionIndex(items,"missing",99)==12);
    CHECK(HomeScreen::preserveGridScroll(9,20,2,4,3)==2);
    CHECK(HomeScreen::preserveGridScroll(12,20,2,4,3)==2);
    CHECK(HomeScreen::preserveGridScroll(12,20,0,4,3)==1);
    CHECK(HomeScreen::preserveGridScroll(2,3,99,4,3)==0);
    std::printf("[test] Home grid position survives incremental refresh OK\n");
}

static void testRecentlyAddedWarmPublication()
{
    std::printf("[test] Recently Added warm publication\n");
    MediaItem first; first.id="recent-first"; first.title="First";
    MediaItem replacement; replacement.id="recent-replacement"; replacement.title="Replacement";
    std::vector<TabData> tabs={{"Home", {{"Continue Watching", {first}}}}};
    updateRecentlyAddedRow(tabs,{first});
    CHECK(tabs[0].rows.size()==2);
    CHECK(tabs[0].rows[1].label=="Recently Added"
          && tabs[0].rows[1].items[0].id==first.id);
    updateRecentlyAddedRow(tabs,{replacement});
    CHECK(tabs[0].rows.size()==2
          && tabs[0].rows[1].items[0].id==replacement.id);
    updateRecentlyAddedRow(tabs,{});
    CHECK(tabs[0].rows.size()==2
          && tabs[0].rows[1].label=="Recently Added"
          && tabs[0].rows[1].items[0].id==replacement.id);
    const std::string apply=readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(apply.find("updateRecentlyAddedRow(m_tabs, m_remoteSnapshot.recentlyAdded)")
          != std::string::npos);
    std::printf("[test] Recently Added warm publication OK\n");
}

static void testWarmHomeCatalogWindows()
{
    std::printf("[test] warm Home catalog windows\n");
    const std::string source=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply=readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(source.find("initialPagePublished = true") != std::string::npos);
    CHECK(apply.find("mediaWindowsFromTabs") != std::string::npos);
    CHECK(apply.find("m_moviePage.items = warmWindows.movies")
          != std::string::npos);
    CHECK(apply.find("m_showPage.items = warmWindows.shows")
          != std::string::npos);
    CHECK(apply.find("updateContinueWatchingRow(m_tabs, m_remoteSnapshot.continueWatching)")
          != std::string::npos);
    std::printf("[test] warm Home catalog windows OK\n");
}

static void testWarmMediaWindowsExtraction()
{
    std::printf("[test] warm media windows extraction\n");
    MediaItem movie=titledMovie("warm-movie", "Warm movie");
    MediaItem show=titledShow("warm-show", "Warm show");
    std::vector<TabData> tabs={
        {"Home", {{"", {}}}},
        {"Movies", {{"Movies", {movie}}}},
        {"Shows", {{"Shows", {show}}}}};
    const HomeMediaWindows windows=mediaWindowsFromTabs(tabs);
    CHECK(windows.movies.size()==1 && windows.movies[0].id==movie.id);
    CHECK(windows.shows.size()==1 && windows.shows[0].id==show.id);
    CHECK(mediaWindowsFromTabs({{"Home", {{"", {}}}}}).movies.empty());
    CHECK(mediaWindowsFromTabs({{"Home", {{"", {}}}}}).shows.empty());
    std::printf("[test] warm media windows extraction OK\n");
}

static void testAnimeSelectionSurvivesSharedWindowTrim()
{
    std::printf("[test] Anime selection survives shared window trim\n");
    const std::string source=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply=readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(source.find("m_showSelected = std::max(0, m_showSelected -")
          == std::string::npos);
    CHECK(source.find("m_animeSelected = std::max(0, m_animeSelected -")
          == std::string::npos);
    CHECK(apply.find("rebuildShowsPresentation();") != std::string::npos);
    CHECK(HomeScreen::restoreSelectionIndex(
              {titledShow("show-a", "Show A"), titledShow("anime-b", "Anime B")},
              "anime-b", 0) == 1);
    CHECK(HomeScreen::restoreSelectionIndex(
              {titledShow("anime-c", "Anime C")}, "trimmed", 4) == 0);
    std::printf("[test] Anime selection survives shared window trim OK\n");
}

static void testHomeUsesDedicatedAnimeCatalogPage()
{
    std::printf("[test] Home uses dedicated Anime catalog page\n");
    const std::string home=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string screen=readTestBytes("src/ui/screens/HomeScreen.cpp");
    const std::string query=readTestBytes("src/library/LibraryQuery.hpp");
    const std::string catalog=readTestBytes("src/catalog/CatalogDb.hpp");
    CHECK(home.find("m_animePage") != std::string::npos);
    CHECK(home.find("m_libraryQuery->anime") != std::string::npos);
    CHECK(screen.find("m_animePage.items") != std::string::npos);
    CHECK(query.find("anime(") != std::string::npos);
    CHECK(catalog.find("CatalogDbMediaPageFilter") != std::string::npos);
    std::printf("[test] Home uses dedicated Anime catalog page OK\n");
}

static void testHomeRefreshPreservesActiveGridSelection()
{
    std::printf("[test] Home refresh preserves active grid selection\n");
    const std::string header=readTestBytes("src/ui/screens/HomeScreen.hpp");
    const std::string source=readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    CHECK(header.find("m_moviePreviewId") != std::string::npos);
    CHECK(source.find("const MediaItem *previousItem = showsSelectedItem();")
          != std::string::npos);
    CHECK(source.find("m_moviePreviewId") != std::string::npos);
    CHECK(source.find("m_showsPreviewId = item->id") != std::string::npos);
    std::printf("[test] Home refresh preserves active grid selection OK\n");
}

static void testHomeBoundedPagesCanRewind()
{
    std::printf("[test] Home bounded pages rewind without cursor jumps\n");
    const std::string header=readTestBytes("src/ui/screens/HomeScreen.hpp");
    const std::string navigation=readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string sync=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply=readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(header.find("hasEarlier") != std::string::npos);
    CHECK(header.find("replaceWindowOnNextPage") != std::string::npos);
    CHECK(navigation.find("requestEarlierMediaPage") != std::string::npos);
    CHECK(sync.find("replaceWindowOnNextPage") != std::string::npos);
    CHECK(apply.find("hasEarlier = true") != std::string::npos);
    std::printf("[test] Home bounded pages rewind without cursor jumps OK\n");
}

static void testHomeUsesSharedGridRewindEdge()
{
    std::printf("[test] Home uses shared grid rewind edge\n");
    const std::string source=readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    CHECK(source.find("gridAtTopRow") != std::string::npos);
    CHECK(source.find("gridAtTopRow(m_showSelected") != std::string::npos);
    CHECK(source.find("gridAtTopRow(m_animeSelected") != std::string::npos);
    CHECK(source.find("gridAtTopRow(m_activeCard") != std::string::npos);
    std::printf("[test] Home uses shared grid rewind edge OK\n");
}

static void testHomeUsesSharedDownIntentAcrossPageLoads()
{
    std::printf("[test] Home preserves Down intent across page loads\n");
    const std::string header=readTestBytes("src/ui/screens/HomeScreen.hpp");
    const std::string navigation=readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string apply=readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(header.find("pendingDown") != std::string::npos);
    CHECK(navigation.find("gridAtBottomRow") != std::string::npos);
    CHECK(navigation.find("pendingDown") != std::string::npos);
    CHECK(apply.find("applyPendingDown") != std::string::npos);
    std::printf("[test] Home preserves Down intent across page loads OK\n");
}

static void testDecideHomeStartupSync()
{
    std::printf("[test] decideHomeStartupSync truth table\n");
    // Zero checkpoint → FullReconcile
    CHECK(decideHomeStartupSync(1000, 0, 0, 0, true, true) == HomeStartupSync::FullReconcile);
    // Committed generation 0 → FullReconcile
    CHECK(decideHomeStartupSync(1000, 100, 100, 0, true, true) == HomeStartupSync::FullReconcile);
    // Scope epoch invalid → FullReconcile
    CHECK(decideHomeStartupSync(1000, 100, 100, 1, false, true) == HomeStartupSync::FullReconcile);
    // Empty catalog → FullReconcile
    CHECK(decideHomeStartupSync(1000, 100, 100, 1, true, false) == HomeStartupSync::FullReconcile);
    // lastReconcile > lastSuccessful → FullReconcile
    CHECK(decideHomeStartupSync(1000, 100, 200, 1, true, true) == HomeStartupSync::FullReconcile);
    // Clock regressed (now < lastSuccessful) → FullReconcile
    CHECK(decideHomeStartupSync(50, 100, 50, 1, true, true) == HomeStartupSync::FullReconcile);
    // Stale lastReconcile (beyond kHomeFullReconcileMs) → FullReconcile
    const std::int64_t k24h = 24LL*60*60*1000;
    CHECK(decideHomeStartupSync(1000 + k24h + 1, 1000, 1000, 1, true, true) == HomeStartupSync::FullReconcile);
    // Within 15min FRESH_MS with valid generation and scope → SkipFresh
    CHECK(decideHomeStartupSync(500000, 100000, 100000, 1, true, true) == HomeStartupSync::SkipFresh);
    // Age 1 hour, valid → DeltaCatchUp
    CHECK(decideHomeStartupSync(1000 + 3600000, 1000, 1000, 1, true, true) == HomeStartupSync::DeltaCatchUp);
    // Age 25 hours → FullReconcile (beyond delta max age)
    CHECK(decideHomeStartupSync(1000 + k24h + 3600000, 1000, 1000, 1, true, true) == HomeStartupSync::FullReconcile);
    std::printf("[test] decideHomeStartupSync truth table OK\n");
}

static void testHomeStartupSyncSourceOrder()
{
    std::printf("[test] Home startup sync source order assertions\n");
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    // Rails fetch (getResumeItems) must appear before begin(
    const auto resumePos = sync.find("getResumeItems");
    const auto beginPos = sync.find("m_librarySync->begin(");
    CHECK(resumePos != std::string::npos);
    CHECK(beginPos != std::string::npos);
    CHECK(resumePos < beginPos);
    // ImageCache::runJanitor and season prefetch must appear before the
    // main m_fetchComplete.store(true) (after the population walk) so they
    // don't block the UI thread.  Use rfind to skip the early-offline
    // branch's store.
    const auto fetchCompletePos = sync.rfind("m_fetchComplete.store(true)");
    const auto janitorPos = sync.find("ImageCache::runJanitor()");
    const auto prefetchPos = sync.find("collectBoundedSeriesIds");
    CHECK(fetchCompletePos != std::string::npos);
    CHECK(janitorPos != std::string::npos);
    CHECK(prefetchPos != std::string::npos);
    CHECK(janitorPos < fetchCompletePos);
    CHECK(prefetchPos < fetchCompletePos);
    // Incremental rail-apply must be gated on m_homeRailsReady, not
    // m_fetchComplete
    CHECK(apply.find("m_homeRailsReady.load() && !m_homeRailsApplied")
          != std::string::npos);
    // Existing source substrings that scan tests pin
    CHECK(sync.find("catchUpChangedCatalog") != std::string::npos);
    CHECK(sync.find("reconcileAuthoritativeMembership") != std::string::npos);
    CHECK(sync.find("planHomeRailPosterJobs(cw, ra)") != std::string::npos);
    CHECK(sync.find("queuePosterJobs(planMediaPagePosterJobs(page.items), start == 0)")
          != std::string::npos);
    // Startup-rail buffer: fetch worker writes m_startupRailCW under
    // m_fetchMutex, finishFetch reads under the same mutex.
    CHECK(sync.find("m_startupRailCW") != std::string::npos);
    CHECK(apply.find("m_startupRailCW") != std::string::npos);
    // m_initialPopulationInProgress must be set before the startup rails
    // fetch so a queued UserDataChanged cannot start a competing rail
    // refresh during the startup window.
    const auto initPopPos = sync.find("m_initialPopulationInProgress = true");
    // Use a fetch-worker-specific pattern (local token variable) to skip
    // the refresh thread's own getResumeItems call.
    const auto railsFetchPos = sync.find("getResumeItems(base, token");
    CHECK(initPopPos != std::string::npos);
    CHECK(railsFetchPos != std::string::npos);
    CHECK(initPopPos < railsFetchPos);
    CHECK(apply.find("updateContinueWatchingRow(m_tabs, m_remoteSnapshot.continueWatching)")
          != std::string::npos);
    CHECK(apply.find("updateRecentlyAddedRow(m_tabs, m_remoteSnapshot.recentlyAdded)")
          != std::string::npos);
    CHECK(apply.find("planHomeRailPosterJobs") != std::string::npos);
    CHECK(apply.find("m_fetchPostFinalizeApplied") != std::string::npos);
    CHECK(apply.find("mediaWindowsFromTabs") != std::string::npos);
    CHECK(apply.find("m_moviePage.items = warmWindows.movies") != std::string::npos);
    std::printf("[test] Home startup sync source order assertions OK\n");
}

static void testShowsPresentation()
{
    std::printf("[test] shows title, anime classification, and grid helpers\n");
    testDecideHomeStartupSync();
    testHomeStartupSyncSourceOrder();
    testHomeIncrementalPublicationPolicy();
    testLibraryChangedEventQueueAndReconnectPolicy();
    testBoundedPagePosterPlanning();
    testHomeRailPosterPlanning();
    testInitialHomeArtworkScheduling();
    testShowsFocusPreservedDuringIncrementalRefresh();
    testHomeGridPositionHelpers();
    testRecentlyAddedWarmPublication();
    testWarmHomeCatalogWindows();
    testWarmMediaWindowsExtraction();
    testAnimeSelectionSurvivesSharedWindowTrim();
    testHomeUsesDedicatedAnimeCatalogPage();
    testHomeRefreshPreservesActiveGridSelection();
    testHomeBoundedPagesCanRewind();
    testHomeUsesSharedGridRewindEdge();
    testHomeUsesSharedDownIntentAcrossPageLoads();
    CHECK_EQ(organizationalTitle("The Boys"),"Boys"); CHECK_EQ(organizationalTitle("The Last of Us"),"Last of Us"); CHECK_EQ(organizationalTitle("The Simpsons"),"Simpsons"); CHECK_EQ(organizationalTitle("The"),"The"); CHECK_EQ(organizationalTitle("There She Goes"),"There She Goes");
    CHECK(libraryNameContainsAnimeToken("Anime"));CHECK(libraryNameContainsAnimeToken("My Anime"));CHECK(libraryNameContainsAnimeToken("Shows - Anime"));CHECK(!libraryNameContainsAnimeToken("Animated Shows"));CHECK(!libraryNameContainsAnimeToken("Animation"));
    CachedLibraryView normal{"n","TV Shows","tvshows",{titledShow("b","The Boys"),titledShow("bb","Breaking Bad"),titledShow("dup","Normal")}}; CachedLibraryView anime{"a","Anime Shows","tvshows",{titledShow("at","Attack on Titan"),titledShow("ap","The Apothecary Diaries"),titledShow("dup","Duplicate")}}; ShowsPresentation p=makeShowsPresentation({normal,anime});CHECK(p.shows.size()==2&&p.anime.size()==3);CHECK_EQ(p.shows[0].title,"The Boys");CHECK_EQ(p.shows[1].title,"Breaking Bad");CHECK_EQ(p.anime[0].title,"The Apothecary Diaries");CHECK(matchesAlphabetFilter(p.anime[0].title,0));CHECK(!matchesAlphabetFilter(p.anime[0].title,19));
    CHECK(moveShowsGrid(3,8,0,1)==3);CHECK(moveShowsGrid(3,8,1,0)==7);CHECK(moveShowsGrid(7,9,1,0)==8);CHECK(clampShowsGridScroll(12,50,0)==1);CHECK(closestShowsGridIndex(11,5)==4);
}

static void testShouldShowClockErrorTrue()
{
    std::printf("[test] shouldShowClockError: peer-fail + 1970 -> true\n");
    // Peer verification failed AND clock is 1970 — must fire.
    CHECK(shouldShowClockError(true, std::time_t(0)));
    CHECK(shouldShowClockError(true, std::time_t(591913)));
    // One second before the 2020 boundary is still invalid.
    CHECK(shouldShowClockError(true, std::time_t(1577836799L)));
    std::printf("[test] shouldShowClockError: peer-fail + 1970 -> true OK\n");
}
static void testShouldShowClockErrorFalseModernEpoch()
{
    std::printf("[test] shouldShowClockError: peer-fail + modern epoch -> false\n");
    // Peer verification failed but clock is fine — must NOT fire.
    CHECK(!shouldShowClockError(true, std::time_t(1577836800L)));  // exactly 2020-01-01
    CHECK(!shouldShowClockError(true, std::time_t(1750000000L)));  // 2025
    std::printf("[test] shouldShowClockError: peer-fail + modern epoch -> false OK\n");
}

static void testShouldShowClockErrorFalseUnrelatedFailure()
{
    std::printf("[test] shouldShowClockError: unrelated failure + 1970 -> false\n");
    // Clock is wrong but the curl error is NOT peer verification — must NOT fire.
    CHECK(!shouldShowClockError(false, std::time_t(0)));
    CHECK(!shouldShowClockError(false, std::time_t(591913)));
    std::printf("[test] shouldShowClockError: unrelated failure + 1970 -> false OK\n");
}

static void testClockMessageFormat()
{
    std::printf("[test] kClockErrorMessage: one-line, short, contains OnionOS path\n");
    std::string msg = kClockErrorMessage;
    // Must be short enough to fit in wrapCols=75 without wrapping.
    CHECK(msg.size() <= 75);
    // Must contain no newline.
    CHECK(msg.find('\n') == std::string::npos);
    // Must contain the OnionOS navigation path.
    CHECK(msg.find("OnionOS") != std::string::npos);
    CHECK(msg.find("Apps") != std::string::npos);
    CHECK(msg.find("Tweaks") != std::string::npos);
    CHECK(msg.find("Date/time") != std::string::npos);
    std::printf("[test] kClockErrorMessage: one-line, short, contains OnionOS path OK\n");
}

static void testLiveChangeCheckpointPolicy()
{
    std::printf("[test] live change checkpoint policy\n");

    // liveChangeIsEmpty truth table
    {
        JellyfinLibraryChangeBatch b;
        CHECK(HomeScreen::liveChangeIsEmpty(b));
    }
    {
        JellyfinLibraryChangeBatch b;
        b.catchUpRequired = true;
        CHECK(!HomeScreen::liveChangeIsEmpty(b));
    }
    {
        JellyfinLibraryChangeBatch b;
        b.itemsAdded.push_back("id1");
        CHECK(!HomeScreen::liveChangeIsEmpty(b));
    }
    {
        JellyfinLibraryChangeBatch b;
        b.itemsUpdated.push_back("id1");
        CHECK(!HomeScreen::liveChangeIsEmpty(b));
    }
    {
        JellyfinLibraryChangeBatch b;
        b.itemsRemoved.push_back("id1");
        CHECK(!HomeScreen::liveChangeIsEmpty(b));
    }

    // liveChangeNeedsFullReconcile truth table
    CHECK(HomeScreen::liveChangeNeedsFullReconcile(0, true));
    CHECK(!HomeScreen::liveChangeNeedsFullReconcile(1000, true));
    CHECK(!HomeScreen::liveChangeNeedsFullReconcile(0, false));
    CHECK(!HomeScreen::liveChangeNeedsFullReconcile(1000, false));

    // Tripwire: both bounded and full reconcile paths remain in the source.
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(sync.find("catchUpChangedCatalog") != std::string::npos);
    CHECK(sync.find("reconcileAuthoritativeMembership") != std::string::npos);

    std::printf("[test] live change checkpoint policy OK\n");
}

static void testHomeRowFocusReconciliation()
{
    std::printf("[test] Home row focus reconciliation sites\n");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    const std::string refresh = readTestBytes("src/ui/screens/HomeScreenRefresh.cpp");
    const std::string navigation = readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string header = readTestBytes("src/ui/screens/HomeScreen.hpp");
    const std::string tabsHeader = readTestBytes("src/ui/HomeTabs.hpp");
    const std::string tabsImpl = readTestBytes("src/ui/HomeTabs.cpp");
    // homeRowIndexByLabel must exist and be usable by restoreHomeRowFocus
    CHECK(tabsHeader.find("homeRowIndexByLabel") != std::string::npos);
    CHECK(tabsImpl.find("homeRowIndexByLabel") != std::string::npos);
    CHECK(navigation.find("homeRowIndexByLabel") != std::string::npos);
    // restoreHomeRowFocus must be declared and implemented
    CHECK(header.find("restoreHomeRowFocus") != std::string::npos);
    CHECK(navigation.find("restoreHomeRowFocus") != std::string::npos);
    // focusedHomeRowLabel must be declared and implemented
    CHECK(header.find("focusedHomeRowLabel") != std::string::npos);
    CHECK(navigation.find("focusedHomeRowLabel") != std::string::npos);
    // Reconciliation must be wired at every Home-row mutation site
    CHECK(apply.find("restoreHomeRowFocus") != std::string::npos);
    CHECK(refresh.find("restoreHomeRowFocus") != std::string::npos);
    // Source files must not mutate m_tabs without focus reconciliation
    // (the tab-move sites in SyncApply must call focusedHomeRowLabel)
    CHECK(apply.find("focusedHomeRowLabel") != std::string::npos);
    CHECK(refresh.find("focusedHomeRowLabel") != std::string::npos);
    std::printf("[test] Home row focus reconciliation sites OK\n");
}

static void testCardSurfaceCacheEviction()
{
    std::printf("[test] Card surface cache eviction policy\n");
    // Verify the render source uses cached surface lookup pattern
    const std::string render = readTestBytes("src/ui/screens/HomeScreenRender.cpp");
    CHECK(render.find("m_cardSurfaceCache") != std::string::npos);
    CHECK(render.find("prepareCardSurface") != std::string::npos);

    // Verify the artwork source prepares and evicts cached surfaces
    const std::string artwork = readTestBytes("src/ui/screens/HomeScreenArtwork.cpp");
    CHECK(artwork.find("m_cardSurfaceCache") != std::string::npos);
    CHECK(artwork.find("freeAllCardSurfaces") != std::string::npos);

    // Verify the destructor frees cached surfaces
    const std::string homeScreen = readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(homeScreen.find("freeAllCardSurfaces()") != std::string::npos);

    // Verify the header declares the cache and helpers
    const std::string header = readTestBytes("src/ui/screens/HomeScreen.hpp");
    CHECK(header.find("m_cardSurfaceCache") != std::string::npos);
    CHECK(header.find("prepareCardSurface") != std::string::npos);
    CHECK(header.find("freeAllCardSurfaces") != std::string::npos);

    std::printf("[test] Card surface cache eviction policy OK\n");
}

static void testNonBlockingFinishPolicy()
{
    std::printf("[test] Non-blocking finish* join policy\n");
    // Verify finish* functions no longer join worker threads
    const std::string syncApply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");

    // finishLiveChangeApply should not contain thread.join()
    // It should contain the "Thread join deferred" comment
    CHECK(syncApply.find("finishLiveChangeApply") != std::string::npos);
    // finishHomeRailRefresh should not join
    CHECK(syncApply.find("finishHomeRailRefresh") != std::string::npos);
    // finishSafetyReconcile should not join
    CHECK(syncApply.find("finishSafetyReconcile") != std::string::npos);

    // finishFetch uses m_fetchPostFinalizeApplied flag instead of join
    CHECK(syncApply.find("m_fetchPostFinalizeApplied") != std::string::npos);

    const std::string refresh = readTestBytes("src/ui/screens/HomeScreenRefresh.cpp");
    // finishResumeRefresh should not join
    CHECK(refresh.find("finishResumeRefresh") != std::string::npos);
    // finishDownloadRefresh should not join
    CHECK(refresh.find("finishDownloadRefresh") != std::string::npos);

    // Verify start* functions still join before starting new threads
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(sync.find("m_liveChangeDone.load() || !m_liveChangeInFlight") != std::string::npos);

    // Verify the header declares the post-finalize flag
    const std::string header = readTestBytes("src/ui/screens/HomeScreen.hpp");
    CHECK(header.find("m_fetchPostFinalizeApplied") != std::string::npos);

    std::printf("[test] Non-blocking finish* join policy OK\n");
}

static void testRefreshMovieFilterHomeNavPreserved()
{
    std::printf("[test] refreshMovieFilter preserves Home nav state\n");
    const std::string navigation = readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    // refreshMovieFilter must capture moviesActive BEFORE mutations
    CHECK(navigation.find("activeTabNamed(\"Movies\")") != std::string::npos);
    // The nav-state mutation block (m_activeRow=0, m_activeCard, etc.)
    // must be guarded so it is skipped when Movies tab is not active.
    // The guard "if (!moviesActive) return;" must appear between the
    // row rebuild and the nav-state writes.
    const std::string marker = "if (!moviesActive) return;";
    CHECK(navigation.find(marker) != std::string::npos);

    // Verify the guard appears AFTER the rows assignment but BEFORE m_activeRow=0
    const auto rowsAssign = navigation.find("m_tabs[movies].rows");
    const auto guard = navigation.find(marker);
    const auto activeRow = navigation.find("m_activeRow = 0");
    CHECK(rowsAssign != std::string::npos);
    CHECK(guard != std::string::npos);
    CHECK(activeRow != std::string::npos);
    CHECK(rowsAssign < guard);
    CHECK(guard < activeRow);

    // Verify the pure helper exists in the header
    const std::string stateHeader = readTestBytes("src/ui/HomeSyncState.hpp");
    CHECK(stateHeader.find("livePublicationIsNoop") != std::string::npos);

    std::printf("[test] refreshMovieFilter preserves Home nav state OK\n");
}

static void testLivePublicationNoopEmptyResult()
{
    std::printf("[test] livePublicationIsNoop truth table\n");
    // Pure predicate: true only when BOTH containers are empty
    CHECK(livePublicationIsNoop(true, true));
    CHECK(!livePublicationIsNoop(true, false));
    CHECK(!livePublicationIsNoop(false, true));
    CHECK(!livePublicationIsNoop(false, false));

    // Source-reading: publishLiveCatalogItems must call livePublicationIsNoop
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(apply.find("livePublicationIsNoop") != std::string::npos);
    CHECK(apply.find("result.items.empty()") != std::string::npos);
    CHECK(apply.find("result.removedIds.empty()") != std::string::npos);

    std::printf("[test] livePublicationIsNoop truth table OK\n");
}

static void testSqliteTempDirPath()
{
    std::printf("[test] SQLite temp dir path selection\n");
    CHECK_EQ(sqliteTempDirPath("/app"), std::string("/app/cache/tmp"));
    CHECK_EQ(sqliteTempDirPath("/mnt/sdcard/Apps/MiyooFin"),
             std::string("/mnt/sdcard/Apps/MiyooFin/cache/tmp"));
    CHECK_EQ(sqliteTempDirPath(""), std::string("/cache/tmp"));
    std::printf("[test] SQLite temp dir path selection OK\n");
}

static void testPageTransactionShouldRetry()
{
    std::printf("[test] Page transaction retry-bound decision\n");
    // Transient errors retry when under the limit.
    CHECK(catalog_db_internal::pageTransactionShouldRetry(SQLITE_BUSY, 0, 3));
    CHECK(catalog_db_internal::pageTransactionShouldRetry(SQLITE_LOCKED, 1, 3));
    CHECK(catalog_db_internal::pageTransactionShouldRetry(SQLITE_FULL, 2, 3));
    // At the limit, no more retries.
    CHECK(!catalog_db_internal::pageTransactionShouldRetry(SQLITE_BUSY, 3, 3));
    CHECK(!catalog_db_internal::pageTransactionShouldRetry(SQLITE_FULL, 3, 3));
    // Non-transient errors never retry.
    CHECK(!catalog_db_internal::pageTransactionShouldRetry(SQLITE_ERROR, 0, 3));
    CHECK(!catalog_db_internal::pageTransactionShouldRetry(SQLITE_CORRUPT, 0, 3));
    CHECK(!catalog_db_internal::pageTransactionShouldRetry(SQLITE_DONE, 0, 3));
    // Edge: maxAttempts=0 means no retries at all.
    CHECK(!catalog_db_internal::pageTransactionShouldRetry(SQLITE_BUSY, 0, 0));
    // Edge: single attempt allowed.
    CHECK(catalog_db_internal::pageTransactionShouldRetry(SQLITE_BUSY, 0, 1));
    CHECK(!catalog_db_internal::pageTransactionShouldRetry(SQLITE_BUSY, 1, 1));
    std::printf("[test] Page transaction retry-bound decision OK\n");
}

static void testPageTransactionCleanupInvariants()
{
    std::printf("[test] processMediaPageUpsert cleanup invariants\n");
    const std::string src = readTestBytes("src/catalog/CatalogDbWrite.cpp");
    const std::string internal =
        readTestBytes("src/catalog/CatalogDbInternal.hpp");
    // A missing or renamed source file must fail loudly: every search below
    // would otherwise pass vacuously against an empty string.
    CHECK(!src.empty());
    CHECK(!internal.empty());

    // Pin the checks to the refactored page-upsert helpers so unrelated code
    // elsewhere in the file cannot satisfy them.
    const auto helpersStart =
        src.find("void CatalogDb::markMediaPagePopulationFailed(");
    const auto processStart =
        src.find("void CatalogDb::processMediaPageUpsert(");
    CHECK(helpersStart != std::string::npos);
    CHECK(processStart != std::string::npos);
    CHECK(processStart > helpersStart);
    const std::string pageHelpers =
        src.substr(helpersStart, processStart - helpersStart);

    const auto itemWriterStart =
        pageHelpers.find("CatalogDb::writeMediaPageItemRows(");
    const auto attemptStart =
        pageHelpers.find("CatalogDb::attemptMediaPageUpsert(");
    CHECK(itemWriterStart != std::string::npos);
    CHECK(attemptStart != std::string::npos);
    CHECK(attemptStart > itemWriterStart);
    const std::string itemWriter = pageHelpers.substr(
        itemWriterStart, attemptStart - itemWriterStart);
    const std::string attempt = pageHelpers.substr(attemptStart);

    // RAII owns every prepared statement in the helpers now: no manual
    // finalize remains in the write path, and the old flag-based cleanup
    // scaffolding (itemLoopFatal/itemLoopRetryable) is gone. Statement
    // release is automatic on every exit, so no exit can forget it.
    CHECK(pageHelpers.find("sqlite3_finalize") == std::string::npos);
    CHECK(pageHelpers.find("itemLoopFatal") == std::string::npos);
    CHECK(pageHelpers.find("itemLoopRetryable") == std::string::npos);
    // All seven RAII guards are declared in the attempt helper.
    CHECK(attempt.find("upsertGuard") != std::string::npos);
    CHECK(attempt.find("deleteGenresGuard") != std::string::npos);
    CHECK(attempt.find("insertGenreGuard") != std::string::npos);
    CHECK(attempt.find("deleteImageTagsGuard") != std::string::npos);
    CHECK(attempt.find("insertImageTagGuard") != std::string::npos);
    CHECK(attempt.find("viewGuard") != std::string::npos);
    CHECK(attempt.find("membershipGuard") != std::string::npos);
    CHECK(internal.find("class ScopedSqliteStmt") != std::string::npos);

    // All failure exits from the per-item writer must map to
    // MediaPageItemLoopOutcome::Fatal. Count only genuine `return` sites in
    // the item writer: the `if (itemOutcome == ...)` dispatch comparisons in
    // the attempt helper must not satisfy this count. Sites: injected
    // failure, bind failure, non-retryable media step, sort key,
    // collections, valid()/stale, membership — 7 sites.
    std::size_t fatalCount = 0;
    {
        std::size_t pos = 0;
        while ((pos = itemWriter.find(
                    "return MediaPageItemLoopOutcome::Fatal", pos))
               != std::string::npos) {
            ++fatalCount;
            pos += 20;
        }
    }
    CHECK(fatalCount >= 7);

    // Transient media step, sort key, collection, and membership failures
    // must still map to MediaPageItemLoopOutcome::Retryable (4 sites).
    std::size_t retryableCount = 0;
    {
        std::size_t pos = 0;
        while ((pos = itemWriter.find(
                    "return MediaPageItemLoopOutcome::Retryable", pos))
               != std::string::npos) {
            ++retryableCount;
            pos += 20;
        }
    }
    CHECK(retryableCount >= 4);
    CHECK(itemWriter.find("return MediaPageItemLoopOutcome::Completed")
          != std::string::npos);

    // Every exit from the per-item writer reports an outcome: a bare
    // `return;` cannot even appear in this value-returning function, so its
    // absence alone proves nothing. Instead pin what matters for the new
    // structure — all returns carry an explicit outcome, and the writer never
    // publishes results itself (commit/rollback/result publication belong to
    // the attempt helper, so no exit can skip guard cleanup).
    {
        std::size_t returnCount = 0;
        std::size_t pos = 0;
        while ((pos = itemWriter.find("return", pos)) != std::string::npos) {
            ++returnCount;
            pos += 6;
        }
        std::size_t outcomeCount = 0;
        pos = 0;
        while ((pos = itemWriter.find("return MediaPageItemLoopOutcome::", pos))
               != std::string::npos) {
            ++outcomeCount;
            pos += 6;
        }
        CHECK(outcomeCount >= 12);
        CHECK(returnCount == outcomeCount);
    }
    CHECK(itemWriter.find("command->result.set_value") == std::string::npos);
    CHECK(itemWriter.find("markMediaPagePopulationFailed") == std::string::npos);

    // Fatal exits must fail the population (unless cancelled/superseded).
    CHECK(pageHelpers.find("markMediaPagePopulationFailed") != std::string::npos);
    CHECK(pageHelpers.find("if (!result.cancelled && !result.superseded)") != std::string::npos);
    CHECK(pageHelpers.find("m_populationStatus.state = CatalogDbPopulationState::Failed") != std::string::npos);
    // Every fatal exit routes through the helper: begin-failed,
    // prepare-failed, item-fatal, and the trailing else branch.
    {
        std::size_t failedCallCount = 0;
        std::size_t pos = 0;
        while ((pos = attempt.find("markMediaPagePopulationFailed(result)", pos)) != std::string::npos) {
            ++failedCallCount;
            pos += 8;
        }
        CHECK(failedCallCount >= 4);
    }

    // Transaction boundaries, the bounded retry policy, and the outcome
    // dispatch live in the attempt helper.
    CHECK(attempt.find("BEGIN IMMEDIATE;") != std::string::npos);
    CHECK(attempt.find("\"COMMIT;\"") != std::string::npos);
    CHECK(pageHelpers.find("kPageTransactionMaxAttempts") != std::string::npos);
    CHECK(pageHelpers.find("pageTransactionShouldRetry") != std::string::npos);
    CHECK(attempt.find("if (itemOutcome == MediaPageItemLoopOutcome::Fatal)")
          != std::string::npos);
    CHECK(attempt.find("if (itemOutcome == MediaPageItemLoopOutcome::Retryable)")
          != std::string::npos);

    // The test-injection hook used by the catalog tests is intact in the
    // item writer.
    CHECK(itemWriter.find("failureSpec().failAfterRows") != std::string::npos);
    CHECK(itemWriter.find("injected page failure") != std::string::npos);

    // Retry-backoff cancellation lives in processMediaPageUpsert.
    const auto fnEnd = src.find("} // namespace miyoofin", processStart);
    CHECK(fnEnd != std::string::npos);
    const std::string processFn =
        src.substr(processStart, fnEnd - processStart);
    // Retry-backoff cancellation must set result.cancelled = true.
    CHECK(processFn.find("result.cancelled = true") != std::string::npos);
    CHECK(processFn.find("kPageTransactionMaxAttempts") != std::string::npos);

    // The fatal branch must roll back exactly once before failing.
    const auto fatalHandlerStart =
        attempt.find("MediaPageItemLoopOutcome::Fatal) {");
    CHECK(fatalHandlerStart != std::string::npos);
    if (fatalHandlerStart != std::string::npos) {
        const auto fatalHandlerEnd = attempt.find(
            "MediaPageItemLoopOutcome::Retryable", fatalHandlerStart);
        CHECK(fatalHandlerEnd != std::string::npos);
        if (fatalHandlerEnd != std::string::npos
            && fatalHandlerEnd > fatalHandlerStart) {
            const std::string fatalHandlerBlock = attempt.substr(
                fatalHandlerStart, fatalHandlerEnd - fatalHandlerStart);
            std::size_t rbCount = 0;
            std::size_t pos = 0;
            while ((pos = fatalHandlerBlock.find("ROLLBACK", pos)) != std::string::npos) {
                ++rbCount;
                pos += 8;
            }
            CHECK(rbCount == 1);
        }
    }

    std::printf("[test] processMediaPageUpsert cleanup invariants OK\n");
}

static void testOfflineModeToggleDrivesFetchPath()
{
    std::printf("[test] Offline mode toggle drives fetch path\n");

    const std::string nav = readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    const std::string hdr = readTestBytes("src/ui/screens/HomeScreen.hpp");

    // The Settings OfflineMode toggle handler must call startFetch() rather
    // than applyPresentationProjection() / restoreOnlinePresentation().
    // The old code did: if (manualOfflineMode) applyPresentationProjection();
    //                    else if (!m_libraryOffline) restoreOnlinePresentation();
    // This was a silent no-op when m_haveCachedSnapshot was not populated.
    const std::size_t toggleStart = nav.find("SettingsRowAction::OfflineMode");
    CHECK(toggleStart != std::string::npos);
    // Search for the closing brace of the OfflineMode case block.
    // The next case (LocalAddress) starts after it.
    const std::size_t nextCase = nav.find("LocalAddress", toggleStart);
    CHECK(nextCase != std::string::npos);
    const std::string toggleCode = nav.substr(toggleStart, nextCase - toggleStart);
    CHECK(toggleCode.find("startFetch()") != std::string::npos);
    // The toggle must NOT call applyPresentationProjection() or
    // restoreOnlinePresentation() as direct function calls (with parens).
    // They may appear in comments explaining what was replaced.
    CHECK(toggleCode.find("applyPresentationProjection()") == std::string::npos);
    CHECK(toggleCode.find("restoreOnlinePresentation()") == std::string::npos);
    // The toggle must set the pending flag when startFetch() returns false.
    CHECK(toggleCode.find("m_offlineModeFetchPending") != std::string::npos);

    // startFetch() must return bool so callers can detect in-flight.
    CHECK(sync.find("bool HomeScreen::startFetch()") != std::string::npos);
    CHECK(hdr.find("bool startFetch()") != std::string::npos);

    // finishFetch() must re-fetch when the pending flag is set.
    CHECK(apply.find("m_offlineModeFetchPending") != std::string::npos);
    CHECK(apply.find("startFetch()") != std::string::npos);

    // The offline worker path must populate m_haveCachedSnapshot.
    CHECK(sync.find("m_haveCachedSnapshot = true") != std::string::npos);
    CHECK(sync.find("m_fetchCacheSaved = true") != std::string::npos);

    std::printf("[test] Offline mode toggle drives fetch path OK\n");
}

static void testOfflineSnapshotSignatureStability()
{
    std::printf("[test] Offline snapshot signature stability/change detection\n");

    // Empty snapshots must match.
    {
        DownloadSnapshot a, b;
        const auto sigA = HomeScreen::computeOfflineSignature(a);
        const auto sigB = HomeScreen::computeOfflineSignature(b);
        CHECK(sigA == sigB);
        CHECK(!(sigA != sigB));
        CHECK(sigA.availableItemCount == 0);
        CHECK(sigA.totalDownloadedBytes == 0);
    }

    // Identical snapshots must match.
    {
        DownloadSnapshot a, b;
        DownloadItem item;
        item.itemId = "m1"; item.itemType = "movie";
        item.state = DownloadState::Complete;
        item.downloadedBytes = 1024;
        a.items.push_back(item); b.items.push_back(item);
        a.localBytes = 2048; b.localBytes = 2048;
        a.reservedBytes = 512; b.reservedBytes = 512;
        const auto sigA = HomeScreen::computeOfflineSignature(a);
        const auto sigB = HomeScreen::computeOfflineSignature(b);
        CHECK(sigA == sigB);
        CHECK(sigA.availableItemCount == 1);
        CHECK(sigA.totalDownloadedBytes == 1024);
        CHECK(sigA.availableItemIds.count("m1") == 1);
    }

    // Different item count must differ.
    {
        DownloadSnapshot a, b;
        DownloadItem item;
        item.itemId = "m1"; item.itemType = "movie";
        item.state = DownloadState::Complete; item.downloadedBytes = 100;
        a.items.push_back(item);
        item.itemId = "m2";
        b.items.push_back(item);
        b.items.push_back(item);
        CHECK(HomeScreen::computeOfflineSignature(a)
              != HomeScreen::computeOfflineSignature(b));
    }

    // Different downloadedBytes must differ.
    {
        DownloadSnapshot a, b;
        DownloadItem item;
        item.itemId = "m1"; item.itemType = "movie";
        item.state = DownloadState::Complete;
        item.downloadedBytes = 100;
        a.items.push_back(item);
        item.downloadedBytes = 200;
        b.items.push_back(item);
        CHECK(HomeScreen::computeOfflineSignature(a)
              != HomeScreen::computeOfflineSignature(b));
    }

    // Unavailable items must not affect the signature.
    {
        DownloadSnapshot a, b;
        DownloadItem item;
        item.itemId = "m1"; item.itemType = "movie";
        item.state = DownloadState::Complete; item.downloadedBytes = 100;
        a.items.push_back(item);
        item.itemId = "m2";
        item.state = DownloadState::Queued;
        a.items.push_back(item);
        b = a;
        b.items[1].state = DownloadState::Downloading;
        CHECK(HomeScreen::computeOfflineSignature(a)
              == HomeScreen::computeOfflineSignature(b));
    }

    // Different localBytes must differ.
    {
        DownloadSnapshot a, b;
        a.localBytes = 1000; b.localBytes = 2000;
        CHECK(HomeScreen::computeOfflineSignature(a)
              != HomeScreen::computeOfflineSignature(b));
    }

    // Source-scan: header must declare the signature infrastructure.
    const std::string hdr = readTestBytes("src/ui/screens/HomeScreen.hpp");
    CHECK(hdr.find("OfflineSnapshotSignature") != std::string::npos);
    CHECK(hdr.find("computeOfflineSignature") != std::string::npos);
    CHECK(hdr.find("tryApplyCachedOfflineSnapshot") != std::string::npos);
    CHECK(hdr.find("m_offlineSnapshotCache") != std::string::npos);
    CHECK(hdr.find("m_offlineSignature") != std::string::npos);
    CHECK(hdr.find("m_haveOfflineSignature") != std::string::npos);
    CHECK(hdr.find("m_haveOfflineSnapshotCache") != std::string::npos);

    std::printf("[test] Offline snapshot signature stability/change detection OK\n");
}

static void testOfflineToggleCancelsFetchAndUsesCache()
{
    std::printf("[test] Offline toggle cancels in-flight fetch and uses cache\n");

    const std::string nav = readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string offline = readTestBytes("src/ui/screens/HomeScreenOffline.cpp");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");

    // Toggle handler must cancel the in-flight fetch before calling startFetch.
    {
        const std::size_t toggleStart = nav.find("SettingsRowAction::OfflineMode");
        CHECK(toggleStart != std::string::npos);
        const std::size_t toggleEnd = nav.find("return true;", toggleStart);
        CHECK(toggleEnd != std::string::npos);
        const std::string toggleCode = nav.substr(toggleStart, toggleEnd - toggleStart);
        // Must set cancellation flag before startFetch()
        CHECK(toggleCode.find("m_fetchCancellation->store(true)") != std::string::npos);
        // Must try the cached path when entering offline mode
        CHECK(toggleCode.find("tryApplyCachedOfflineSnapshot()") != std::string::npos);
        // The cancellation store must appear before the startFetch() call
        const auto cancelPos = toggleCode.find("m_fetchCancellation->store(true)");
        const auto fetchPos = toggleCode.find("startFetch()");
        CHECK(cancelPos < fetchPos);
    }

    // finishFetch error path must not set LoadState::Error when a mode switch
    // is pending (avoids transient error flash on cancel).
    {
        CHECK(apply.find("m_offlineModeFetchPending") != std::string::npos);
        // The guard must be: !m_haveCachedSnapshot && !m_offlineModeFetchPending
        CHECK(apply.find("!m_haveCachedSnapshot && !m_offlineModeFetchPending") != std::string::npos);
    }

    // Offline worker path must store the cached snapshot + signature.
    {
        CHECK(sync.find("m_offlineSnapshotCache = m_cachedSnapshot") != std::string::npos);
        CHECK(sync.find("m_haveOfflineSnapshotCache = true") != std::string::npos);
        CHECK(sync.find("m_offlineSignature = computeOfflineSignature(downloads)") != std::string::npos);
        CHECK(sync.find("m_haveOfflineSignature = true") != std::string::npos);
    }

    // tryApplyCachedOfflineSnapshot must exist and use the cache.
    {
        CHECK(offline.find("HomeScreen::tryApplyCachedOfflineSnapshot()") != std::string::npos);
        CHECK(offline.find("m_haveOfflineSignature") != std::string::npos);
        CHECK(offline.find("m_haveOfflineSnapshotCache") != std::string::npos);
        CHECK(offline.find("computeOfflineSignature(downloads)") != std::string::npos);
        CHECK(offline.find("m_offlineSnapshotCache") != std::string::npos);
        CHECK(offline.find("applyPresentationProjection()") != std::string::npos);
    }

    // computeOfflineSignature must exist and use isAvailable.
    {
        CHECK(offline.find("HomeScreen::computeOfflineSignature(") != std::string::npos);
        CHECK(offline.find("OfflineLibraryQuery::isAvailable") != std::string::npos);
    }

    std::printf("[test] Offline toggle cancels in-flight fetch and uses cache OK\n");
}
