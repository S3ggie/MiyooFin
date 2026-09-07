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
static void testShowsPresentation()
{
    std::printf("[test] shows title, anime classification, and grid helpers\n");
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
