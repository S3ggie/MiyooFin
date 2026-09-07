static MediaItem cacheItem(const std::string &id) { MediaItem i; i.id=id;i.title="\xCE\xA9 \"title\"";i.overview="line one\nline two";i.year=2024;i.rating=8.5f;i.genre="Drama";i.type="movie";i.etag="etag-"+id;i.genres={"Drama","Sci-Fi"};i.played=true;i.progress=.5f;i.playbackPositionTicks=987654;i.imageTags["Primary"]="tag-"+id;i.indexNumber=2;i.parentIndexNumber=3;i.runTimeTicks=10000000;i.seriesName="series";i.seriesId="sid";i.seasonId="seid";i.artR=1;i.artG=2;i.artB=3;return i; }
static bool sameItem(const MediaItem&a,const MediaItem&b){return a.id==b.id&&a.title==b.title&&a.overview==b.overview&&a.year==b.year&&a.rating==b.rating&&a.genre==b.genre&&a.type==b.type&&a.etag==b.etag&&a.genres==b.genres&&a.played==b.played&&a.progress==b.progress&&a.playbackPositionTicks==b.playbackPositionTicks&&a.imageTags==b.imageTags&&a.indexNumber==b.indexNumber&&a.parentIndexNumber==b.parentIndexNumber&&a.runTimeTicks==b.runTimeTicks&&a.seriesName==b.seriesName&&a.seriesId==b.seriesId&&a.seasonId==b.seasonId&&a.artR==b.artR&&a.artG==b.artG&&a.artB==b.artB;}
static void testLibraryCacheNew(){std::string d="/tmp/miyoofin-cache-test";mkdir(d.c_str(),0755);std::string p=d+"/x.bin";LibrarySnapshot s;CachedLibraryView v;v.id="v";v.name="Movies";v.collectionType="movies";for(int i=0;i<500;i++)v.items.push_back(cacheItem(std::to_string(i)));s.movies.push_back(v);s.shows.push_back({"s","Shows","tvshows",{cacheItem("show")}});CHECK(LibraryCache::save(p,s));LibrarySnapshot got;CHECK(LibraryCache::load(p,got));CHECK(got.movies.size()==1&&got.movies[0].items.size()==500&&sameItem(got.movies[0].items[0],s.movies[0].items[0]));FILE*f=fopen(p.c_str(),"r+b");fputc('X',f);fclose(f);LibrarySnapshot keep=s;CHECK(!LibraryCache::load(p,keep));CHECK(keep.movies[0].items.size()==500);}
static void testLibraryCacheV3SaveLoad(){std::printf("[test] LibraryCache v3 save/load and needsRefresh\n");std::string d="/tmp/miyoofin-cache-v3-test-"+std::to_string((long long)getpid());mkdir(d.c_str(),0755);std::string p=d+"/snap.bin";LibrarySnapshot s;CachedLibraryView v;v.id="v";v.name="Movies";v.collectionType="movies";v.items.push_back(cacheItem("m1"));s.movies.push_back(v);s.shows.push_back({"s","Shows","tvshows",{cacheItem("show1")}});s.continueWatching.push_back(cacheItem("cw1"));s.recentlyAdded.push_back(cacheItem("ra1"));CHECK(LibraryCache::save(p,s));LibrarySnapshot got;bool needsRefresh=true;CHECK(LibraryCache::load(p,got,nullptr,&needsRefresh));CHECK(!needsRefresh);CHECK(got.movies.size()==1&&got.movies[0].items.size()==1);CHECK(sameItem(got.movies[0].items[0],s.movies[0].items[0]));CHECK(got.shows.size()==1&&got.shows[0].items.size()==1);CHECK(sameItem(got.shows[0].items[0],s.shows[0].items[0]));CHECK(got.continueWatching.size()==1);CHECK(got.recentlyAdded.size()==1);CHECK(sameItem(got.continueWatching[0],s.continueWatching[0]));CHECK(sameItem(got.recentlyAdded[0],s.recentlyAdded[0]));std::printf("[test] LibraryCache v3 save/load and needsRefresh OK\n");}
static void testLibraryCacheV2BackCompat(){std::printf("[test] LibraryCache v2 back-compat loads with needsRefresh\n");std::string d="/tmp/miyoofin-cache-v2-test-"+std::to_string((long long)getpid());mkdir(d.c_str(),0755);std::string p=d+"/snap.bin";LibrarySnapshot s;CachedLibraryView v;v.id="v";v.name="Movies";v.collectionType="movies";v.items.push_back(cacheItem("m1"));s.movies.push_back(v);s.shows.push_back({"s","Shows","tvshows",{cacheItem("show1")}});s.continueWatching.push_back(cacheItem("cw1"));s.recentlyAdded.push_back(cacheItem("ra1"));CHECK(LibraryCache::save(p,s));{FILE *f=fopen(p.c_str(),"r+b");CHECK(f!=nullptr);fseek(f,4,SEEK_SET);unsigned char v2[4]={2,0,0,0};fwrite(v2,1,4,f);fclose(f);}LibrarySnapshot got;bool needsRefresh=false;CHECK(LibraryCache::load(p,got,nullptr,&needsRefresh));CHECK(needsRefresh);CHECK(got.movies.size()==1&&got.movies[0].items.size()==1);CHECK(sameItem(got.movies[0].items[0],s.movies[0].items[0]));CHECK(got.continueWatching.size()==1);CHECK(got.recentlyAdded.size()==1);std::printf("[test] LibraryCache v2 back-compat loads with needsRefresh OK\n");}
static void testLibraryCacheV1BackCompat(){std::printf("[test] LibraryCache v1 back-compat loads with needsRefresh\n");std::string d="/tmp/miyoofin-cache-v1-test-"+std::to_string((long long)getpid());mkdir(d.c_str(),0755);std::string p=d+"/snap.bin";std::vector<unsigned char> b;auto u32=[&b](uint32_t n){for(int i=0;i<4;i++)b.push_back((unsigned char)(n>>(i*8)));};auto str=[&u32,&b](const std::string& s){u32((uint32_t)s.size());b.insert(b.end(),s.begin(),s.end());};b={'M','F','L','C'};u32(1);u32(1);str("v1mov");str("Movies");str("movies");u32(0);u32(1);str("v1shw");str("Shows");str("tvshows");u32(0);FILE*f=fopen(p.c_str(),"wb");CHECK(f!=nullptr);fwrite(b.data(),1,b.size(),f);fclose(f);LibrarySnapshot got;bool needsRefresh=false;CHECK(LibraryCache::load(p,got,nullptr,&needsRefresh));CHECK(needsRefresh);CHECK(got.movies.size()==1);CHECK(got.movies[0].id=="v1mov");CHECK(got.movies[0].items.empty());CHECK(got.shows.size()==1);CHECK(got.shows[0].id=="v1shw");CHECK(got.shows[0].items.empty());CHECK(got.continueWatching.empty());CHECK(got.recentlyAdded.empty());std::printf("[test] LibraryCache v1 back-compat loads with needsRefresh OK\n");}
static void testLibraryCacheUnknownVersion(){std::printf("[test] LibraryCache unknown version rejected\n");std::string d="/tmp/miyoofin-cache-unk-test-"+std::to_string((long long)getpid());mkdir(d.c_str(),0755);std::string p=d+"/snap.bin";LibrarySnapshot s;CachedLibraryView v;v.id="v";v.name="Movies";v.collectionType="movies";s.movies.push_back(v);CHECK(LibraryCache::save(p,s));{FILE*f=fopen(p.c_str(),"r+b");CHECK(f!=nullptr);fseek(f,4,SEEK_SET);unsigned char bad[4]={99,0,0,0};fwrite(bad,1,4,f);fclose(f);}LibrarySnapshot got;bool needsRefresh=true;CHECK(!LibraryCache::load(p,got,nullptr,&needsRefresh));std::printf("[test] LibraryCache unknown version rejected OK\n");}
static bool librarySyncShouldFetch(bool haveCache,bool cacheNeedsRefresh,bool haveSyncState,bool syncFresh){return !haveCache||cacheNeedsRefresh||!haveSyncState||!syncFresh;}
static void testLibraryCacheFetchDecision(){std::printf("[test] librarySyncShouldFetch decision logic\n");CHECK(!librarySyncShouldFetch(true,false,true,true));CHECK(librarySyncShouldFetch(true,false,true,false));CHECK(librarySyncShouldFetch(true,true,true,true));CHECK(librarySyncShouldFetch(true,true,true,false));CHECK(librarySyncShouldFetch(false,false,true,true));CHECK(librarySyncShouldFetch(false,false,false,false));CHECK(librarySyncShouldFetch(false,false,true,false));std::printf("[test] librarySyncShouldFetch decision logic OK\n");}
static void testSyncState(){std::string p="/tmp/miyoofin-sync-state-"+std::to_string((long long)getpid())+"/state";SyncState a;a.lastSuccessfulMs=1000;a.lastReconcileMs=800;CHECK(SyncStateStore::save(p,a));SyncState b;CHECK(SyncStateStore::load(p,b)&&b.lastSuccessfulMs==1000&&b.lastReconcileMs==800);CHECK(syncStateFresh(b,1000+15*60*1000-1,15*60*1000));CHECK(!syncStateFresh(b,1000+15*60*1000,15*60*1000));FILE*f=fopen(p.c_str(),"wb");CHECK(f!=nullptr);if(f){fputs("broken",f);fclose(f);}SyncState keep=b;CHECK(!SyncStateStore::load(p,keep));CHECK(keep.lastSuccessfulMs==1000);}
static std::string readFixture(const std::string &path){std::string out;FILE*f=fopen(path.c_str(),"rb");if(!f)return out;char b[128];size_t n;while((n=fread(b,1,sizeof(b),f)))out.append(b,n);fclose(f);return out;}
static void testOfflineCatalog(){std::string root="/tmp/miyoofin-offline-catalog-test-"+std::to_string((long long)getpid()),p=root+"/catalog";MediaItem s=cacheItem("series");s.title="Séries 世界";MediaItem season=cacheItem("season");season.seriesId=s.id;MediaItem ep=cacheItem("episode");ep.seriesId=s.id;ep.seasonId=season.id;ep.indexNumber=2;ep.parentIndexNumber=1;
    // Season planning stores its series, season and episodes without requiring a screen visit.
    CHECK(OfflineCatalog::storeDiscoveredHierarchy(p,s,{season},{{season.id,{ep}}}));OfflineCatalogSnapshot x;CHECK(OfflineCatalog::load(p,x));CHECK(x.series[s.id].title==s.title&&x.seasonsBySeries[s.id][0].id==season.id);auto &got=x.episodesBySeason[season.id][0];CHECK(got.id==ep.id&&got.indexNumber==2&&got.parentIndexNumber==1&&got.runTimeTicks==ep.runTimeTicks&&got.playbackPositionTicks==ep.playbackPositionTicks);
    // Series planning merges all fetched seasons/episodes, retains other cached series, and de-duplicates IDs.
    MediaItem other=cacheItem("other-series"),season2=cacheItem("season-2"),ep2=cacheItem("episode-2");season2.seriesId=s.id;ep2.seriesId=s.id;ep2.seasonId=season2.id;CHECK(OfflineCatalog::storeSeasons(p,other,{cacheItem("other-season")}));CHECK(OfflineCatalog::storeDiscoveredHierarchy(p,s,{season,season2,season2},{{season.id,{ep,ep}},{season2.id,{ep2,ep2}}}));CHECK(OfflineCatalog::load(p,x));CHECK(x.series.count(other.id)==1&&x.seasonsBySeries[s.id].size()==(size_t)2&&x.episodesBySeason[season.id].size()==(size_t)1&&x.episodesBySeason[season2.id].size()==(size_t)1);
    // An incomplete planner result is a no-op and cannot erase valid cached hierarchy.
    CHECK(OfflineCatalog::storeDiscoveredHierarchy(p,s,{},{{season.id,{}}},false));CHECK(OfflineCatalog::load(p,x));CHECK(x.seasonsBySeries[s.id].size()==(size_t)2&&x.episodesBySeason[season.id].size()==(size_t)1);
    // A successful authoritative top-level listing prunes deleted series and
    // their hierarchy only; failed/incomplete refreshes above never do.
    CHECK(OfflineCatalog::reconcileSeries(p,{other}));CHECK(OfflineCatalog::load(p,x));CHECK(!x.series.count(s.id)&&x.series.count(other.id)&&!x.seasonsBySeries.count(s.id)&&!x.episodesBySeason.count(season.id));
    // Corrupting metadata never rewrites it or touches a real downloaded-item fixture.
    DownloadStore store(root+"/downloads");std::string scope="fixture",itemId="downloaded-item";DownloadItem downloaded;downloaded.itemId=itemId;downloaded.expectedSize=5;downloaded.chunkSize=5;downloaded.state=DownloadState::Complete;downloaded.downloadedBytes=5;CHECK(store.saveManifest(scope,downloaded));CHECK(store.saveIndex(scope,{downloaded}));std::string chunks=store.itemPath(scope,itemId)+"/chunks";CHECK(mkdir(chunks.c_str(),0755)==0);FILE*chunk=fopen(store.chunkPath(scope,itemId,0).c_str(),"wb");fwrite("hello",1,5,chunk);fclose(chunk);std::string manifestBefore=readFixture(store.manifestPath(scope,itemId)),chunkBefore=readFixture(store.chunkPath(scope,itemId,0));FILE*f=fopen(p.c_str(),"wb");fputs("bad",f);fclose(f);std::string catalogBefore=readFixture(p);OfflineCatalogSnapshot keep=x;CHECK(!OfflineCatalog::load(p,keep));CHECK(!OfflineCatalog::storeDiscoveredHierarchy(p,s,{season},{{season.id,{ep}}}));CHECK(readFixture(p)==catalogBefore);CHECK(readFixture(store.manifestPath(scope,itemId))==manifestBefore);CHECK(readFixture(store.chunkPath(scope,itemId,0))==chunkBefore);DownloadItem loaded;CHECK(store.loadManifest(scope,itemId,loaded));CHECK(store.validateCompletedDownload(scope,loaded));std::vector<DownloadItem> index;CHECK(store.loadIndex(scope,index));CHECK(index.size()==(size_t)1&&index[0].itemId==itemId);}
static void testSeasonPosterScheduling(){
    std::string old=ImageCache::cacheDir(); ImageCache::setCacheDir("/tmp/miyoofin-season-poster-test/");
    MediaItem season=cacheItem("season-poster"); season.type="season"; MediaItem duplicate=season, episode=cacheItem("episode-thumb"); episode.type="episode";
    auto jobs=HomeScreen::collectSeasonPosterJobs({season,duplicate});
    CHECK(jobs.size()==(size_t)1 && jobs[0].width==74 && jobs[0].height==111 && jobs[0].itemId==season.id);
    unsigned char bytes[1]={0}; CHECK(ImageCache::writeToCache(season.id,ImageType::Primary,season.imageTags["Primary"],74,111,bytes,1));
    CHECK(HomeScreen::collectSeasonPosterJobs({season}).empty());
    // The global hierarchy prefetch accepts only seasons; episode thumbnails
    // stay exclusively in EpisodeBrowserScreen's visible/on-demand queue.
    CHECK(HomeScreen::collectSeasonPosterJobs({episode}).empty());
    ImageCache::removeCached(season.id,ImageType::Primary,season.imageTags["Primary"],74,111); ImageCache::setCacheDir(old);
}
static void testOfflineLibraryProjection(){
    LibrarySnapshot l; MediaItem movie=cacheItem("movie"),partial=cacheItem("partial"),show=cacheItem("show"),anime=cacheItem("anime"),season=cacheItem("s1"),empty=cacheItem("s2"),ep1=cacheItem("e1"),ep3=cacheItem("e3"),remote=cacheItem("remote");
    movie.type=partial.type="movie"; movie.playbackPositionTicks=50; show.type=anime.type="show"; show.title="Show"; anime.title="Anime"; season.seriesId="stale-series"; empty.seriesId.clear();
    ep1.type=ep3.type=remote.type="episode"; ep1.seriesId=ep3.seriesId=remote.seriesId=show.id; ep1.seasonId=ep3.seasonId=remote.seasonId=season.id; ep1.indexNumber=1; ep3.indexNumber=3; ep1.playbackPositionTicks=25;
    l.movies={{"m","Movies","movies",{movie,partial}}}; l.shows={{"tv","Shows","tvshows",{show}},{"a","Anime","tvshows",{anime}}}; l.continueWatching={remote,movie,ep1,partial}; l.recentlyAdded={remote};
    // The map key is authoritative: stale and missing season parents are
    // normalized before applying the complete-download visibility filter.
    OfflineCatalogSnapshot c; c.series[show.id]=show; c.series[anime.id]=anime; c.seasonsBySeries[show.id]={season,empty}; c.episodesBySeason[season.id]={ep1,ep3,remote};
    DownloadSnapshot d; DownloadItem dm; dm.itemId=movie.id; dm.itemType="movie"; dm.state=DownloadState::Complete; dm.updatedAt=20;
    DownloadItem de1; de1.itemId=ep1.id; de1.itemType="episode"; de1.state=DownloadState::Complete; de1.updatedAt=30; de1.seriesId=show.id; de1.seasonId=season.id;
    DownloadItem de3=de1; de3.itemId=ep3.id; de3.updatedAt=10;
    DownloadItem failed=dm; failed.itemId=partial.id; failed.state=DownloadState::Failed;
    DownloadItem queued=dm; queued.itemId="queued"; queued.state=DownloadState::Queued;
    DownloadItem paused=dm; paused.itemId="partial"; paused.state=DownloadState::Paused;
    d.items={dm,de1,de3,failed,queued,paused};
    for(int n=0;n<8;++n) { DownloadItem extra=dm; extra.itemId="extra-"+std::to_string(n); extra.title="Extra"; extra.updatedAt=40+n; d.items.push_back(extra); }
    OfflineLibraryProjection p(l,c,d);
    CHECK(p.movies().size()==1&&p.movies()[0].id==movie.id); CHECK(p.series().size()==1&&p.series()[0].id==show.id); CHECK(p.seasons(show.id).size()==1&&p.seasons(show.id)[0].id==season.id); CHECK(p.episodes(season.id).size()==2&&p.episodes(season.id)[1].id==ep3.id); CHECK(!p.playable(partial.id));
    // Online retains Home; offline has no Home projection or curation surface.
    auto online=tabsFromSnapshot(l);
    auto offline=offlineTabsFromSnapshot(l);
    const auto onlineNames=tabNames(online), offlineNames=tabNames(offline);
    CHECK((onlineNames==std::vector<std::string>{"Home","Movies","Shows","Downloads","Settings"}));
    CHECK((offlineNames==std::vector<std::string>{"Movies","Shows","Downloads","Settings"}));
    CHECK(std::find(offlineNames.begin(),offlineNames.end(),"Home")==offlineNames.end());
    // Cold offline startup and a selected Home both use the Movies fallback.
    CHECK(transitionTabIndex({},0,offline)==0);
    CHECK(transitionTabIndex(online,0,offline)==0);
    // Other named tabs survive layout changes where possible.
    CHECK(transitionTabIndex(online,2,offline)==1);
    CHECK(transitionTabIndex(online,3,offline)==2);
    CHECK(transitionTabIndex(online,4,offline)==3);
    CHECK(transitionTabIndex(offline,0,online)==1);
    CHECK(transitionTabIndex(offline,1,online)==2);
    CHECK(transitionTabIndex(offline,2,online)==3);
    CHECK(transitionTabIndex(offline,3,online)==4);
}
static void testSettingsRowActions(){
    CHECK(HomeScreen::settingsRowCount() == 9);
    CHECK(HomeScreen::settingsRowAction(0)==HomeScreen::SettingsRowAction::OfflineMode);
    CHECK(HomeScreen::settingsRowAction(1)==HomeScreen::SettingsRowAction::ChangeServer);
    CHECK(HomeScreen::settingsRowAction(2)==HomeScreen::SettingsRowAction::LocalAddress);
    CHECK(HomeScreen::settingsRowAction(3)==HomeScreen::SettingsRowAction::None);
    CHECK(HomeScreen::settingsRowAction(7)==HomeScreen::SettingsRowAction::None);
    CHECK(HomeScreen::settingsRowAction(8)==HomeScreen::SettingsRowAction::Logout);
    CHECK(HomeScreen::settingsRowAction(9)==HomeScreen::SettingsRowAction::None);
}

static void testLanServerAddressClassificationAndSettingsLayout(){
    std::printf("[test] LAN server address classification and Settings layout\n");
    CHECK(isObviousLanServerUrl("http://10.0.0.1:8096"));
    CHECK(isObviousLanServerUrl("http://172.16.0.1:8096"));
    CHECK(isObviousLanServerUrl("http://172.31.255.255:8096"));
    CHECK(isObviousLanServerUrl("http://192.168.1.212:8096"));
    CHECK(isObviousLanServerUrl("http://127.0.0.1:8096"));
    CHECK(isObviousLanServerUrl("http://LOCALHOST:8096"));
    CHECK(!isObviousLanServerUrl("http://172.15.255.255:8096"));
    CHECK(!isObviousLanServerUrl("http://172.32.0.1:8096"));
    CHECK(!isObviousLanServerUrl("http://8.8.8.8:8096"));
    CHECK(!isObviousLanServerUrl("https://public.example.com"));
    CHECK(!isObviousLanServerUrl("http://jellyfin.local:8096"));
    CHECK(!isObviousLanServerUrl("http://192.168.1.212:70000"));
    CHECK(!isObviousLanServerUrl("not a url"));

    Session lanOnly; lanOnly.serverUrl="http://192.168.1.212:8096";
    const auto lanRows=HomeScreen::settingsAddressRows(lanOnly);
    CHECK(lanRows.size()==2);
    CHECK_EQ(lanRows[0].section,"LAN Server");
    CHECK_EQ(lanRows[0].value,"http://192.168.1.212:8096");
    CHECK(lanRows[0].action==HomeScreen::SettingsRowAction::ChangeServer);
    CHECK_EQ(lanRows[1].section,"Public Address");
    CHECK_EQ(lanRows[1].value,"Not Set");
    CHECK(lanRows[1].action==HomeScreen::SettingsRowAction::PublicAddress);
    CHECK(HomeScreen::settingsRowCount(lanOnly)==10);
    CHECK(HomeScreen::settingsRowAction(1,lanOnly)==HomeScreen::SettingsRowAction::ChangeServer);
    CHECK(HomeScreen::settingsRowAction(2,lanOnly)==HomeScreen::SettingsRowAction::PublicAddress);
    lanOnly.publicServerUrl="https://public.example.com";
    CHECK_EQ(HomeScreen::settingsAddressRows(lanOnly)[1].value,"https://public.example.com");

    Session publicOnly; publicOnly.serverUrl="https://public.example.com";
    const auto publicRows=HomeScreen::settingsAddressRows(publicOnly);
    CHECK(publicRows.size()==2);
    CHECK_EQ(publicRows[0].section,"Public Server");
    CHECK_EQ(publicRows[0].value,"https://public.example.com");
    CHECK_EQ(publicRows[1].section,"Local Address");
    CHECK_EQ(publicRows[1].value,"Not Set");
    CHECK(HomeScreen::settingsRowAction(2,publicOnly)==HomeScreen::SettingsRowAction::LocalAddress);

    Session dualRoute=publicOnly; dualRoute.localServerUrl="http://192.168.1.212:8096";
    const auto dualRows=HomeScreen::settingsAddressRows(dualRoute);
    CHECK(dualRows.size()==2);
    CHECK_EQ(dualRows[0].section,"Public Server");
    CHECK_EQ(dualRows[1].section,"Local Address");
    CHECK_EQ(dualRows[1].value,"http://192.168.1.212:8096");
}
static void testSeriesCachedSeasonHandoff(){
    Session session; MediaItem show=cacheItem("show"),cached=cacheItem("cached"),fresh=cacheItem("fresh"); show.type="show"; cached.type=fresh.type="season";
    // A nonempty RAM handoff is ready before enter(), so push has no catalog I/O.
    SeriesScreen warm(session,show,{},false,{cached}); CHECK(warm.diagnosticSeasonsReady()); CHECK(warm.diagnosticSeasons().size()==(size_t)1&&warm.diagnosticSeasons()[0].id==cached.id);
    // With no handoff, the constructor preserves the existing Loading state.
    SeriesScreen cold(session,show); CHECK(!cold.diagnosticSeasonsReady());
    // The later network completion still replaces the handed-off hierarchy.
    int selected=0; auto replaced=SeriesScreen::replaceSeasonsKeepingSelection({cached},{fresh},selected); CHECK(replaced.size()==(size_t)1&&replaced[0].id==fresh.id&&selected==0);
    // Offline callers hand over the existing downloaded-only projection, never
    // its raw catalog seasons.
    MediaItem remote=cacheItem("remote"); remote.type="season"; OfflineCatalogSnapshot catalog; catalog.seasonsBySeries[show.id]={cached,remote}; MediaItem episode=cacheItem("episode"); episode.type="episode"; episode.seriesId=show.id; episode.seasonId=cached.id; catalog.episodesBySeason[cached.id]={episode}; DownloadItem download; download.itemId=episode.id; download.state=DownloadState::Complete; DownloadSnapshot downloads; downloads.items={download}; LibrarySnapshot library; OfflineLibraryProjection projection(library,catalog,downloads); SeriesScreen offline(session,show,{},true,projection.seasons(show.id)); CHECK(offline.diagnosticSeasonsReady()&&offline.diagnosticSeasons().size()==(size_t)1&&offline.diagnosticSeasons()[0].id==cached.id);
}
static void testNewGridAndSchedule(){CHECK(moveMovieGrid(8,10,0,1)==8);CHECK(moveMovieGrid(9,10,0,-1)==9);CHECK(moveMovieGrid(8,10,1,0)==9);CHECK(clampMovieGridScroll(36,100,0)==1);MovieArtworkRange a=movieVisibleArtworkRange(0,36);CHECK(a.first==0&&a.lastExclusive==36);a=movieVisibleArtworkRange(0,37);CHECK(a.first==0&&a.lastExclusive==36);a=movieVisibleArtworkRange(1,37);CHECK(a.first==9&&a.lastExclusive==37);a=movieVisibleArtworkRange(4,100);CHECK(a.first==36&&a.lastExclusive==72);a=movieVisibleArtworkRange(9,100);CHECK(a.first==81&&a.lastExclusive==100);
    // A successful sync is fresh for fifteen minutes, and tab requests while
    // in flight coalesce instead of spawning another worker.
    LibrarySyncSchedule q; CHECK(q.request(0)); CHECK(!q.request(1)); CHECK(q.pending); CHECK(!q.complete(10,true)); CHECK(!q.request(10+LibrarySyncSchedule::FRESH_MS-1)); CHECK(q.request(10+LibrarySyncSchedule::FRESH_MS));
    // Failed cached refreshes wait before another normal-navigation attempt.
    LibrarySyncSchedule failed; CHECK(failed.request(0)); failed.complete(10,false); CHECK(!failed.request(11)); CHECK(failed.request(10+LibrarySyncSchedule::RETRY_DELAY_MS));
    // Shows progress is real, clamped and monotonic; poster work is not an input.
    ShowsSyncProgress zero{0,120}, mid{48,120}, done{120,120}, overflow{140,120}, regressed{12,120}; CHECK(zero.percent()==0); CHECK(mid.percent()==40); CHECK(done.percent()==100); CHECK(overflow.percent()==100); CHECK(regressed.percent(40)==40);
    // Cached content remains browseable and explicitly reports offline rather
    // than promoting a transient network failure to a fatal browsing error.
    CHECK_EQ(librarySyncStatus(0,true,true,false,true),"OFFLINE");
    CHECK_EQ(librarySyncStatus(1,true,false,true,true),"SYNCING...");
    CHECK_EQ(librarySyncStatus(2,true,false,false,true,mid,true),"SYNC 40%");
    CHECK_EQ(librarySyncStatus(2,true,false,false,true,done,false),"SYNCED");
}
static void testCacheRemoveNew(){std::string old=ImageCache::cacheDir();ImageCache::setCacheDir("/tmp/miyoofin-images/");unsigned char b[2]={1,2};CHECK(ImageCache::writeToCache("a",ImageType::Primary,"x",64,96,b,2));CHECK(ImageCache::writeToCache("b",ImageType::Primary,"x",64,96,b,2));CHECK(ImageCache::removeCached("a",ImageType::Primary,"x",64,96));CHECK(!ImageCache::isCached("a",ImageType::Primary,"x",64,96));CHECK(ImageCache::isCached("b",ImageType::Primary,"x",64,96));ImageCache::setCacheDir(old);}
