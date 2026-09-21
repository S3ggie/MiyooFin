#include "../../src/net/JellyfinLibraryEvents.hpp"
#include "../../src/update/AppDir.hpp"
#include "../../src/catalog/CatalogDbInternal.hpp"
#include "../../src/ui/HomeTabs.hpp"
#include <filesystem>

static void testNetworkLayerDoesNotDependOnPresentationModels()
{
    std::printf("[test] network layer has no presentation-model references\n");
    const std::filesystem::path root("src/net");
    CHECK(std::filesystem::exists(root));
    if (std::filesystem::exists(root)) {
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            const std::string source = readTestBytes(entry.path().string());
            CHECK(sourceLacks(source, "PresentationModels"));
            CHECK(sourceLacks(source, "TabData"));
            CHECK(sourceLacks(source, "MediaRow"));
        }
    }
    std::printf("[test] network layer has no presentation-model references OK\n");
}

static void testSourceMatcherTolerance()
{
    std::printf("[test] whitespace-tolerant source matcher\n");
    // Matches across line breaks and indentation.
    CHECK(sourceContains("int f() {\n        return 1;\n    }",
                         "int f() { return 1; }"));
    CHECK(sourceContains("a   =\n    b;", "a=b;"));
    CHECK(sourceContains("publishTransferProgress(i,\n item)",
                         "publishTransferProgress(i,item)"));
    // Still fails on genuinely different code.
    CHECK(!sourceContains("a = c;", "a=b;"));
    CHECK(!sourceContains("int f() { return 2; }", "return 1;"));
    // Empty/unreadable haystack never matches ...
    CHECK(!sourceContains("", "x"));
    // ... and never satisfies an absence check (no vacuous pass on a
    // missing file).
    CHECK(!sourceLacks("", "x"));
    CHECK(sourceLacks("a = c;", "a=b;"));
    CHECK(!sourceLacks("a=\nb;", "a = b;"));
    // Counts and ordering work in whitespace-free token space.
    CHECK(sourceCount("f(x); f( x );", "f(x);") == 2);
    CHECK(sourceCount("", "f(x);") == 0);
    CHECK(sourcePos("aa bb cc", "bb") < sourcePos("aa bb cc", "cc"));
    CHECK(sourcePos("x", "missing") == std::string::npos);
    CHECK(sourceRPos("a b a", "a") > sourcePos("a b a", "a"));
    // Region helpers pin a function / ordered window, not the whole file.
    const std::string twoFns = "void A::f(){\n x();\n}\nvoid A::g(){\n y();\n}\n";
    CHECK(sourceContains(sourceFunction(twoFns, "void A::f("), "x();"));
    CHECK(sourceLacks(sourceFunction(twoFns, "void A::f("), "y();"));
    CHECK(sourceFunction(twoFns, "void A::missing(").empty());
    CHECK(sourceContains(sourceBetween("START a=1; END", "START", "END"),
                         "a=1;"));
    CHECK(sourceBetween("no anchors here", "START", "END").empty());
    std::printf("[test] whitespace-tolerant source matcher OK\n");
}

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
    CHECK(!render.empty());
    CHECK(sourceContains(render, "homeSyncStatus(true)"));
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    const std::string coordinator = readTestBytes(
        "src/library/LibraryCoordinator.cpp");
    CHECK(!sync.empty());
    CHECK(!apply.empty());
    CHECK(!coordinator.empty());
    // Structural: these call sites have no pure unit behind them (they run
    // on the fetch worker), so the wiring is pinned by scan while the
    // policy above is covered behaviourally.
    CHECK(sourceContains(sync, "updateLiveLibraryChanges()"));
    CHECK(sourceContains(coordinator, "catchUpChangedCatalog"));
    CHECK(sourceContains(coordinator, "reconcileAuthoritativeMembership"));
    CHECK(sourceContains(coordinator,
                         "status.committedGeneration = m_catalogGeneration"));
    CHECK(sourceContains(coordinator,
                         "status.maintenanceDue = status.safetyReconcileDue"));
    CHECK(sourceContains(apply, "m_cachedSnapshot.continueWatching"));
    CHECK(sourceContains(apply, "PendingPresentation"));
    CHECK(sourceContains(apply, "publishPendingPresentation"));
    CHECK(sourceContains(apply, "takePendingPresentation"));
    CHECK(sourceContains(apply, "std::make_shared<const PendingPresentation>"));
    CHECK(sourceLacks(sync, "m_syncState"));
    CHECK(sourceLacks(sync, "m_fetchStats"));
    CHECK(sourceLacks(sync, "m_tabs ="));
    CHECK(sourceLacks(sync, "m_cachedSnapshot ="));
    CHECK(sourceLacks(sync, "m_fetchResult ="));
    const std::string settings = readTestBytes(
        "src/ui/screens/HomeScreenSettings.cpp");
    CHECK(sourceContains(settings, "m_libraryCoordinator->status()"));
    CHECK(sourceLacks(settings, "m_syncState"));
    CHECK(sourceLacks(coordinator, "HomeState"));
    CHECK(sourceLacks(coordinator, "publishHomeState"));
    CHECK(sourceLacks(coordinator, "revision"));
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
    CHECK(!source.empty());
    CHECK(sourceContains(source, "queuePosterJobs(planHomeRailPosterJobs(cw, ra), true)"));
    // Structural: the page-poster call must stay gated on `start == 0` (the
    // unconditioned `, true)` variant in this file would requeue every page
    // on every validation). The gating expression itself lives inline in the
    // call argument, so no unit can observe it; the scan pins the shape.
    CHECK(sourceLacks(source, "queuePosterJobs(planMediaPagePosterJobs(page.items), true)"));
    CHECK(sourceContains(source, "queuePosterJobs(planMediaPagePosterJobs(page.items),"));
    CHECK(sourceContains(source, "update.firstPage"));
    const std::string coordinator=readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(!coordinator.empty());
    CHECK(sourceContains(coordinator, "m_posterThreads.emplace_back"));
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(!apply.empty());
    CHECK(sourceContains(apply, "queuePosterJobs(planMediaPagePosterJobs(result.items), true)"));
    CHECK(sourceContains(source, "m_artworkPlanningComplete.store(false)"));
    CHECK(sourceContains(source, "m_artworkPlanningComplete.store(artworkPlanningComplete)"));
    const std::string hierarchy=readTestBytes("src/ui/screens/HomeScreenHierarchy.cpp");
    CHECK(!hierarchy.empty());
    // Structural: the poster worker owns its client/loop in this
    // translation unit; scoped to posterWorker so the hierarchy worker's
    // own loop cannot satisfy the check.
    const std::string posterWorker = sourceFunction(hierarchy, "void HomeScreen::posterWorker(");
    CHECK(!posterWorker.empty());
    CHECK(sourceContains(posterWorker, "HttpClient client;"));
    CHECK(sourceContains(posterWorker, "for (;;)"));
    CHECK(sourceContains(posterWorker, "PosterJob job;"));
    const std::string http=readTestBytes("src/net/HttpClient.hpp");
    CHECK(!http.empty());
    CHECK(sourceContains(http, "CURL *m_curl"));
    CHECK(sourceContains(http, "~HttpClient"));
    std::printf("[test] initial Home artwork scheduling OK\n");
}

static void testPersistentHttpClientPatterns()
{
    std::printf("[test] persistent HttpClient patterns in worker threads\n");
    // HomeScreen fetch worker reuses one client across all metadata calls.
    // Structural: thread-local client reuse has no observable unit (the
    // sharing only matters across sequential network calls), so the
    // declaration-before-use wiring is pinned by scan.
    const std::string sync=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(!sync.empty());
    const std::string coordinator=readTestBytes(
        "src/library/LibraryCoordinator.cpp");
    CHECK(sourceContains(coordinator, "HttpClient client;"));
    CHECK(sourceContains(coordinator, "getViews(")
          && sourceContains(coordinator, "getLibraryItemsPage("));
    CHECK(!coordinator.empty());
    CHECK(sourceContains(coordinator, "HttpClient railClient;"));
    // LibrarySync reconcile reuses one client across views + pages loop
    const std::string inc=readTestBytes("src/library/LibrarySyncIncremental.cpp");
    CHECK(!inc.empty());
    CHECK(sourceContains(inc, "HttpClient reconcileClient;"));
    // EpisodeBrowser artwork worker reuses one client
    const std::string ep=readTestBytes("src/ui/screens/EpisodeBrowserArtwork.cpp");
    CHECK(!ep.empty());
    CHECK(sourceContains(ep, "HttpClient artworkClient;"));
    CHECK(sourceContains(ep, "artworkClient.setTimeoutSec(3)"));
    // SeriesScreen artwork worker reuses one client
    const std::string ss=readTestBytes("src/ui/screens/SeriesScreenWorker.cpp");
    CHECK(!ss.empty());
    CHECK(sourceContains(ss, "HttpClient artworkClient;"));
    CHECK(sourceContains(ss, "artworkClient.setTimeoutSec(8)"));
    // JellyfinApiLibrary exposes HttpClient& overloads
    const std::string api=readTestBytes("src/net/JellyfinApiLibrary.cpp");
    CHECK(!api.empty());
    CHECK(sourceContains(api, "HttpClient &client,"));
    CHECK(sourceContains(api, "return getViews(baseUrl, accessToken, userId, deviceId, views, error, client, cancelled)"));
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
    CHECK(!apply.empty());
    // Structural: the warm-publication call site runs on the fetch worker;
    // the row helper itself is covered behaviourally above.
    CHECK(sourceContains(apply, "updateRecentlyAddedRow(m_tabs, m_remoteSnapshot.recentlyAdded)"));
    std::printf("[test] Recently Added warm publication OK\n");
}

static void testWarmHomeCatalogWindows()
{
    std::printf("[test] warm Home catalog windows\n");
    const std::string source=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply=readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(!source.empty());
    CHECK(!apply.empty());
    CHECK(sourceContains(source, "initialPagePublished = true"));
    // Structural: the warm-window extraction runs inside finishFetch on the
    // worker, so the call sites are pinned by scan (scoped to finishFetch so
    // other mediaWindowsFromTabs users cannot satisfy them) while the
    // extraction helper itself is covered behaviourally.
    const std::string finishFetch = sourceFunction(apply, "void HomeScreen::finishFetch(");
    CHECK(!finishFetch.empty());
    CHECK(sourceContains(finishFetch, "mediaWindowsFromTabs"));
    CHECK(sourceContains(finishFetch, "m_moviePage.items = warmWindows.movies"));
    CHECK(sourceContains(finishFetch, "m_showPage.items = warmWindows.shows"));
    CHECK(sourceContains(finishFetch, "updateContinueWatchingRow(m_tabs, m_remoteSnapshot.continueWatching)"));
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
    CHECK(!source.empty());
    CHECK(!apply.empty());
    // Structural: the old manual clamp-back assignments must stay gone from
    // the sync worker (selection is owned by restoreSelectionIndex +
    // rebuildShowsPresentation, covered behaviourally below). Absence of any
    // direct assignment is the pin: reintroducing the clamp fails this.
    CHECK(sourceLacks(source, "m_showSelected ="));
    CHECK(sourceLacks(source, "m_animeSelected ="));
    CHECK(sourceContains(apply, "rebuildShowsPresentation();"));
    CHECK(HomeScreen::restoreSelectionIndex(
              {titledShow("show-a", "Show A"), titledShow("anime-b", "Anime B")},
              "anime-b", 0) == 1);
    CHECK(HomeScreen::restoreSelectionIndex(
              {titledShow("anime-c", "Anime C")}, "trimmed", 4) == 0);
    std::printf("[test] Anime selection survives shared window trim OK\n");
}

static void testCoordinatorCommittedGenerationStatus()
{
    std::printf("[test] coordinator committed generation/status\n");
    const std::string sync=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string home=readTestBytes("src/ui/screens/HomeScreen.cpp");
    const std::string coordinator=readTestBytes(
        "src/library/LibraryCoordinator.cpp");
    const std::string header=readTestBytes(
        "src/library/LibraryCoordinator.hpp");
    CHECK(!sync.empty());
    CHECK(!home.empty());
    CHECK(!coordinator.empty());
    CHECK(!header.empty());
    // The committed catalog generation and maintenance policy are coordinator
    // state.  Home only projects the coordinator status and uses the same
    // committed generation for its cache and hierarchy guards.
    CHECK(sourceContains(header, "std::uint64_t committedGeneration = 0"));
    CHECK(sourceContains(coordinator,
                         "status.committedGeneration = m_catalogGeneration"));
    CHECK(sourceContains(coordinator,
                         "status.maintenanceDue = status.safetyReconcileDue"));
    CHECK(sourceContains(home,
                         "m_libraryCoordinator->status().committedGeneration"));
    CHECK(sourceContains(sync, "committedCatalogGeneration()"));
    CHECK(sourceLacks(sync,"m_topLevelSyncGeneration"));
    std::printf("[test] coordinator committed generation/status OK\n");
}

static void testHomeUsesDedicatedAnimeCatalogPage()
{
    std::printf("[test] Home uses dedicated Anime catalog page\n");
    const std::string home=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string screen=readTestBytes("src/ui/screens/HomeScreen.cpp");
    const std::string query=readTestBytes("src/library/LibraryQuery.hpp");
    const std::string catalog=readTestBytes("src/catalog/CatalogDb.hpp");
    CHECK(!home.empty());
    CHECK(!screen.empty());
    CHECK(!query.empty());
    CHECK(!catalog.empty());
    // Structural: the dedicated Anime page is a cross-file wiring invariant
    // (worker page + query surface + catalog filter); no single pure unit
    // observes all four ends, so each end is pinned by scan.
    CHECK(sourceContains(home, "m_animePage"));
    CHECK(sourceContains(home, "m_libraryQuery->anime"));
    CHECK(sourceContains(screen, "m_animePage.items"));
    CHECK(sourceContains(query, "anime("));
    CHECK(sourceContains(catalog, "CatalogDbMediaPageFilter"));
    std::printf("[test] Home uses dedicated Anime catalog page OK\n");
}

static void testHomeRefreshPreservesActiveGridSelection()
{
    std::printf("[test] Home refresh preserves active grid selection\n");
    const std::string header=readTestBytes("src/ui/screens/HomeScreen.hpp");
    const std::string source=readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    CHECK(!header.empty());
    CHECK(!source.empty());
    // Structural: grid-selection preservation is member state captured
    // around refresh (needs a live HomeScreen, so no unit can observe it);
    // the capture sites are pinned by scan.
    CHECK(sourceContains(header, "m_moviePreviewId"));
    CHECK(sourceContains(source, "const MediaItem *previousItem = showsSelectedItem();"));
    CHECK(sourceContains(source, "m_moviePreviewId"));
    CHECK(sourceContains(source, "m_showsPreviewId = item->id"));
    std::printf("[test] Home refresh preserves active grid selection OK\n");
}

static void testHomeBoundedPagesCanRewind()
{
    std::printf("[test] Home bounded pages rewind without cursor jumps\n");
    const std::string header=readTestBytes("src/ui/screens/HomeScreen.hpp");
    const std::string navigation=readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string sync=readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply=readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(!header.empty());
    CHECK(!navigation.empty());
    CHECK(!sync.empty());
    CHECK(!apply.empty());
    // Structural: bounded-page rewind is a four-file protocol (header state
    // + nav edge + sync window flag + apply arm); the protocol ends are
    // pinned by scan because the cursor behaviour needs a live screen.
    CHECK(sourceContains(header, "hasEarlier"));
    CHECK(sourceContains(header, "replaceWindowOnNextPage"));
    CHECK(sourceContains(navigation, "requestEarlierMediaPage"));
    CHECK(sourceContains(sync, "replaceWindowOnNextPage"));
    CHECK(sourceContains(apply, "hasEarlier = true"));
    std::printf("[test] Home bounded pages rewind without cursor jumps OK\n");
}

static void testHomeUsesSharedGridRewindEdge()
{
    std::printf("[test] Home uses shared grid rewind edge\n");
    const std::string source=readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    CHECK(!source.empty());
    // Structural: the shared top-row edge helper is static in this
    // translation unit (no unit can link it), so each grid's use of the
    // shared helper — not a local copy — is pinned by scan.
    CHECK(sourceContains(source, "gridAtTopRow"));
    CHECK(sourceContains(source, "gridAtTopRow(m_showSelected"));
    CHECK(sourceContains(source, "gridAtTopRow(m_animeSelected"));
    CHECK(sourceContains(source, "gridAtTopRow(m_activeCard"));
    std::printf("[test] Home uses shared grid rewind edge OK\n");
}

static void testHomeUsesSharedDownIntentAcrossPageLoads()
{
    std::printf("[test] Home preserves Down intent across page loads\n");
    const std::string header=readTestBytes("src/ui/screens/HomeScreen.hpp");
    const std::string navigation=readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string apply=readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(!header.empty());
    CHECK(!navigation.empty());
    CHECK(!apply.empty());
    // Structural: the Down-intent handoff is member state set during page
    // loads and consumed on apply (needs a live screen); both ends are
    // pinned by scan.
    CHECK(sourceContains(header, "pendingDown"));
    CHECK(sourceContains(navigation, "gridAtBottomRow"));
    CHECK(sourceContains(navigation, "pendingDown"));
    CHECK(sourceContains(apply, "applyPendingDown"));
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
    const std::string coordinator = readTestBytes(
        "src/library/LibraryCoordinator.cpp");
    CHECK(!sync.empty());
    CHECK(!apply.empty());
    CHECK(!coordinator.empty());
    // Structural: fetch ordering across worker threads has no deterministic
    // unit (thread interleaving), so the call-site order is pinned by scan
    // in whitespace-free token space (positions, unlike raw offsets, are
    // unaffected by reformatting).
    // Home starts the coordinator-owned startup policy before requesting the
    // coordinator-owned rails worker.
    const auto railsRequestPos = sourcePos(
        sync, "requestHomeRailRefresh(railRequest)");
    const auto beginPos = sourcePos(sync, "startStartupSync(");
    CHECK(railsRequestPos != std::string::npos);
    CHECK(beginPos != std::string::npos);
    CHECK(beginPos < railsRequestPos);
    // ImageCache::runJanitor and season prefetch must appear before the
    // terminal pending publication (after the population walk) so they don't
    // block the UI thread.  Use rfind to skip the earlier incremental
    // publications.
    const auto terminalPublishPos = sourceRPos(sync, "publish();");
    const auto janitorPos = sourcePos(sync, "ImageCache::runJanitor()");
    const auto prefetchPos = sourcePos(sync, "collectBoundedSeriesIds");
    CHECK(terminalPublishPos != std::string::npos);
    CHECK(janitorPos != std::string::npos);
    CHECK(prefetchPos != std::string::npos);
    CHECK(janitorPos < terminalPublishPos);
    CHECK(prefetchPos < terminalPublishPos);
    // Incremental rail-apply must be gated on m_homeRailsReady, not
    // m_fetchComplete
    CHECK(sourceContains(apply, "m_homeRailsReady.load() && !m_homeRailsApplied"));
    // Existing source substrings that scan tests pin
    CHECK(sourceContains(coordinator, "catchUpChangedCatalog"));
    CHECK(sourceContains(coordinator, "reconcileAuthoritativeMembership"));
    CHECK(sourceContains(sync, "planHomeRailPosterJobs(cw, ra)"));
    CHECK(sourceContains(sync, "queuePosterJobs(planMediaPagePosterJobs(page.items),"));
    CHECK(sourceContains(sync, "update.firstPage"));
    // Startup rails are fields of the immutable pending presentation.  The
    // worker publishes them and finishFetch consumes them on the SDL thread.
    CHECK(sourceContains(sync, "pending.railsReady = true"));
    CHECK(sourceContains(sync, "pending.continueWatching = cw"));
    CHECK(sourceContains(apply, "m_fetchRailCW"));
    // m_initialPopulationInProgress must be set before the startup rails
    // fetch so a queued UserDataChanged cannot start a competing rail
    // refresh during the startup window.
    const auto initPopPos = sourcePos(sync, "m_initialPopulationInProgress = true");
    const auto railsFetchPos = sourcePos(
        sync, "requestHomeRailRefresh(railRequest)");
    CHECK(initPopPos != std::string::npos);
    CHECK(railsFetchPos != std::string::npos);
    CHECK(initPopPos < railsFetchPos);
    CHECK(sourceContains(apply, "updateContinueWatchingRow(m_tabs, m_remoteSnapshot.continueWatching)"));
    CHECK(sourceContains(apply, "updateRecentlyAddedRow(m_tabs, m_remoteSnapshot.recentlyAdded)"));
    CHECK(sourceContains(apply, "planHomeRailPosterJobs"));
    CHECK(sourceContains(apply, "m_fetchPostFinalizeApplied"));
    CHECK(sourceContains(apply, "mediaWindowsFromTabs"));
    CHECK(sourceContains(apply, "m_moviePage.items = warmWindows.movies"));
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

static void testHomeRowFocusReconciliation()
{
    std::printf("[test] Home row focus reconciliation sites\n");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    const std::string refresh = readTestBytes("src/ui/screens/HomeScreenRefresh.cpp");
    const std::string navigation = readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string header = readTestBytes("src/ui/screens/HomeScreen.hpp");
    const std::string tabsHeader = readTestBytes("src/ui/HomeTabs.hpp");
    const std::string tabsImpl = readTestBytes("src/ui/HomeTabs.cpp");
    CHECK(!apply.empty());
    CHECK(!refresh.empty());
    CHECK(!navigation.empty());
    CHECK(!header.empty());
    CHECK(!tabsHeader.empty());
    CHECK(!tabsImpl.empty());
    // Behavioural: the row-lookup helper the reconciliation sites share.
    {
        MediaRow first; first.label = "Continue Watching";
        MediaRow second; second.label = "Recently Added";
        const std::vector<MediaRow> rows{first, second};
        CHECK(homeRowIndexByLabel(rows, "Continue Watching") == 0);
        CHECK(homeRowIndexByLabel(rows, "Recently Added") == 1);
        CHECK(homeRowIndexByLabel(rows, "Missing") == -1);
        CHECK(homeRowIndexByLabel({}, "Continue Watching") == -1);
        MediaRow dup; dup.label = "Continue Watching";
        CHECK(homeRowIndexByLabel({first, dup}, "Continue Watching") == 0);
    }
    // Structural: the member helpers and every Home-row mutation site using
    // them need a live HomeScreen, so declaration + wiring are pinned by
    // scan while the lookup above is covered behaviourally.
    // homeRowIndexByLabel must exist and be usable by restoreHomeRowFocus
    CHECK(sourceContains(tabsHeader, "homeRowIndexByLabel"));
    CHECK(sourceContains(tabsImpl, "homeRowIndexByLabel"));
    CHECK(sourceContains(navigation, "homeRowIndexByLabel"));
    // restoreHomeRowFocus must be declared and implemented
    CHECK(sourceContains(header, "restoreHomeRowFocus"));
    CHECK(sourceContains(navigation, "restoreHomeRowFocus"));
    // focusedHomeRowLabel must be declared and implemented
    CHECK(sourceContains(header, "focusedHomeRowLabel"));
    CHECK(sourceContains(navigation, "focusedHomeRowLabel"));
    // Reconciliation must be wired at every Home-row mutation site
    CHECK(sourceContains(apply, "restoreHomeRowFocus"));
    // Source files must not mutate m_tabs without focus reconciliation
    // (the tab-move sites in SyncApply must call focusedHomeRowLabel)
    CHECK(sourceContains(apply, "focusedHomeRowLabel"));
    std::printf("[test] Home row focus reconciliation sites OK\n");
}

static void testHomeRailRefreshCoordinatorPath()
{
    std::printf("[test] Home rail refresh uses one coordinator path\n");
    const std::string header = readTestBytes("src/ui/screens/HomeScreen.hpp");
    const std::string home = readTestBytes("src/ui/screens/HomeScreen.cpp");
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    const std::string refresh = readTestBytes("src/ui/screens/HomeScreenRefresh.cpp");
    const std::string coordinator = readTestBytes(
        "src/library/LibraryCoordinator.cpp");
    CHECK(!header.empty());
    CHECK(!home.empty());
    CHECK(!sync.empty());
    CHECK(!apply.empty());
    CHECK(!refresh.empty());
    CHECK(!coordinator.empty());

    // Returning to a loaded Home uses the same live-refresh state machine;
    // the old resume worker and its result buffers must stay retired.
    CHECK(sourceContains(home, "startHomeRailRefresh()"));
    CHECK(sourceLacks(header, "m_resumeRefreshThread"));
    CHECK(sourceLacks(header, "m_resumeRefreshResult"));
    CHECK(sourceLacks(home, "finishResumeRefresh"));
    CHECK(sourceLacks(refresh, "getResumeItems"));
    CHECK(sourceLacks(refresh, "getLatestItems"));
    CHECK(sourceLacks(refresh, "RouteRequest"));

    // Both rails are consumed from the immutable coordinator result.  A
    // partial failure only updates the valid rail, retaining cached content
    // for the failed one, and the row label is restored around the mutation.
    CHECK(sourceContains(home, "takeHomeRailResult"));
    CHECK(sourceContains(home, "railResult.continueValid"));
    CHECK(sourceContains(home, "railResult.recentlyAddedValid"));
    const std::string finish = sourceFunction(
        apply, "void HomeScreen::finishHomeRailRefresh(");
    CHECK(sourceContains(finish, "m_homeRailContinueValid"));
    CHECK(sourceContains(finish, "m_homeRailRecentValid"));
    CHECK(sourceContains(finish, "restoreHomeRowFocus"));
    CHECK(sourceContains(finish, "clampNavigation"));
    CHECK(sourceContains(finish, "planHomeRailPosterJobs"));
    CHECK(sourceContains(finish, "m_homeRailRefreshError"));

    // Repeated triggers coalesce locally and rerun once after completion;
    // teardown cancels the coordinator worker instead of joining a Home rail
    // thread, so startup/live/resume cannot create duplicate Home workers.
    CHECK(sourceContains(sync, "m_homeRailRefreshPending = true"));
    CHECK(sourceContains(finish, "startHomeRailRefresh()"));
    CHECK(sourceContains(home, "cancelHomeRailRefresh()"));
    CHECK(sourceContains(sync, "requestHomeRailRefresh(request)"));
    CHECK(sourceContains(home, "takeHomeRailResult"));
    CHECK(sourceCount(coordinator, "m_homeRailThread = std::thread") == 1);
    CHECK(sourceLacks(header, "m_resumeRefreshInFlight"));
    std::printf("[test] Home rail refresh coordinator path OK\n");
}

static void testCardSurfaceCacheEviction()
{
    std::printf("[test] Card surface cache eviction policy\n");
    // Verify the render source uses cached surface lookup pattern.
    // Structural: surface lifetime is owned by the render thread (needs a
    // live screen + SDL surfaces), so the cache plumbing is pinned by scan.
    const std::string render = readTestBytes("src/ui/screens/HomeScreenRender.cpp");
    CHECK(!render.empty());
    CHECK(sourceContains(render, "m_cardSurfaceCache"));
    CHECK(sourceContains(render, "prepareCardSurface"));

    // Verify the artwork source prepares and evicts cached surfaces
    const std::string artwork = readTestBytes("src/ui/screens/HomeScreenArtwork.cpp");
    CHECK(!artwork.empty());
    CHECK(sourceContains(artwork, "m_cardSurfaceCache"));
    CHECK(sourceContains(artwork, "freeAllCardSurfaces"));

    // Verify the destructor frees cached surfaces
    const std::string homeScreen = readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(!homeScreen.empty());
    CHECK(sourceContains(homeScreen, "freeAllCardSurfaces()"));

    // Verify the header declares the cache and helpers
    const std::string header = readTestBytes("src/ui/screens/HomeScreen.hpp");
    CHECK(!header.empty());
    CHECK(sourceContains(header, "m_cardSurfaceCache"));
    CHECK(sourceContains(header, "prepareCardSurface"));
    CHECK(sourceContains(header, "freeAllCardSurfaces"));

    std::printf("[test] Card surface cache eviction policy OK\n");
}

static void testNonBlockingFinishPolicy()
{
    std::printf("[test] Non-blocking finish* join policy\n");
    // Verify finish* functions no longer join worker threads.
    // Structural: UI-thread blocking only manifests on device under load,
    // so each finish body is pinned by scan (scoped to the function, since
    // the sibling start* functions legitimately join). A reintroduced join
    // in any finish body fails the absence check below; the old test only
    // checked the function names existed, which passed even when they
    // joined.
    const std::string syncApply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(!syncApply.empty());
    const std::string header = readTestBytes("src/ui/screens/HomeScreen.hpp");
    CHECK(!header.empty());

    // finishHomeRailRefresh should not join; the coordinator owns its worker.
    CHECK(sourceContains(syncApply, "finishHomeRailRefresh"));
    const std::string homeRailBody = sourceFunction(syncApply, "void HomeScreen::finishHomeRailRefresh(");
    CHECK(!homeRailBody.empty());
    CHECK(sourceLacks(homeRailBody, ".join()"));
    // finishSafetyReconcile should not join
    CHECK(sourceContains(syncApply, "finishSafetyReconcile"));
    const std::string safetyBody = sourceFunction(syncApply, "void HomeScreen::finishSafetyReconcile(");
    CHECK(!safetyBody.empty());
    CHECK(sourceLacks(safetyBody, ".join()"));
    CHECK(sourceContains(safetyBody, "Thread join deferred"));

    // finishFetch uses m_fetchPostFinalizeApplied flag instead of join
    CHECK(sourceContains(syncApply, "m_fetchPostFinalizeApplied"));

    const std::string refresh = readTestBytes("src/ui/screens/HomeScreenRefresh.cpp");
    CHECK(!refresh.empty());
    // Resume refresh now reuses finishHomeRailRefresh; no dedicated Home
    // resume worker, completion flag, or join path remains.
    CHECK(sourceLacks(refresh, "finishResumeRefresh"));
    CHECK(sourceLacks(header, "m_resumeRefreshDone"));
    CHECK(sourceLacks(header, "m_resumeRefreshThread"));
    // finishDownloadRefresh should not join
    CHECK(sourceContains(refresh, "finishDownloadRefresh"));
    const std::string downloadBody = sourceFunction(refresh, "void HomeScreen::finishDownloadRefresh(");
    CHECK(!downloadBody.empty());
    CHECK(sourceLacks(downloadBody, ".join()"));
    CHECK(sourceContains(downloadBody, "Thread join deferred"));

    // Verify the header declares the post-finalize flag
    CHECK(sourceContains(header, "m_fetchPostFinalizeApplied"));

    std::printf("[test] Non-blocking finish* join policy OK\n");
}

static void testRefreshMovieFilterHomeNavPreserved()
{
    std::printf("[test] refreshMovieFilter preserves Home nav state\n");
    const std::string navigation = readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    CHECK(!navigation.empty());
    // Structural: nav-state preservation is member state around a row
    // rebuild (needs a live screen), so the guard ordering is pinned by
    // scan in token space: the guard must sit between the rows assignment
    // and the nav-state writes. Scoped to refreshMovieFilter so the other
    // m_activeRow = 0 sites cannot satisfy the order.
    const std::string filterFn = sourceFunction(navigation, "void HomeScreen::refreshMovieFilter(");
    CHECK(!filterFn.empty());
    // refreshMovieFilter must capture moviesActive BEFORE mutations
    CHECK(sourceContains(filterFn, "activeTabNamed(\"Movies\")"));
    // The nav-state mutation block (m_activeRow=0, m_activeCard, etc.)
    // must be guarded so it is skipped when Movies tab is not active.
    // The guard "if (!moviesActive) return;" must appear between the
    // row rebuild and the nav-state writes.
    const std::string marker = "if (!moviesActive) return;";
    CHECK(sourceContains(filterFn, marker));

    // Verify the guard appears AFTER the rows assignment but BEFORE m_activeRow=0
    const auto rowsAssign = sourcePos(filterFn, "m_tabs[movies].rows");
    const auto guard = sourcePos(filterFn, marker);
    const auto activeRow = sourcePos(filterFn, "m_activeRow = 0");
    CHECK(rowsAssign != std::string::npos);
    CHECK(guard != std::string::npos);
    CHECK(activeRow != std::string::npos);
    CHECK(rowsAssign < guard);
    CHECK(guard < activeRow);

    // Verify the pure helper exists in the header (structural: the header
    // declaration has no behaviour; the predicate is covered below).
    const std::string stateHeader = readTestBytes("src/ui/HomeSyncState.hpp");
    CHECK(!stateHeader.empty());
    CHECK(sourceContains(stateHeader, "livePublicationIsNoop"));

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

    // Source-reading: publishLiveCatalogItems must call livePublicationIsNoop.
    // Structural: the gating call runs on the fetch worker (needs live
    // results), so it is pinned by scan scoped to publishLiveCatalogItems —
    // other livePublicationIsNoop users cannot satisfy it — while the
    // predicate itself is covered behaviourally above.
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(!apply.empty());
    const std::string publishFn = sourceFunction(apply, "void HomeScreen::publishLiveCatalogItems(");
    CHECK(!publishFn.empty());
    CHECK(sourceContains(publishFn, "livePublicationIsNoop"));
    CHECK(sourceContains(publishFn, "result.items.empty()"));
    CHECK(sourceContains(publishFn, "result.removedIds.empty()"));

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

static void testDiagnosticPageKindClassification()
{
    std::printf("[test] diagnostic page kind classification\n");
    const std::string catalog = readTestBytes("src/catalog/CatalogDbWrite.cpp");
    const std::string coordinator =
        readTestBytes("src/library/LibraryCoordinator.cpp");
    CHECK(!catalog.empty());
    CHECK(!coordinator.empty());

    const std::string catalogClassification =
        "page.collectionType == \"movies\" ? \"movies\" : "
        "page.collectionType == \"tvshows\" ? \"tvshows\" : \"unknown\"";
    CHECK(sourceCount(catalog, catalogClassification) == 2);
    CHECK(sourceLacks(catalog,
                      "page.collectionType == \"tvshows\" ? \"series\" : \"movie\""));

    const std::string coordinatorClassification =
        "writePage.collectionType == \"movies\" ? \"movies\" : "
        "writePage.collectionType == \"tvshows\" ? \"tvshows\" : \"unknown\"";
    CHECK(sourceContains(coordinator, coordinatorClassification));
    CHECK(sourceLacks(coordinator,
                      "writePage.collectionType == \"tvshows\" ? \"series\" : \"movie\""));

    const auto completedPageIncrement = sourcePos(
        coordinator, "if (written.success) ++completedPageCount;");
    const auto stageCompleteLog = sourcePos(
        coordinator, "full_population_stage_complete");
    CHECK(completedPageIncrement != std::string::npos);
    CHECK(stageCompleteLog != std::string::npos);
    CHECK(completedPageIncrement < stageCompleteLog);
    std::printf("[test] diagnostic page kind classification OK\n");
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
    // elsewhere in the file cannot satisfy them. (Structural throughout:
    // statement lifetimes and rollback placement only exist as source
    // structure; behaviour is covered by the catalog tests' injected-failure
    // runs. All matching below is whitespace-insensitive.)
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
    CHECK(sourceLacks(pageHelpers, "sqlite3_finalize"));
    CHECK(sourceLacks(pageHelpers, "itemLoopFatal"));
    CHECK(sourceLacks(pageHelpers, "itemLoopRetryable"));
    // All seven RAII guards are declared in the attempt helper.
    CHECK(sourceContains(attempt, "upsertGuard"));
    CHECK(sourceContains(attempt, "deleteGenresGuard"));
    CHECK(sourceContains(attempt, "insertGenreGuard"));
    CHECK(sourceContains(attempt, "deleteImageTagsGuard"));
    CHECK(sourceContains(attempt, "insertImageTagGuard"));
    CHECK(sourceContains(attempt, "viewGuard"));
    CHECK(sourceContains(attempt, "membershipGuard"));
    CHECK(sourceContains(internal, "class ScopedSqliteStmt"));

    // All failure exits from the per-item writer must map to
    // MediaPageItemLoopOutcome::Fatal. Count only genuine `return` sites in
    // the item writer: the `if (itemOutcome == ...)` dispatch comparisons in
    // the attempt helper must not satisfy this count. Sites: injected
    // failure, bind failure, non-retryable media step, sort key,
    // collections, valid()/stale, membership — 7 sites.
    CHECK(sourceCount(itemWriter, "return MediaPageItemLoopOutcome::Fatal") >= 7);

    // Transient media step, sort key, collection, and membership failures
    // must still map to MediaPageItemLoopOutcome::Retryable (4 sites).
    CHECK(sourceCount(itemWriter, "return MediaPageItemLoopOutcome::Retryable") >= 4);
    CHECK(sourceContains(itemWriter, "return MediaPageItemLoopOutcome::Completed"));

    // Every exit from the per-item writer reports an outcome: a bare
    // `return;` cannot even appear in this value-returning function, so its
    // absence alone proves nothing. Instead pin what matters for the new
    // structure — all returns carry an explicit outcome, and the writer never
    // publishes results itself (commit/rollback/result publication belong to
    // the attempt helper, so no exit can skip guard cleanup).
    CHECK(sourceCount(itemWriter, "return MediaPageItemLoopOutcome::") >= 12);
    CHECK(sourceCount(itemWriter, "return")
          == sourceCount(itemWriter, "return MediaPageItemLoopOutcome::"));
    CHECK(sourceLacks(itemWriter, "command->result.set_value"));
    CHECK(sourceLacks(itemWriter, "markMediaPagePopulationFailed"));

    // Fatal exits must fail the population (unless cancelled/superseded).
    CHECK(sourceContains(pageHelpers, "markMediaPagePopulationFailed"));
    CHECK(sourceContains(pageHelpers, "if (!result.cancelled && !result.superseded)"));
    CHECK(sourceContains(pageHelpers, "m_populationStatus.state = CatalogDbPopulationState::Failed"));
    // Every fatal exit routes through the helper: begin-failed,
    // prepare-failed, item-fatal, and the trailing else branch.
    CHECK(sourceCount(attempt, "markMediaPagePopulationFailed(result)") >= 4);

    // Transaction boundaries, the bounded retry policy, and the outcome
    // dispatch live in the attempt helper.
    CHECK(sourceContains(attempt, "BEGIN IMMEDIATE;"));
    CHECK(sourceContains(attempt, "\"COMMIT;\""));
    CHECK(sourceContains(pageHelpers, "kPageTransactionMaxAttempts"));
    CHECK(sourceContains(pageHelpers, "pageTransactionShouldRetry"));
    CHECK(sourceContains(attempt, "if (itemOutcome == MediaPageItemLoopOutcome::Fatal)"));
    CHECK(sourceContains(attempt, "if (itemOutcome == MediaPageItemLoopOutcome::Retryable)"));

    // Check each terminal branch in its own source region.  A single
    // aggregate count can pass when a page_complete from an earlier branch
    // is incorrectly used to satisfy a later result publication.
    const std::string completionToken = sourceTokenString(
        "logMediaPageTransition(command, \"page_complete\"");
    const std::string resultToken = sourceTokenString(
        "command->result.set_value");
    const auto checkCompletionBeforePublication =
        [&](const std::string &branch) {
            const std::size_t completionPos = sourcePos(branch, completionToken);
            const std::size_t resultPos = sourcePos(branch, resultToken);
            CHECK(completionPos != std::string::npos);
            CHECK(resultPos != std::string::npos);
            CHECK(completionPos < resultPos);
        };

    // The injected row failure is the existing seam that exercises the
    // MediaPageItemLoopOutcome::Fatal dispatch.
    const std::string fatalBranch = sourceBetween(
        attempt, "if (itemOutcome == MediaPageItemLoopOutcome::Fatal)",
        "if (itemOutcome == MediaPageItemLoopOutcome::Retryable)");
    CHECK(!fatalBranch.empty());
    CHECK(sourceContains(itemWriter, "failureSpec().failAfterRows"));
    checkCompletionBeforePublication(fatalBranch);

    // Retry-backoff cancellation lives in processMediaPageUpsert.
    const auto fnEnd = src.find("} // namespace miyoofin", processStart);
    CHECK(fnEnd != std::string::npos);
    const std::string processFn =
        src.substr(processStart, fnEnd - processStart);

    // Enqueue rejection completes its promise synchronously, before the
    // command can ever reach the worker.
    const auto enqueueStart = src.find(
        "std::future<CatalogDbMediaPageUpsertResult> CatalogDb::enqueueMediaPageUpsert(");
    const auto transitionStart = enqueueStart == std::string::npos
        ? std::string::npos
        : src.find("void CatalogDb::logMediaPageTransition(", enqueueStart);
    CHECK(enqueueStart != std::string::npos);
    CHECK(transitionStart != std::string::npos);
    const std::string enqueueFn = enqueueStart == std::string::npos
        || transitionStart == std::string::npos
        ? std::string()
        : src.substr(enqueueStart, transitionStart - enqueueStart);
    const std::string enqueueRejection = sourceBetween(
        enqueueFn, "if (m_stopping || m_pendingJobs >= kMaxPendingJobs)",
        "m_mediaPageUpsertCommands.push_back");
    CHECK(!enqueueRejection.empty());
    checkCompletionBeforePublication(enqueueRejection);

    // Cancellation during retry backoff and exhaustion after the bounded
    // retry loop each have their own terminal result publication.
    const std::string processCancellation = sourceBetween(
        processFn,
        "if (command->metadata.cancellation && command->metadata.cancellation->load())",
        "std::this_thread::sleep_for");
    CHECK(!processCancellation.empty());
    checkCompletionBeforePublication(processCancellation);

    const auto exhaustedStart = processFn.find("All retry attempts exhausted");
    const std::string retryExhaustion = exhaustedStart == std::string::npos
        ? std::string() : processFn.substr(exhaustedStart);
    CHECK(!retryExhaustion.empty());
    checkCompletionBeforePublication(retryExhaustion);

    // The test-injection hook used by the catalog tests is intact in the
    // item writer.
    CHECK(sourceContains(itemWriter, "failureSpec().failAfterRows"));
    CHECK(sourceContains(itemWriter, "injected page failure"));

    // Retry-backoff cancellation must set result.cancelled = true.
    CHECK(sourceContains(processFn, "result.cancelled = true"));
    CHECK(sourceContains(processFn, "kPageTransactionMaxAttempts"));

    // The fatal branch must roll back exactly once before failing, measured
    // between the Fatal dispatch and the Retryable dispatch (token space, so
    // reformatting the handler cannot shift the window).
    const std::string fatalHandlerBlock = sourceBetween(
        attempt, "MediaPageItemLoopOutcome::Fatal)",
        "MediaPageItemLoopOutcome::Retryable");
    CHECK(!fatalHandlerBlock.empty());
    CHECK(sourceCount(fatalHandlerBlock, "ROLLBACK") == 1);

    std::printf("[test] processMediaPageUpsert cleanup invariants OK\n");
}

static void testOfflineModeToggleDrivesFetchPath()
{
    std::printf("[test] Offline mode toggle drives fetch path\n");

    const std::string nav = readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    const std::string hdr = readTestBytes("src/ui/screens/HomeScreen.hpp");
    CHECK(!nav.empty());
    CHECK(!sync.empty());
    CHECK(!apply.empty());
    CHECK(!hdr.empty());

    // The Settings OfflineMode toggle handler must call startFetch() rather
    // than applyPresentationProjection() / restoreOnlinePresentation().
    // The old code did: if (manualOfflineMode) applyPresentationProjection();
    //                    else if (!m_libraryOffline) restoreOnlinePresentation();
    // This was a silent no-op when m_haveCachedSnapshot was not populated.
    // Structural: the toggle runs on the UI thread against live worker
    // state, so the handler shape is pinned by scan scoped to the
    // OfflineMode case (a missing anchor fails loudly instead of slicing
    // garbage).
    const std::size_t toggleStart = nav.find("SettingsRowAction::OfflineMode");
    CHECK(toggleStart != std::string::npos);
    // Search for the closing brace of the OfflineMode case block.
    // The next case (LocalAddress) starts after it.
    const std::size_t nextCase = toggleStart == std::string::npos
        ? std::string::npos
        : nav.find("LocalAddress", toggleStart);
    CHECK(nextCase != std::string::npos);
    CHECK(nextCase > toggleStart);
    const std::string toggleCode = (toggleStart == std::string::npos
                                    || nextCase == std::string::npos
                                    || nextCase <= toggleStart)
        ? std::string()
        : nav.substr(toggleStart, nextCase - toggleStart);
    CHECK(!toggleCode.empty());
    CHECK(sourceContains(toggleCode, "startFetch()"));
    // The toggle must NOT call applyPresentationProjection() or
    // restoreOnlinePresentation() as direct function calls (with parens).
    // They may appear in comments explaining what was replaced.
    CHECK(sourceLacks(toggleCode, "applyPresentationProjection()"));
    CHECK(sourceLacks(toggleCode, "restoreOnlinePresentation()"));
    // The toggle must set the pending flag when startFetch() returns false.
    CHECK(sourceContains(toggleCode, "m_offlineModeFetchPending"));

    // startFetch() must return bool so callers can detect in-flight.
    CHECK(sourceContains(sync, "bool HomeScreen::startFetch()"));
    CHECK(sourceContains(hdr, "bool startFetch()"));

    // finishFetch() must re-fetch when the pending flag is set.
    CHECK(sourceContains(apply, "m_offlineModeFetchPending"));
    CHECK(sourceContains(apply, "startFetch()"));

    // The worker publishes the offline snapshot through PendingPresentation;
    // the SDL thread applies it to the live presentation fields.
    CHECK(sourceContains(sync, "pending.haveCachedSnapshot = true"));
    CHECK(sourceContains(sync, "pending.cacheSaved = true"));
    CHECK(sourceContains(apply, "m_haveCachedSnapshot = true"));
    CHECK(sourceContains(apply, "m_fetchCacheSaved = presentation.cacheSaved"));

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

    // Same downloads but different catalog generations must differ: a
    // metadata-only catalog sync (titles, artwork tags, playback state)
    // must invalidate the cached offline snapshot.
    {
        DownloadSnapshot a;
        DownloadItem item;
        item.itemId = "m1"; item.itemType = "movie";
        item.state = DownloadState::Complete; item.downloadedBytes = 100;
        a.items.push_back(item);
        a.localBytes = 100;
        CHECK(HomeScreen::computeOfflineSignature(a, 5)
              != HomeScreen::computeOfflineSignature(a, 6));
        // Same generation still matches.
        CHECK(HomeScreen::computeOfflineSignature(a, 5)
              == HomeScreen::computeOfflineSignature(a, 5));
        // The generation rides in the signature field itself.
        CHECK(HomeScreen::computeOfflineSignature(a, 7).catalogGeneration == 7);
        CHECK(HomeScreen::computeOfflineSignature(a).catalogGeneration == 0);
    }

    // Source-scan: header must declare the signature infrastructure.
    // Structural: the declarations have no behaviour of their own; the
    // signature semantics above are covered behaviourally.
    const std::string hdr = readTestBytes("src/ui/screens/HomeScreen.hpp");
    CHECK(!hdr.empty());
    CHECK(sourceContains(hdr, "OfflineSnapshotSignature"));
    CHECK(sourceContains(hdr, "computeOfflineSignature"));
    CHECK(sourceContains(hdr, "tryApplyCachedOfflineSnapshot"));
    CHECK(sourceContains(hdr, "m_offlineSnapshotCache"));
    CHECK(sourceContains(hdr, "m_offlineSignature"));
    CHECK(sourceContains(hdr, "m_haveOfflineSignature"));
    CHECK(sourceContains(hdr, "m_haveOfflineSnapshotCache"));
    // The signature must carry the coordinator's committed catalog generation.
    CHECK(sourceContains(hdr, "catalogGeneration"));
    CHECK(sourceContains(hdr, "std::uint64_t committedCatalogGeneration() const"));

    std::printf("[test] Offline snapshot signature stability/change detection OK\n");
}

static void testOfflineToggleCancelsFetchAndUsesCache()
{
    std::printf("[test] Offline toggle cancels in-flight fetch and uses cache\n");

    const std::string nav = readTestBytes("src/ui/screens/HomeScreenNavigation.cpp");
    const std::string sync = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string offline = readTestBytes("src/ui/screens/HomeScreenOffline.cpp");
    const std::string apply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(!nav.empty());
    CHECK(!sync.empty());
    CHECK(!offline.empty());
    CHECK(!apply.empty());

    // Toggle handler must cancel the in-flight fetch before calling startFetch.
    // Structural: the cancel/fetch race needs live threads, so the handler
    // shape is pinned by scan (a missing anchor fails loudly instead of
    // slicing garbage). Scoped to the whole OfflineMode case (through
    // LocalAddress): the startFetch() call sits after the cached-path early
    // return, so ending the window at the first `return true;` would exclude
    // it and make the order check vacuous.
    {
        const std::size_t toggleStart = nav.find("SettingsRowAction::OfflineMode");
        CHECK(toggleStart != std::string::npos);
        const std::size_t toggleEnd = toggleStart == std::string::npos
            ? std::string::npos
            : nav.find("LocalAddress", toggleStart);
        CHECK(toggleEnd != std::string::npos);
        CHECK(toggleEnd > toggleStart);
        const std::string toggleCode = (toggleStart == std::string::npos
                                        || toggleEnd == std::string::npos
                                        || toggleEnd <= toggleStart)
            ? std::string()
            : nav.substr(toggleStart, toggleEnd - toggleStart);
        CHECK(!toggleCode.empty());
        // Must set cancellation flag before startFetch()
        CHECK(sourceContains(toggleCode, "m_fetchCancellation->store(true)"));
        // Must try the cached path when entering offline mode
        CHECK(sourceContains(toggleCode, "tryApplyCachedOfflineSnapshot()"));
        // The cancellation store must appear before the startFetch() call
        const auto cancelPos = sourcePos(toggleCode, "m_fetchCancellation->store(true)");
        const auto fetchPos = sourcePos(toggleCode, "startFetch()");
        CHECK(cancelPos != std::string::npos);
        CHECK(fetchPos != std::string::npos);
        CHECK(cancelPos < fetchPos);
    }

    // finishFetch error path must not set LoadState::Error when a mode switch
    // is pending (avoids transient error flash on cancel).
    {
        CHECK(sourceContains(apply, "m_offlineModeFetchPending"));
        // The guard must be: !m_haveCachedSnapshot && !m_offlineModeFetchPending
        CHECK(sourceContains(apply, "!m_haveCachedSnapshot && !m_offlineModeFetchPending"));
    }

    // Offline worker path must publish the cached snapshot + signature,
    // with the current catalog generation folded into the signature.  The
    // SDL thread applies those fields after taking the immutable handoff.
    {
        CHECK(sourceContains(sync, "pending.offlineSnapshotCache = pending.cachedSnapshot"));
        CHECK(sourceContains(sync, "pending.offlineCacheValid = true"));
        CHECK(sourceContains(sync, "pending.offlineSignature = computeOfflineSignature(downloads,"));
        CHECK(sourceContains(sync, "committedCatalogGeneration()"));
        CHECK(sourceContains(apply, "m_offlineSnapshotCache = presentation.offlineSnapshotCache"));
        CHECK(sourceContains(apply, "m_haveOfflineSnapshotCache = true"));
        CHECK(sourceContains(apply, "m_offlineSignature = presentation.offlineSignature"));
        CHECK(sourceContains(apply, "m_haveOfflineSignature = true"));
    }

    // tryApplyCachedOfflineSnapshot must exist and use the cache.
    {
        CHECK(sourceContains(offline, "HomeScreen::tryApplyCachedOfflineSnapshot()"));
        CHECK(sourceContains(offline, "m_haveOfflineSignature"));
        CHECK(sourceContains(offline, "m_haveOfflineSnapshotCache"));
        CHECK(sourceContains(offline, "computeOfflineSignature(downloads,"));
        CHECK(sourceContains(offline, "committedCatalogGeneration()"));
        CHECK(sourceContains(offline, "m_offlineSnapshotCache"));
        CHECK(sourceContains(offline, "applyPresentationProjection()"));
    }

    // computeOfflineSignature must exist and use isAvailable.
    {
        CHECK(sourceContains(offline, "HomeScreen::computeOfflineSignature("));
        CHECK(sourceContains(offline, "OfflineLibraryQuery::isAvailable"));
    }

    std::printf("[test] Offline toggle cancels in-flight fetch and uses cache OK\n");
}
