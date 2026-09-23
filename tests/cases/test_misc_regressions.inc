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
    // The live-change/reconcile wiring that used to be pinned here is
    // exercised behaviourally by test_library_coordinator
    // (live-change catch-up, serialized slots, safety reconcile) and by
    // test_home_library_controller for the Home publication handoff.
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

// Home artwork scheduling and poster/decode worker ownership are covered
// behaviourally by test_home_artwork_controller (queue ordering, deferral,
// promotion, decode bounds, stale rejection, stop).  The former source-text
// scheduling scan had no runtime-equivalent-only call-site observation that
// was not already covered there, so it was removed.

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
    std::printf("[test] Recently Added warm publication OK\n");
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
    CHECK(HomeScreen::restoreSelectionIndex(
              {titledShow("show-a", "Show A"), titledShow("anime-b", "Anime B")},
              "anime-b", 0) == 1);
    CHECK(HomeScreen::restoreSelectionIndex(
              {titledShow("anime-c", "Anime C")}, "trimmed", 4) == 0);
    std::printf("[test] Anime selection survives shared window trim OK\n");
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

static void testShowsPresentation()
{
    std::printf("[test] shows title, anime classification, and grid helpers\n");
    testDecideHomeStartupSync();
    testHomeIncrementalPublicationPolicy();
    testLibraryChangedEventQueueAndReconnectPolicy();
    testBoundedPagePosterPlanning();
    testHomeRailPosterPlanning();
    testShowsFocusPreservedDuringIncrementalRefresh();
    testHomeGridPositionHelpers();
    testRecentlyAddedWarmPublication();
    testWarmMediaWindowsExtraction();
    testAnimeSelectionSurvivesSharedWindowTrim();
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
    std::printf("[test] Home row focus reconciliation\n");
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
    std::printf("[test] Home row focus reconciliation OK\n");
}

// Home rail-refresh coordinator routing (single coordinator worker, cached
// partial-rail retention, coalescing, cancellation-on-teardown) is covered
// behaviourally by test_library_coordinator
// (testHomeRailSuccessPublishesBothRails, testHomeRailCachedFailureRetainsInvalidRail,
// testHomeRailCoalescesAndRerunsAfterConsumption, testHomeRailStopPublishesCompletion)
// and by test_home_library_controller for the Home fetch handoff.
//
// Card-surface cache eviction is render-thread/SDL state that cannot be
// observed without a live screen; the former source pin is removed rather
// than pretending a unit test covers it.

static void testNonBlockingFinishPolicy()
{
    std::printf("[test] Non-blocking finish* join policy\n");
    // Architecture guard (class A): finish* handlers run on the SDL/UI
    // thread and must never join a worker; only start* handlers join.  Each
    // body is scoped so the sibling start* functions cannot satisfy the
    // absence check.  (Behavioral UI-thread timing is not deterministically
    // observable on the host.)
    const std::string syncApply = readTestBytes("src/ui/screens/HomeScreenSyncApply.cpp");
    CHECK(!syncApply.empty());
    const std::string homeRailBody =
        sourceFunction(syncApply, "void HomeScreen::finishHomeRailRefresh(");
    CHECK(!homeRailBody.empty());
    CHECK(sourceLacks(homeRailBody, ".join()"));
    const std::string safetyBody =
        sourceFunction(syncApply, "void HomeScreen::finishSafetyReconcile(");
    CHECK(!safetyBody.empty());
    CHECK(sourceLacks(safetyBody, ".join()"));

    const std::string refresh = readTestBytes("src/ui/screens/HomeScreenRefresh.cpp");
    CHECK(!refresh.empty());
    const std::string downloadBody =
        sourceFunction(refresh, "void HomeScreen::finishDownloadRefresh(");
    CHECK(!downloadBody.empty());
    CHECK(sourceLacks(downloadBody, ".join()"));
    std::printf("[test] Non-blocking finish* join policy OK\n");
}

static void testLivePublicationNoopEmptyResult()
{
    std::printf("[test] livePublicationIsNoop truth table\n");
    // Pure predicate: true only when BOTH containers are empty.  The gating
    // call site runs on the fetch worker and is exercised end-to-end by the
    // coordinator live-change publication tests.
    CHECK(livePublicationIsNoop(true, true));
    CHECK(!livePublicationIsNoop(true, false));
    CHECK(!livePublicationIsNoop(false, true));
    CHECK(!livePublicationIsNoop(false, false));
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

// The page-upsert transaction/cleanup invariants (RAII statement ownership,
// rollback-before-failure, bounded retry, terminal result publication) are
// exercised behaviourally by the catalog tests through the injected failure
// seam (testCatalogDbMediaPageUpsertAndPopulation rollback/atomicity,
// testCatalogDbTopLevelSyncStagingLifecycle, and the migration/bootstrap
// tests).  The former exhaustive source-shape scan is removed.
// The Settings OfflineMode toggle path (cancel in-flight fetch, try the
// cached offline snapshot, re-fetch when the pending flag is set) runs on the
// UI thread against live worker state and has no deterministic host seam.
// Its worker/publish half is covered by test_home_library_controller's
// offline fetch test; the former source pin is removed rather than pretending
// a unit test covers the UI-thread race.

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

    std::printf("[test] Offline snapshot signature stability/change detection OK\n");
}

// The offline-toggle cancel/fetch race runs against live worker threads on
// the UI thread and has no deterministic host seam.  Its worker/publish half
// is covered behaviourally by test_home_library_controller's offline fetch
// test and the offline snapshot signature tests; the former source-shape pin
// is removed rather than pretending a unit test covers the race.
