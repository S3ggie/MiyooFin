#include "../../src/library/LibraryQuery.hpp"
#include "../../src/library/LibraryCoordinator.hpp"
#include "../../src/net/TlsConfig.hpp"

static void testDownloadInterruptStates()
{
    std::printf("[test] download interrupt state mapping\n");
    CHECK(stateAfterInterrupt(DownloadInterrupt::Playback) == DownloadState::PausedForPlayback);
    CHECK(stateAfterInterrupt(DownloadInterrupt::UserPause) == DownloadState::Paused);
    CHECK(stateAfterInterrupt(DownloadInterrupt::Cancel) == DownloadState::Failed);
    CHECK(stateAfterInterrupt(DownloadInterrupt::Shutdown) == DownloadState::Failed);
}

static DownloadItem planItem(const std::string &id, std::uint64_t size, std::uint64_t done=0) {
    DownloadItem i; i.itemId=id; i.expectedSize=size; i.downloadedBytes=done; return i;
}

static void writeFixture(const std::string &path,const char *data,size_t size){FILE *f=fopen(path.c_str(),"wb");CHECK(f!=nullptr);if(f){CHECK(fwrite(data,1,size,f)==size);fclose(f);}}
static void testHlsDownloadStore(){
    std::printf("[test] HLS manifest and segment storage\n");
    std::string root="/tmp/miyoofin-hls-"+std::to_string((long long)getpid()); DownloadStore store(root); DownloadItem h;
    h.itemId="hls-item";h.mediaSourceId="source";h.expectedSize=999;h.chunkSize=1;h.hlsStorage=true;h.hlsSegmentCount=0;h.hlsProfile="test-profile";h.state=DownloadState::Downloading;
    // Playlist discovery is durable before segment zero has completed.
    h.hlsSegmentCount=3;
    CHECK(store.ensureHlsDirectories("scope",h.itemId)); CHECK(store.saveManifest("scope",h)); DownloadItem loaded; CHECK(store.loadManifest("scope",h.itemId,loaded));
    CHECK(loaded.hlsStorage&&loaded.itemId==h.itemId&&loaded.mediaSourceId=="source"&&loaded.hlsSegmentCount==3&&loaded.hlsProfile=="test-profile");
    DownloadItem legacy=h;legacy.itemId="legacy";legacy.hlsStorage=false;legacy.expectedSize=1; CHECK(store.saveManifest("scope",legacy)); CHECK(readFixture(store.manifestPath("scope",h.itemId)).rfind("MFDM=2\n",0)==0); CHECK(readFixture(store.manifestPath("scope",legacy.itemId)).rfind("MFDM=1\n",0)==0);
    writeFixture(store.segmentPath("scope",h.itemId,0),"abc",3); writeFixture(store.segmentPath("scope",h.itemId,1),"de",2); writeFixture(store.segmentPath("scope",h.itemId,2,true),"x",1);
    CHECK(store.isCompleteSegment("scope",h.itemId,0)); CHECK(!store.isCompleteSegment("scope",h.itemId,2)); CHECK(store.firstIncompleteSegment("scope",h)==2); CHECK(::access(store.segmentPath("scope",h.itemId,2,true).c_str(),F_OK)==0); // interrupted part is retained
    // A retry starts the same incomplete segment from a fresh .part and must
    // not alter media that was already completed before the failed attempt.
    writeFixture(store.segmentPath("scope",h.itemId,2,true),"proxy error body",16);
    writeFixture(store.segmentPath("scope",h.itemId,2,true),"retry-media",11);
    CHECK(readFixture(store.segmentPath("scope",h.itemId,2,true))=="retry-media");
    CHECK(readFixture(store.segmentPath("scope",h.itemId,0))=="abc");
    store.reconcile("scope",h); CHECK(h.downloadedBytes==5); CHECK(!store.validateCompletedDownload("scope",h)); // missing segment prevents completion
    writeFixture(store.segmentPath("scope",h.itemId,2),"fghi",4); CHECK(store.firstIncompleteSegment("scope",h)==3); store.reconcile("scope",h); CHECK(h.downloadedBytes==9); CHECK(store.validateCompletedDownload("scope",h)); CHECK(h.state==DownloadState::Complete);
    std::printf("[test] HLS manifest and segment storage OK\n");
}
static DownloadItem restartFixture(const std::string &id, DownloadState state=DownloadState::Queued) {
    DownloadItem item; item.itemId=id; item.chunkSize=1; item.hlsStorage=true;
    item.hlsSegmentCount=2; item.hlsProfile="test"; item.state=state; return item;
}
static void testDownloadRestartPersistence(){
    std::printf("[test] download restart persistence\n");
    const std::string root="/tmp/miyoofin-restart-"+std::to_string((long long)getpid());
    DownloadStore store(root);
    // Enqueue is UI-safe, while the worker persists each manifest before the
    // shared index. Playback holds transfers back so both remain queued.
    { DownloadManager manager({},root); manager.setPlaybackActive(true);
      DownloadItem one=restartFixture("queued-one"), two=restartFixture("queued-two");
      manager.enqueue({one,two});
      for(unsigned n=0;n<100;n++){std::vector<DownloadItem> saved;if(store.loadIndex("anonymous",saved,nullptr)&&saved.size()==2)break;usleep(10000);}
      std::vector<DownloadItem> saved; CHECK(store.loadIndex("anonymous",saved)); CHECK(saved.size()==2);
      for(const auto &item:saved){DownloadItem manifest;CHECK(store.loadManifest("anonymous",item.itemId,manifest));CHECK(manifest.state==DownloadState::Queued);}
    }
    { DownloadManager restarted({},root); restarted.setPlaybackActive(true); auto snapshot=restarted.snapshot();
      CHECK(snapshot.items.size()==2); }

    // A corrupt index which loaded one manifest before finding a missing one
    // rebuilds from a clean vector; it cannot duplicate the first item.
    DownloadItem active=restartFixture("active",DownloadState::Downloading), queued=restartFixture("queued",DownloadState::Queued);
    CHECK(store.ensureHlsDirectories("fixture",active.itemId)); CHECK(store.ensureHlsDirectories("fixture",queued.itemId));
    writeFixture(store.segmentPath("fixture",active.itemId,0),"partial",7);
    CHECK(store.saveManifest("fixture",active)); CHECK(store.saveManifest("fixture",queued));
    DownloadItem recovered, stillQueued; CHECK(store.loadManifest("fixture","active",recovered)); CHECK(store.reconcile("fixture",recovered)); CHECK(recovered.state==DownloadState::Queued);
    CHECK(store.loadManifest("fixture","queued",stillQueued)); CHECK(store.reconcile("fixture",stillQueued)); CHECK(stillQueued.state==DownloadState::Queued);
    FILE *bad=fopen((store.scopePath("fixture")+"/index.v1").c_str(),"wb"); CHECK(bad!=nullptr); if(bad){fputs("MFDI=1\nactive\nmissing\n",bad);fclose(bad);}
    Session session;session.serverUrl="http://fixture";session.userId="user";
    { DownloadManager restarted(session,root); restarted.setPlaybackActive(true); auto snapshot=restarted.snapshot();
      CHECK(snapshot.items.size()==2); std::set<std::string> ids; for(const auto&i:snapshot.items)ids.insert(i.itemId); CHECK(ids.size()==2); }
    CHECK(store.isCompleteSegment("fixture","active",0));
    std::printf("[test] download restart persistence OK\n");
}

static void testDownloadsUiHelpers()
{
    std::printf("[test] Downloads UI helpers\n");
    CHECK(std::string(downloadStateLabel(DownloadState::Queued)) == "Queued");
    CHECK(std::string(downloadStateLabel(DownloadState::PausedForPlayback)) == "Paused for playback");
    CHECK(std::string(downloadStateLabel(DownloadState::NoSpace)) == "No space");
    DownloadItem episode; episode.itemType="episode"; episode.title="Slumber Party Panic"; episode.seasonNumber=1; episode.episodeNumber=1;
    CHECK(episodeDownloadLabel(episode) == "S01E01 Slumber Party Panic");
    episode.expectedSize=200; episode.downloadedBytes=87; CHECK(downloadPercent(episode)==43);
    DownloadItem hls; hls.hlsStorage=true; hls.hlsSegmentCount=4; hls.expectedSize=1000000; // storage estimate must not affect active percentage
    hls.hlsCompletedSegments=2; hls.downloadedBytes=1; CHECK(downloadPercent(hls)==50);
    hls.hlsCurrentSegmentBytes=25; hls.hlsCurrentSegmentSize=100; CHECK(downloadPercent(hls)==56); hls.hlsActivePercent=56;
    hls.hlsCurrentSegmentBytes=1; hls.hlsCurrentSegmentSize=0; CHECK(downloadPercent(hls)==56); // monotonic when a retry has no length
    hls.hlsCompletedSegments=4; hls.hlsActivePercent=99; CHECK(downloadPercent(hls)==99); // validation owns 100%
    hls.state=DownloadState::Complete; CHECK(downloadPercent(hls)==100);
    CHECK(downloadPrimaryControl(DownloadState::Complete)==DownloadPrimaryControl::Play);
    CHECK(downloadPrimaryControl(DownloadState::Downloading)==DownloadPrimaryControl::Pause);
    CHECK(downloadPrimaryControl(DownloadState::Paused)==DownloadPrimaryControl::Resume);
    CHECK(downloadPrimaryControl(DownloadState::WaitingForNetwork)==DownloadPrimaryControl::Retry);
    CHECK(downloadPrimaryControl(DownloadState::Unauthorized)==DownloadPrimaryControl::None);
    CHECK(clampDownloadSelection(4, 2)==1); CHECK(clampDownloadSelection(0, 0)==0);
    CHECK(clampDownloadScroll(6, 7, 0, 5)==2); CHECK(clampDownloadScroll(0, 0, 3, 5)==0);
    DownloadItem complete; complete.itemId="done"; complete.state=DownloadState::Complete;
    DownloadItem incomplete; incomplete.itemId="later"; incomplete.state=DownloadState::Queued;
    CHECK(downloadCanRemove(complete)); CHECK(downloadRemoveIsDelete(complete));
    CHECK(downloadCanRemove(incomplete)); CHECK(!downloadRemoveIsDelete(incomplete));
    CHECK(!downloadCanRemove(DownloadItem{}));
    CHECK(!downloadRemovalConfirmed("", incomplete));
    CHECK(!downloadRemovalConfirmed("other", incomplete));
    CHECK(downloadRemovalConfirmed("later", incomplete));
    std::printf("[test] Downloads UI helpers OK\n");
}

static void testDownloadHierarchy()
{
    std::printf("[test] download hierarchy\n");
    DownloadSnapshot snapshot;
    DownloadItem movie=planItem("movie",100,50); movie.itemType="movie"; movie.title="A Movie";
    DownloadItem a1=planItem("a1",100,100); a1.itemType="episode"; a1.title="Second"; a1.seriesId="a"; a1.seriesName="The Alpha Show"; a1.seasonId="a2"; a1.seasonName="Season 2"; a1.seasonNumber=2; a1.episodeNumber=2; a1.state=DownloadState::Complete;
    DownloadItem a0=planItem("a0",100,25); a0.itemType="episode"; a0.title="First"; a0.seriesId="a"; a0.seriesName="The Alpha Show"; a0.seasonId="a1"; a0.seasonName="Season 1"; a0.seasonNumber=1; a0.episodeNumber=1; a0.state=DownloadState::Downloading;
    DownloadItem b=planItem("b",200,200); b.itemType="episode"; b.title="Only"; b.seriesId="b"; b.seriesName="Breaking Bad"; b.seasonId="b1"; b.seasonNumber=1; b.episodeNumber=1; b.state=DownloadState::Complete;
    snapshot.items={movie,a1,a0,b};
    auto collapsed=buildDownloadHierarchy(snapshot,{});
    CHECK(collapsed.movies.size()==(size_t)1); CHECK(collapsed.shows.size()==(size_t)2); // movies separated; multiple series
    CHECK(collapsed.shows[0].title=="The Alpha Show"); // article-aware ordering
    CHECK(collapsed.shows[0].aggregate.episodes==2 && collapsed.shows[0].aggregate.active==1);
    CHECK(collapsed.shows[0].aggregate.progress==62); // (25 + complete 100) / 2
    std::set<std::string> expanded{"series:a","series:a/season:a1","series:a/season:a2"};
    auto open=buildDownloadHierarchy(snapshot,expanded);
    CHECK(open.visible.size()==(size_t)7); // movie, 2 series, 2 seasons, 2 episodes
    CHECK(open.shows[1].kind==DownloadHierarchyRowKind::Season && open.shows[1].title=="Season 1");
    CHECK(open.shows[2].kind==DownloadHierarchyRowKind::Episode && open.shows[2].item->itemId=="a0");
    CHECK(open.shows[3].kind==DownloadHierarchyRowKind::Season && open.shows[3].title=="Season 2");
    // Rebuild preserves expansion by stable IDs and selection by logical row.
    DownloadSnapshot refreshed=snapshot;
    auto after=buildDownloadHierarchy(refreshed,expanded);
    int selected=downloadHierarchySelection(after.visible,"series:a/season:a1",4);
    CHECK(after.shows[0].expanded && after.shows[1].expanded); CHECK(after.visible[selected].id=="series:a/season:a1");
    // A deleted selected leaf clamps safely; an empty category does not create a section.
    CHECK(downloadHierarchySelection(after.visible,"episode:deleted",99)==(int)after.visible.size()-1);
    DownloadSnapshot episodesOnly; episodesOnly.items={a0}; CHECK(buildDownloadHierarchy(episodesOnly,{}).movies.empty());
    DownloadSnapshot moviesOnly; moviesOnly.items={movie}; CHECK(buildDownloadHierarchy(moviesOnly,{}).shows.empty());
    CHECK(downloadPrimaryControl(a0.state)==DownloadPrimaryControl::Pause); // leaf controls remain unchanged
    // Y is handled by Downloads for hierarchy parents, so it cannot reach
    // HomeScreen's global ActionsMenu/logout path.
    DownloadHierarchyRow seriesRow; seriesRow.kind=DownloadHierarchyRowKind::Series;
    DownloadHierarchyRow seasonRow; seasonRow.kind=DownloadHierarchyRowKind::Season;
    CHECK(downloadHierarchyConsumesActionsMenu(seriesRow));
    CHECK(downloadHierarchyConsumesActionsMenu(seasonRow));
    // Parent Y plans only matching local DownloadManager IDs.  The plan is
    // later executed solely through DownloadManager::erase, never a server API.
    const DownloadHierarchyRow &alphaSeries=open.shows[0];
    const DownloadHierarchyRow &alphaSeason=open.shows[1];
    auto seasonDelete=downloadHierarchyBulkRemovalItemIds(alphaSeason,snapshot);
    CHECK(seasonDelete.size()==(size_t)1 && seasonDelete[0]=="a0");
    auto seriesDelete=downloadHierarchyBulkRemovalItemIds(alphaSeries,snapshot);
    CHECK(seriesDelete.size()==(size_t)2);
    CHECK(std::find(seriesDelete.begin(),seriesDelete.end(),"a0")!=seriesDelete.end());
    CHECK(std::find(seriesDelete.begin(),seriesDelete.end(),"a1")!=seriesDelete.end());
    CHECK(std::find(seriesDelete.begin(),seriesDelete.end(),"b")==seriesDelete.end());
    CHECK(downloadHierarchyBulkRemovalConfirmed(alphaSeason.id,alphaSeason));
    CHECK(!downloadHierarchyBulkRemovalConfirmed(alphaSeries.id,alphaSeason));
    DownloadSnapshot afterSeason=snapshot;
    afterSeason.items.erase(afterSeason.items.begin()+2);
    auto afterSeasonHierarchy=buildDownloadHierarchy(afterSeason,expanded);
    CHECK(std::none_of(afterSeasonHierarchy.shows.begin(),afterSeasonHierarchy.shows.end(),[](const DownloadHierarchyRow &row){ return row.id=="series:a/season:a1"; }));
    CHECK(std::any_of(afterSeasonHierarchy.shows.begin(),afterSeasonHierarchy.shows.end(),[](const DownloadHierarchyRow &row){ return row.id=="series:b"; }));
    DownloadSnapshot afterSeries=snapshot;
    afterSeries.items.erase(afterSeries.items.begin()+1,afterSeries.items.begin()+3);
    auto afterSeriesHierarchy=buildDownloadHierarchy(afterSeries,expanded);
    CHECK(afterSeriesHierarchy.shows.size()==(size_t)1 && afterSeriesHierarchy.shows[0].title=="Breaking Bad");
    std::printf("[test] download hierarchy OK\n");
}

static void testDownloadSourceReconciliation()
{
    std::printf("[test] download source reconciliation\n");
    DownloadItem item; item.itemId="item"; item.mediaSourceId="source-a"; item.sourceEtag="etag-a"; item.expectedSize=100; item.downloadedBytes=100; item.state=DownloadState::Complete;
    DownloadMediaSource source; source.id="source-a"; source.etag="etag-a"; source.size=100;
    CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.state==DownloadState::Complete);
    source.etag="etag-b"; CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.state==DownloadState::UpdateAvailable&&item.sourceEtag=="etag-a");
    item.state=DownloadState::Complete; item.updateAvailable=false; source.etag="etag-a"; source.size=101; CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.state==DownloadState::UpdateAvailable);
    item.state=DownloadState::Complete; item.updateAvailable=false; source.size=100; source.id="source-b"; CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.state==DownloadState::UpdateAvailable);
    item.state=DownloadState::Complete; CHECK(!reconcileSource(item,SourceCheck::Missing)); CHECK(item.state==DownloadState::LocalOnly);
    CHECK(!reconcileSource(item,SourceCheck::Transient)); CHECK(item.state==DownloadState::LocalOnly); CHECK(!reconcileSource(item,SourceCheck::Unauthorized)); CHECK(item.state==DownloadState::LocalOnly);
    item.state=DownloadState::Paused; item.mediaSourceId="source-a"; item.sourceEtag="etag-a"; item.expectedSize=100; item.downloadedBytes=45; source.id="source-b"; source.etag="etag-b"; source.size=120;
    CHECK(reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.downloadedBytes==0&&item.expectedSize==120&&item.state==DownloadState::Queued);
    item.state=DownloadState::Complete; item.downloadedBytes=100; item.mediaSourceId="source-a"; item.sourceEtag="etag-a"; item.expectedSize=100; CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.downloadedBytes==100&&item.expectedSize==100);
    std::string root="output/test/download-source-fixture-"+std::to_string((long long)getpid()); Session session; session.serverUrl="http://server"; session.userId="user"; session.accessToken="token"; session.deviceId="device";
    DownloadStore store(root); std::string scope=DownloadStore::scopeKey(session.serverUrl,session.userId); DownloadItem local; local.itemId="item"; local.expectedSize=4; local.chunkSize=4; local.downloadedBytes=4; local.state=DownloadState::UpdateAvailable; CHECK(store.saveManifest(scope,local)); CHECK(::mkdir((store.itemPath(scope,"item")+"/chunks").c_str(),0755)==0); FILE *f=std::fopen(store.chunkPath(scope,"item",0).c_str(),"wb"); CHECK(f!=nullptr); std::fwrite("data",1,4,f); std::fclose(f); CHECK(store.saveIndex(scope,{local}));
    DownloadManager d(session,root); MediaItem media; media.id="item"; CHECK(resolvePlayback(media,d,true)==PlaybackSource::Local); local.state=DownloadState::LocalOnly; CHECK(store.saveManifest(scope,local)); d.configure(session); CHECK(resolvePlayback(media,d,true)==PlaybackSource::Local);
    DownloadItem updated; updated.state=DownloadState::UpdateAvailable; CHECK(downloadIsLocal(local)); CHECK(downloadIsLocal(updated));
    DownloadItem partial; partial.itemId="partial"; partial.expectedSize=4; partial.chunkSize=4; CHECK(store.saveManifest(scope,partial)); CHECK(::mkdir((store.itemPath(scope,"partial")+"/chunks").c_str(),0755)==0); f=std::fopen(store.chunkPath(scope,"partial",0,true).c_str(),"wb"); CHECK(f!=nullptr); std::fwrite("xx",1,2,f); std::fclose(f); CHECK(store.removePartialBytes(scope,"partial")); CHECK(::access(store.chunkPath(scope,"partial",0,true).c_str(),F_OK)!=0);
    std::printf("[test] download source reconciliation OK\n");
}

static void testDownloadPlanBatchAccounting()
{
    // A manager with an invalid session never performs HTTP; makePlan is pure
    // with respect to its input and the manager's local queue.
    char rootTemplate[]="/tmp/miyoofin-download-plan-XXXXXX";
    char *rootPath=::mkdtemp(rootTemplate);
    CHECK(rootPath!=nullptr);
    if (!rootPath) return;
    const std::string root(rootPath);
    std::string scope;
    {
        DownloadManager d({},root);
        d.setPlaybackActive(true);
        // enqueue is called by the SDL confirmation handler.  Playback pauses
        // transfer selection, but the worker may persist the queued state.
        DownloadItem deferred=planItem("deferred",100);
        d.enqueue(std::vector<DownloadItem>{deferred,planItem("deferred-two",100)});
        scope=d.scope();
        DownloadStore deferredStore(root);
        bool persisted=false;
        for(unsigned n=0;n<100;n++){
            std::vector<DownloadItem> saved;
            if(deferredStore.loadIndex(scope,saved,nullptr)&&saved.size()==2){persisted=true;break;}
            usleep(10000);
        }
        CHECK(persisted);
        DownloadItem savedDeferred;
        CHECK(deferredStore.loadManifest(scope,"deferred",savedDeferred));
        CHECK(savedDeferred.state==DownloadState::Queued);
        auto pausedSnapshot=d.snapshot();
        CHECK(pausedSnapshot.playbackActive);
        CHECK(pausedSnapshot.items.size()==2);
        CHECK(std::all_of(pausedSnapshot.items.begin(),pausedSnapshot.items.end(),[](const DownloadItem&i){return i.state==DownloadState::Queued;}));
        // No HLS segment bytes may be created while playback is active.
        CHECK(::access(deferredStore.segmentPath(scope,"deferred",0).c_str(),F_OK)!=0);
        CHECK(::access(deferredStore.segmentPath(scope,"deferred",0,true).c_str(),F_OK)!=0);
        auto duplicate=d.makePlan({planItem("a",100),planItem("a",100),planItem("b",200)});
        CHECK(duplicate.items.size() == (size_t)2); // season / cross-season dedupe
        CHECK(duplicate.additionalRequiredBytes == (std::uint64_t)300);
        auto unknown=d.makePlan({planItem("unknown",0)});
        CHECK(!unknown.sizeKnown); CHECK(!unknown.error.empty());
        CHECK(!DownloadManager::acceptsPlanResult(7,8)); // stale planner generation is ignored
        CHECK(DownloadManager::acceptsPlanResult(8,8));
        std::vector<DownloadItem> large; for(int n=0;n<128;++n) large.push_back(planItem("episode"+std::to_string(n),1));
        CHECK(d.makePlan(large).items.size() == (size_t)128); // large series stays one plan
        d.enqueue(planItem("partial",100,40));
        auto partial=d.makePlan({planItem("partial",100)});
        CHECK(partial.alreadyPresentBytes == (std::uint64_t)40);
        CHECK(partial.additionalRequiredBytes == (std::uint64_t)0); // its remaining 60 is reserved once
        d.enqueue(planItem("complete",100,100));
        CHECK(d.makePlan({planItem("complete",100)}).additionalRequiredBytes == (std::uint64_t)0);
        auto queued=d.makePlan({planItem("partial",100),planItem("new",10)});
        CHECK(queued.additionalRequiredBytes == (std::uint64_t)10); // queued partial is not double-reserved
    }
    DownloadStore cleanup(root);
    for (const char *itemId : {"deferred","deferred-two","partial","complete"})
        cleanup.removeItem(scope,itemId,nullptr);
    std::remove((cleanup.scopePath(scope)+"/index.v1").c_str());
    std::remove((cleanup.scopePath(scope)+"/index.v1.tmp").c_str());
    ::rmdir((cleanup.scopePath(scope)+"/items").c_str());
    ::rmdir(cleanup.scopePath(scope).c_str());
    ::rmdir(root.c_str());
}

static void testHlsSizeEstimates()
{
    CHECK(EpisodeBrowserScreen::hasSeparateDownloadActions()); // Episode and Season controls coexist.
    std::uint64_t bytes=0;
    CHECK(estimateHlsBytes(22LL*60*HLS_TICKS_PER_SECOND,bytes)); CHECK(bytes == 224597536ULL);
    CHECK(estimateHlsBytes(45LL*60*HLS_TICKS_PER_SECOND,bytes)); CHECK(bytes == 459335536ULL);
    CHECK(estimateHlsBytes(2LL*60*60*HLS_TICKS_PER_SECOND,bytes)); CHECK(bytes == 1224785536ULL);
    CHECK(!estimateHlsBytes(0,bytes)); CHECK(estimateHlsBytes((std::numeric_limits<std::int64_t>::max)(),bytes)); CHECK(bytes > 100000000000000ULL);
    DownloadManager d({}, "output/test/hls-plan-fixture-"+std::to_string((long long)getpid())); d.setPlaybackActive(true);
    auto hls=[](const std::string&id,std::int64_t ticks){DownloadItem i;i.itemId=id;i.hlsStorage=true;i.runtimeTicks=ticks;i.expectedSize=9999999999ULL;return i;};
    auto season=d.makePlan({hls("one",22LL*60*HLS_TICKS_PER_SECOND),hls("two",45LL*60*HLS_TICKS_PER_SECOND)});
    CHECK(season.sizeKnown&&season.additionalRequiredBytes == 683933072ULL); // season total
    auto series=d.makePlan({hls("one",22LL*60*HLS_TICKS_PER_SECOND),hls("two",45LL*60*HLS_TICKS_PER_SECOND),hls("three",2LL*60*60*HLS_TICKS_PER_SECOND)});
    CHECK(series.sizeKnown&&series.additionalRequiredBytes == 1908718608ULL); // series total
    CHECK(!d.makePlan({hls("unknown",0)}).sizeKnown);
    // Cached metadata produces an HLS estimate before PlaybackInfo is fetched.
    DownloadItem metadataOnly=hls("metadata-only",22LL*60*HLS_TICKS_PER_SECOND);
    CHECK(d.makePlan({metadataOnly}).sizeKnown);
    DownloadItem complete=hls("actual",22LL*60*HLS_TICKS_PER_SECOND); complete.state=DownloadState::Complete; complete.downloadedBytes=12345;
    CHECK(displayDownloadBytes(complete)==12345);
    // Queue accounting switches from the conservative estimate to actual HLS
    // segment progress as soon as the active playlist is known.
    DownloadItem active=hls("active",22LL*60*HLS_TICKS_PER_SECOND); active.state=DownloadState::Downloading; active.hlsSegmentCount=4; active.hlsCompletedSegments=2; active.downloadedBytes=20ULL*1024*1024;
    CHECK(queueRemainingBytes(active)==20ULL*1024*1024);
    active.state=DownloadState::Complete;
    CHECK(queueRemainingBytes(active)==0);
    DownloadItem queued=hls("queued",22LL*60*HLS_TICKS_PER_SECOND); queued.state=DownloadState::Queued;
    CHECK(queueRemainingBytes(queued)==queued.expectedSize); // no playlist: retain reservation estimate
}

static void testHlsFailureClassification()
{
    std::printf("[test] recent transfer speed\n");
    RecentSpeedSample speed; std::uint64_t bytesPerSec=0;
    CHECK(!DownloadManager::updateRecentSpeed(speed,100,1000,bytesPerSec));
    CHECK(DownloadManager::updateRecentSpeed(speed,600,1500,bytesPerSec));
    CHECK(bytesPerSec==1000);
    // A short HLS segment gap retains the rolling rate.
    CHECK(!DownloadManager::updateRecentSpeed(speed,600,2900,bytesPerSec));
    CHECK(bytesPerSec==1000);
    // A gap > STALL_GAP_MS (5s) zeros the displayed rate.
    CHECK(DownloadManager::updateRecentSpeed(speed,600,7001,bytesPerSec));
    CHECK(bytesPerSec==0);
    // A new download (a lower byte count) resets rather than carrying speed.
    CHECK(!DownloadManager::updateRecentSpeed(speed,0,7100,bytesPerSec));
    CHECK(bytesPerSec==0);
    std::printf("[test] recent transfer speed OK\n");

    CHECK(DownloadManager::hlsSegmentRetryable(520, CURLE_OK));
    CHECK(DownloadManager::hlsSegmentRetryable(502, CURLE_OK));
    CHECK(DownloadManager::hlsSegmentRetryable(503, CURLE_OK));
    CHECK(DownloadManager::hlsSegmentRetryable(504, CURLE_OK));
    CHECK(DownloadManager::hlsSegmentRetryable(0, CURLE_OPERATION_TIMEDOUT));
    CHECK(DownloadManager::hlsSegmentRetryable(0, CURLE_RECV_ERROR));
    CHECK(!DownloadManager::hlsSegmentRetryable(401, CURLE_OK));
    CHECK(!DownloadManager::hlsSegmentRetryable(403, CURLE_OK));
    CHECK(!DownloadManager::hlsSegmentRetryable(404, CURLE_OK));
    CHECK(!DownloadManager::hlsSegmentRetryable(0, CURLE_ABORTED_BY_CALLBACK));
    CHECK(DownloadManager::hlsSegmentShouldRetry(520, CURLE_OK, 1)); // a later attempt can succeed
    CHECK(DownloadManager::hlsSegmentShouldRetry(520, CURLE_OK, 4));
    CHECK(!DownloadManager::hlsSegmentShouldRetry(520, CURLE_OK, DownloadManager::HLS_SEGMENT_ATTEMPTS)); // exhausted
    CHECK(JellyfinApi::classifyHlsFailure(0, CURLE_OPERATION_TIMEDOUT) == JellyfinApi::HlsFailure::Timeout);
    CHECK(JellyfinApi::classifyHlsFailure(0, CURLE_COULDNT_CONNECT) == JellyfinApi::HlsFailure::Network);
    CHECK(JellyfinApi::classifyHlsFailure(401, CURLE_OK) == JellyfinApi::HlsFailure::Unauthorized);
    CHECK(JellyfinApi::classifyHlsFailure(403, CURLE_OK) == JellyfinApi::HlsFailure::Unauthorized);
    CHECK(JellyfinApi::classifyHlsFailure(500, CURLE_OK) == JellyfinApi::HlsFailure::Http);
    CHECK(JellyfinApi::classifyHlsFailure(404, CURLE_OK) == JellyfinApi::HlsFailure::Http);
    CHECK(DownloadManager::hlsSegmentFailureState(0, CURLE_OPERATION_TIMEDOUT) == DownloadState::WaitingForNetwork);
    CHECK(DownloadManager::hlsSegmentFailureState(0, CURLE_COULDNT_CONNECT) == DownloadState::WaitingForNetwork);
    CHECK(DownloadManager::hlsSegmentFailureState(0, CURLE_COULDNT_RESOLVE_HOST) == DownloadState::WaitingForNetwork);
    CHECK(DownloadManager::hlsSegmentFailureState(401, CURLE_OK) == DownloadState::Unauthorized);
    CHECK(DownloadManager::hlsSegmentFailureState(500, CURLE_OK) == DownloadState::Failed);
    CHECK(DownloadManager::hlsSegmentFailureState(404, CURLE_OK) == DownloadState::Failed);
}

static void testStartupReconcileSkip()
{
    std::printf("[test] startup reconcile skip predicate\n");
    DownloadItem complete; complete.itemId="c"; complete.state=DownloadState::Complete;
    CHECK(startupReconcileShouldSkip(complete));

    DownloadItem localOnly; localOnly.itemId="l"; localOnly.state=DownloadState::LocalOnly;
    CHECK(!startupReconcileShouldSkip(localOnly));

    DownloadItem queued; queued.itemId="q"; queued.state=DownloadState::Queued;
    CHECK(!startupReconcileShouldSkip(queued));

    DownloadItem downloading; downloading.itemId="d"; downloading.state=DownloadState::Downloading;
    CHECK(!startupReconcileShouldSkip(downloading));

    DownloadItem paused; paused.itemId="p"; paused.state=DownloadState::Paused;
    CHECK(!startupReconcileShouldSkip(paused));

    DownloadItem failed; failed.itemId="f"; failed.state=DownloadState::Failed;
    CHECK(!startupReconcileShouldSkip(failed));

    DownloadItem updateAvail; updateAvail.itemId="u"; updateAvail.state=DownloadState::UpdateAvailable;
    CHECK(!startupReconcileShouldSkip(updateAvail));

    DownloadItem serverMissing; serverMissing.itemId="s"; serverMissing.state=DownloadState::ServerMissing;
    CHECK(!startupReconcileShouldSkip(serverMissing));

    std::printf("[test] startup reconcile skip predicate OK\n");
}

static void testReconcileShouldSkip()
{
    std::printf("[test] reconcileShouldSkip truth table\n");
    constexpr std::uint64_t TTL = 24ULL * 60 * 60 * 1000;
    constexpr std::uint64_t NOW_MS = 1000000ULL * 1000; // arbitrary monotonic anchor

    // --- Non-Complete items never skip ---
    DownloadItem queued; queued.itemId="q"; queued.state=DownloadState::Queued;
    CHECK(!reconcileShouldSkip(queued, false, NOW_MS));
    CHECK(!reconcileShouldSkip(queued, true,  NOW_MS));

    DownloadItem dl; dl.itemId="d"; dl.state=DownloadState::Downloading;
    CHECK(!reconcileShouldSkip(dl, false, NOW_MS));
    CHECK(!reconcileShouldSkip(dl, true,  NOW_MS));

    DownloadItem paused; paused.itemId="p"; paused.state=DownloadState::Paused;
    CHECK(!reconcileShouldSkip(paused, false, NOW_MS));

    DownloadItem failed; failed.itemId="f"; failed.state=DownloadState::Failed;
    CHECK(!reconcileShouldSkip(failed, false, NOW_MS));

    DownloadItem updateAvail; updateAvail.itemId="u"; updateAvail.state=DownloadState::UpdateAvailable;
    CHECK(!reconcileShouldSkip(updateAvail, false, NOW_MS));

    DownloadItem localOnly; localOnly.itemId="l"; localOnly.state=DownloadState::LocalOnly;
    CHECK(!reconcileShouldSkip(localOnly, false, NOW_MS));

    // --- Complete + startup pass → always skip ---
    DownloadItem completeStartup; completeStartup.itemId="cs";
    completeStartup.state=DownloadState::Complete;
    CHECK(reconcileShouldSkip(completeStartup, true, NOW_MS));

    // --- Complete + manual pass + never verified → check ---
    DownloadItem completeNever; completeNever.itemId="cn";
    completeNever.state=DownloadState::Complete;
    completeNever.lastVerifiedMs=0;
    CHECK(!reconcileShouldSkip(completeNever, false, NOW_MS));

    // --- Complete + manual pass + recently verified → skip ---
    DownloadItem completeRecent; completeRecent.itemId="cr";
    completeRecent.state=DownloadState::Complete;
    completeRecent.lastVerifiedMs=NOW_MS - 1000; // 1 second ago
    CHECK(reconcileShouldSkip(completeRecent, false, NOW_MS));

    // --- Complete + manual pass + verified exactly TTL-1 ms ago → skip ---
    DownloadItem completeEdge; completeEdge.itemId="ce";
    completeEdge.state=DownloadState::Complete;
    completeEdge.lastVerifiedMs=NOW_MS - (TTL - 1);
    CHECK(reconcileShouldSkip(completeEdge, false, NOW_MS));

    // --- Complete + manual pass + verified exactly TTL ms ago → check ---
    DownloadItem completeExpired; completeExpired.itemId="cx";
    completeExpired.state=DownloadState::Complete;
    completeExpired.lastVerifiedMs=NOW_MS - TTL;
    CHECK(!reconcileShouldSkip(completeExpired, false, NOW_MS));

    // --- Complete + manual pass + verified beyond TTL → check ---
    DownloadItem completeOld; completeOld.itemId="co";
    completeOld.state=DownloadState::Complete;
    completeOld.lastVerifiedMs=NOW_MS - TTL - 1;
    CHECK(!reconcileShouldSkip(completeOld, false, NOW_MS));

    // --- Failed check still records timestamp, next entry skipped ---
    DownloadItem completeFailed; completeFailed.itemId="cf";
    completeFailed.state=DownloadState::Complete;
    completeFailed.lastVerifiedMs=0;
    CHECK(!reconcileShouldSkip(completeFailed, false, NOW_MS));
    // Simulate recording the attempt
    completeFailed.lastVerifiedMs=NOW_MS;
    CHECK(reconcileShouldSkip(completeFailed, false, NOW_MS));

    std::printf("[test] reconcileShouldSkip truth table OK\n");
}

static void testReconcilerPreservesLiveState()
{
    std::printf("[test] reconciler preserves concurrent live state\n");
    // The reconciler must apply through the real worker apply step
    // (applyReconciledSource), which merges into the LIVE item under the
    // lock and never assigns a pass-start snapshot over it.  A regression
    // that reintroduces a snapshot/whole-item assignment fails below: the
    // paused/playback state set after the snapshot would be reverted.
    const std::uint64_t nowMs=987654321ULL;
    DownloadItem live; live.itemId="live"; live.mediaSourceId="s"; live.sourceEtag="e";
    live.expectedSize=100; live.downloadedBytes=40; live.state=DownloadState::Queued;
    DownloadItem passStart=live; // worker's pass-start snapshot: apply must not copy this back
    (void)passStart;
    // Concurrent user pause lands between the snapshot and the apply.
    live.state=stateAfterInterrupt(DownloadInterrupt::UserPause);
    CHECK(live.state==DownloadState::Paused);
    CHECK(!applyReconciledSource(live,SourceCheck::Transient,nullptr,nowMs));
    CHECK(live.state==DownloadState::Paused); // pause not clobbered by the snapshot
    CHECK(live.downloadedBytes==40);
    CHECK(live.lastVerifiedMs==nowMs);
    // Playback interrupts are equally sticky: worker() only dequeues Queued,
    // so reverting a PausedForPlayback item to the snapshot state could
    // strand it in Downloading or restart it spuriously.
    live.state=DownloadState::Downloading;
    live.state=stateAfterInterrupt(DownloadInterrupt::Playback);
    CHECK(live.state==DownloadState::PausedForPlayback);
    CHECK(!applyReconciledSource(live,SourceCheck::Transient,nullptr,nowMs));
    CHECK(live.state==DownloadState::PausedForPlayback);
    CHECK(live.lastVerifiedMs==nowMs);
    // A changed source still resets an incomplete item (discard=true) even
    // when the live state moved since the snapshot was taken.
    DownloadMediaSource changed; changed.id="s2"; changed.etag="e2"; changed.size=120;
    live.state=DownloadState::Paused; live.downloadedBytes=40; live.expectedSize=100;
    live.mediaSourceId="s"; live.sourceEtag="e";
    CHECK(applyReconciledSource(live,SourceCheck::Same,&changed,nowMs));
    CHECK(live.state==DownloadState::Queued);
    CHECK(live.downloadedBytes==0);
    CHECK(live.expectedSize==120);
    CHECK(live.lastVerifiedMs==nowMs);
    std::printf("[test] reconciler preserves concurrent live state OK\n");
}

static void testTransferPreservesLiveState()
{
    std::printf("[test] transfer preserves concurrent live state\n");
    // Segment progress published by the transfer: the live item is one
    // segment behind, as after a real rename-then-lock window.
    DownloadItem progress; progress.itemId="x"; progress.hlsStorage=true;
    progress.hlsProfile="test"; progress.hlsSegmentCount=4; progress.expectedSize=400;
    progress.hlsCompletedSegments=3; progress.downloadedBytes=300;
    progress.hlsCurrentSegmentBytes=0; progress.hlsCurrentSegmentSize=0;
    progress.hlsActivePercent=75; progress.state=DownloadState::Downloading;
    // A user pause landing between the rename and the lock acquisition must
    // survive the publish: progress fields advance, state stays Paused, and
    // the worker/UI abort predicate (state != Downloading) still fires.
    DownloadItem live=progress; live.state=DownloadState::Paused;
    live.hlsCompletedSegments=2; live.downloadedBytes=200; live.hlsActivePercent=50;
    publishTransferProgress(live,progress);
    CHECK(live.state==DownloadState::Paused);
    CHECK(live.hlsCompletedSegments==3);
    CHECK(live.downloadedBytes==300);
    CHECK(live.hlsSegmentCount==4);
    CHECK(live.hlsStorage);
    CHECK(live.state!=DownloadState::Downloading); // abort-on-pause still observable
    // A playback interrupt is equally sticky: reverting it to Downloading
    // would strand the item, since setPlaybackActive(false) only restores
    // PausedForPlayback items.
    live=progress; live.state=DownloadState::PausedForPlayback;
    live.hlsCompletedSegments=2; live.downloadedBytes=200; live.hlsActivePercent=50;
    publishTransferProgress(live,progress);
    CHECK(live.state==DownloadState::PausedForPlayback);
    CHECK(live.hlsCompletedSegments==3);
    CHECK(live.downloadedBytes==300);
    // Terminal failure states are never forced back to Downloading either,
    // while progress fields still advance.
    live=progress; live.state=DownloadState::WaitingForNetwork;
    live.hlsCompletedSegments=2; live.downloadedBytes=200;
    publishTransferProgress(live,progress);
    CHECK(live.state==DownloadState::WaitingForNetwork);
    CHECK(live.hlsCompletedSegments==3);
    CHECK(live.downloadedBytes==300);
    live=progress; live.state=DownloadState::Failed;
    live.hlsCompletedSegments=2; live.downloadedBytes=200;
    publishTransferProgress(live,progress);
    CHECK(live.state==DownloadState::Failed);
    CHECK(live.downloadedBytes==300);
    // The active path is unchanged: Queued/Downloading still reads as
    // Downloading while transferring.
    live=progress; live.state=DownloadState::Queued;
    live.hlsCompletedSegments=2; live.downloadedBytes=200;
    publishTransferProgress(live,progress);
    CHECK(live.state==DownloadState::Downloading);
    CHECK(live.hlsCompletedSegments==3);
    CHECK(live.downloadedBytes==300);
    live=progress; live.state=DownloadState::Downloading;
    live.hlsCompletedSegments=2; live.downloadedBytes=200;
    publishTransferProgress(live,progress);
    CHECK(live.state==DownloadState::Downloading);
    CHECK(live.downloadedBytes==300);
    std::printf("[test] transfer preserves concurrent live state OK\n");
}

static void testDownloadPlanFullCardRejection()
{
    std::printf("[test] download plan full-card rejection\n");
    // A request that cannot fit must report canFit=false with "Not enough
    // space".  An astronomically large plain item exceeds any real free
    // space without touching the filesystem beyond the manager's root.
    char rootTemplate[]="/tmp/miyoofin-download-full-XXXXXX";
    char *rootPath=::mkdtemp(rootTemplate);
    CHECK(rootPath!=nullptr);
    if (!rootPath) return;
    DownloadManager d({},rootPath);
    d.setPlaybackActive(true); // no transfers run; makePlan stays pure
    const std::uint64_t huge=100ULL*1024*1024*1024*1024; // 100 TiB
    auto rejected=d.makePlan({planItem("huge",huge)});
    CHECK(rejected.sizeKnown);
    CHECK(!rejected.canFit);
    CHECK(rejected.error=="Not enough space");
    CHECK(rejected.additionalRequiredBytes==huge);
    // A zero-byte request still fits: only genuine shortage rejects.
    auto empty=d.makePlan({planItem("empty",0)});
    CHECK(!empty.sizeKnown);
    std::printf("[test] download plan full-card rejection OK\n");
}

static void testCatalogDbHierarchyPlanning()
{
    std::printf("[test] DownloadManager shared hierarchy policy\n");
    const std::string suffix=std::to_string((long long)getpid());
    Session session; session.serverUrl="https://download-catalog-"+suffix+".example"; session.userId="download-user-"+suffix;
    auto catalog=std::make_shared<CatalogDb>();
    const std::uint64_t epoch=catalog->configureScope(session.serverUrl,session.userId);
    CHECK(catalog->waitForIdleForTest(std::chrono::seconds(2)));
    const auto seeded=catalog->seedHierarchyQueryFixturesForTest();
    CHECK(seeded.success&&seeded.hierarchyFixture);
    DownloadManager manager(session,"output/test/catalog-plan-downloads-"+suffix);
    auto libraryCoordinator =
        std::make_shared<library::LibraryCoordinator>(session, catalog, epoch);
    manager.setLibraryServices(libraryCoordinator->query(), libraryCoordinator);
    MediaItem series; series.id="__task10_series__"; series.type="show";
    MediaItem season; season.id="__task10_season_a__"; season.type="season"; season.seriesId=series.id;
    const std::uint64_t planId=manager.requestSeasonPlan(series,season);
    DownloadPlanSnapshot snapshot;
    bool hierarchyEstimate=false;
    for(int i=0;i<500&&!hierarchyEstimate;++i){
        snapshot=manager.planSnapshot(planId);
        hierarchyEstimate=snapshot.itemCount==3&&snapshot.plan.items.size()==3;
        if(!hierarchyEstimate)usleep(10000);
    }
    CHECK(hierarchyEstimate);
    CHECK(epoch==catalog->scopeState().requestedEpoch);
    CHECK(catalog->getEpisodes(season.id).get().items.size()==(size_t)3);
    std::printf("[test] DownloadManager shared hierarchy policy OK\n");
}

static void testPredictedDownloadTotalBytes()
{
    std::printf("[test] predictedDownloadTotalBytes\n");
    // Complete item -> exact downloadedBytes.
    DownloadItem complete; complete.state=DownloadState::Complete;
    complete.downloadedBytes=5000; complete.expectedSize=8000;
    CHECK(predictedDownloadTotalBytes(complete)==5000);
    // LocalOnly -> exact downloadedBytes.
    DownloadItem local; local.state=DownloadState::LocalOnly;
    local.downloadedBytes=3000; local.expectedSize=6000;
    CHECK(predictedDownloadTotalBytes(local)==3000);
    // UpdateAvailable -> exact downloadedBytes.
    DownloadItem update; update.state=DownloadState::UpdateAvailable;
    update.downloadedBytes=1000; update.expectedSize=4000;
    CHECK(predictedDownloadTotalBytes(update)==1000);
    // Non-HLS -> expectedSize (or downloadedBytes if larger).
    DownloadItem plain; plain.state=DownloadState::Downloading;
    plain.expectedSize=2000; plain.downloadedBytes=500;
    CHECK(predictedDownloadTotalBytes(plain)==2000);
    plain.downloadedBytes=3000;
    CHECK(predictedDownloadTotalBytes(plain)==3000); // downloadedBytes exceeds expected
    // HLS, 0 completed segments -> expectedSize.
    DownloadItem hls0; hls0.state=DownloadState::Downloading;
    hls0.hlsStorage=true; hls0.hlsSegmentCount=10;
    hls0.expectedSize=50000; hls0.downloadedBytes=0;
    hls0.hlsCompletedSegments=0;
    CHECK(predictedDownloadTotalBytes(hls0)==50000);
    // HLS, N completed segments -> measured avg * segmentCount, never below downloadedBytes.
    DownloadItem hls; hls.state=DownloadState::Downloading;
    hls.hlsStorage=true; hls.hlsSegmentCount=10;
    hls.expectedSize=100000; // preflight estimate, should not be used for prediction
    hls.downloadedBytes=5000; // total bytes on disk
    hls.hlsCompletedSegments=3; // 3 segments fully written
    hls.hlsCurrentSegmentBytes=0; hls.hlsCurrentSegmentSize=0;
    // completedBytes = 5000 - 0 = 5000, avg = 5000/3 = 1666, predicted = 1666*10 = 16666
    const std::uint64_t pred3 = predictedDownloadTotalBytes(hls);
    CHECK(pred3 == (5000/3)*10);
    CHECK(pred3 >= hls.downloadedBytes); // never below downloadedBytes
    // Degenerate: segmentCount=0, completed=0 -> falls back to expectedSize.
    DownloadItem deg; deg.state=DownloadState::Downloading;
    deg.hlsStorage=true; deg.hlsSegmentCount=0;
    deg.expectedSize=7777; deg.downloadedBytes=100;
    deg.hlsCompletedSegments=0;
    CHECK(predictedDownloadTotalBytes(deg)==7777);
    // HLS with current segment in progress: completedBytes excludes current bytes.
    DownloadItem hlsCur; hlsCur.state=DownloadState::Downloading;
    hlsCur.hlsStorage=true; hlsCur.hlsSegmentCount=5;
    hlsCur.expectedSize=20000;
    hlsCur.downloadedBytes=4000; // 2000 complete + 2000 current
    hlsCur.hlsCompletedSegments=2;
    hlsCur.hlsCurrentSegmentBytes=2000; hlsCur.hlsCurrentSegmentSize=2000;
    // completedBytes = 4000 - 2000 = 2000, avg = 1000, predicted = 5000
    CHECK(predictedDownloadTotalBytes(hlsCur)==(2000/2)*5);
    CHECK(predictedDownloadTotalBytes(hlsCur) >= hlsCur.downloadedBytes);
    // All-zero degenerate: no crash, returns expectedSize.
    DownloadItem empty;
    CHECK(predictedDownloadTotalBytes(empty)==0);
    // HLS, 0 completed segments but bytes already written (everything so far
    // belongs to the in-progress segment): the fallback must never drop
    // below downloadedBytes.
    DownloadItem hlsStall; hlsStall.state=DownloadState::Downloading;
    hlsStall.hlsStorage=true; hlsStall.hlsSegmentCount=8;
    hlsStall.expectedSize=1000; hlsStall.downloadedBytes=5000;
    hlsStall.hlsCompletedSegments=0;
    CHECK(predictedDownloadTotalBytes(hlsStall)==5000);
    // HLS where every written byte belongs to the current segment, so
    // completedBytes==0 and avg==0: the same floor applies.
    DownloadItem hlsCurAll; hlsCurAll.state=DownloadState::Downloading;
    hlsCurAll.hlsStorage=true; hlsCurAll.hlsSegmentCount=8;
    hlsCurAll.expectedSize=1000; hlsCurAll.downloadedBytes=9000;
    hlsCurAll.hlsCompletedSegments=2;
    hlsCurAll.hlsCurrentSegmentBytes=9000; hlsCurAll.hlsCurrentSegmentSize=9000;
    CHECK(predictedDownloadTotalBytes(hlsCurAll)==9000);
    std::printf("[test] predictedDownloadTotalBytes OK\n");
}

static void testSpeedStallGap()
{
    std::printf("[test] speed stall gap threshold\n");
    RecentSpeedSample sample;
    std::uint64_t bps=0;
    // Initial: bytes arrive, rate computed (0 -> nonzero returns true).
    CHECK(!DownloadManager::updateRecentSpeed(sample,100,1000,bps));
    CHECK(DownloadManager::updateRecentSpeed(sample,200,2000,bps));
    CHECK(bps==100); // (200-100)*1000/(2000-1000) = 100 B/s
    // Short gap (3s < STALL_GAP_MS): rate should NOT zero.
    CHECK(!DownloadManager::updateRecentSpeed(sample,200,5000,bps));
    CHECK(bps==100); // still 100, not zeroed
    // Long gap (>5s): rate should zero.
    CHECK(DownloadManager::updateRecentSpeed(sample,200,7500,bps));
    CHECK(bps==0);
    // After resume, rate resumes (0 -> nonzero returns true).
    CHECK(DownloadManager::updateRecentSpeed(sample,300,8000,bps));
    CHECK(bps!=0); // nonzero rate restored
    std::printf("[test] speed stall gap threshold OK\n");
}

static void testHlsSegmentCompletedIncremental()
{
    std::printf("[test] incremental HLS segment accounting\n");
    DownloadItem item; item.hlsStorage=true; item.hlsSegmentCount=4;
    applyHlsSegmentCompleted(item,0,100);
    CHECK(item.hlsCompletedSegments==1); CHECK(item.downloadedBytes==100);
    applyHlsSegmentCompleted(item,1,200);
    CHECK(item.hlsCompletedSegments==2); CHECK(item.downloadedBytes==300);
    // A repeated completion for an already-accounted ordinal (retry path)
    // must not double-count bytes.
    applyHlsSegmentCompleted(item,1,200);
    CHECK(item.hlsCompletedSegments==2); CHECK(item.downloadedBytes==300);
    applyHlsSegmentCompleted(item,0,100);
    CHECK(item.downloadedBytes==300);
    // Completed ordinals never run past the discovered playlist length.
    applyHlsSegmentCompleted(item,9,50);
    CHECK(item.hlsCompletedSegments==4); CHECK(item.downloadedBytes==350);
    // Saturating add: near-overflow stays clamped, never wraps.
    DownloadItem big; big.hlsStorage=true; big.hlsSegmentCount=2;
    big.downloadedBytes=(std::numeric_limits<std::uint64_t>::max)()-10;
    applyHlsSegmentCompleted(big,0,100);
    CHECK(big.downloadedBytes==(std::numeric_limits<std::uint64_t>::max)());
    CHECK(big.hlsCompletedSegments==1);
    // Unknown segment count (undiscovered playlist) still tracks progress.
    DownloadItem unknown; unknown.hlsStorage=true; unknown.hlsSegmentCount=0;
    applyHlsSegmentCompleted(unknown,0,7);
    CHECK(unknown.hlsCompletedSegments==1); CHECK(unknown.downloadedBytes==7);
    std::printf("[test] incremental HLS segment accounting OK\n");
}

static void testSegmentRecoveryMatchesIncremental()
{
    std::printf("[test] crash recovery matches incremental accounting\n");
    const std::string root="/tmp/miyoofin-seginc-"+std::to_string((long long)getpid());
    DownloadStore store(root);
    DownloadItem item; item.itemId="seginc"; item.mediaSourceId="s";
    item.hlsStorage=true; item.hlsSegmentCount=3; item.hlsProfile="test";
    item.state=DownloadState::Downloading;
    CHECK(store.ensureHlsDirectories("scope",item.itemId));
    CHECK(store.saveManifest("scope",item));
    writeFixture(store.segmentPath("scope",item.itemId,0),"01234567",8);
    writeFixture(store.segmentPath("scope",item.itemId,1),"0123456789abcdef",16);
    writeFixture(store.segmentPath("scope",item.itemId,2,true),"partial",7);
    // Kill mid-download: relaunch rebuilds counters from segment files only.
    DownloadItem recovered;
    CHECK(store.loadManifest("scope",item.itemId,recovered));
    CHECK(store.reconcile("scope",recovered));
    CHECK(recovered.downloadedBytes==24);
    CHECK(store.firstIncompleteSegment("scope",recovered)==2);
    // The per-segment incremental path must agree exactly with the scan.
    DownloadItem incremental; incremental.itemId="seginc"; incremental.hlsStorage=true; incremental.hlsSegmentCount=3;
    applyHlsSegmentCompleted(incremental,0,8);
    applyHlsSegmentCompleted(incremental,1,16);
    CHECK(incremental.downloadedBytes==recovered.downloadedBytes);
    CHECK(incremental.hlsCompletedSegments==store.firstIncompleteSegment("scope",recovered));
    // downloadedBytes is durable in the manifest (crash between segments keeps
    // the last completed total, never a torn estimate).
    CHECK(store.saveManifest("scope",incremental));
    DownloadItem reloaded;
    CHECK(store.loadManifest("scope",incremental.itemId,reloaded));
    CHECK(reloaded.downloadedBytes==24);
    CHECK(store.removeItem("scope",item.itemId,nullptr));
    std::printf("[test] crash recovery matches incremental accounting OK\n");
}

static void testFreeSpaceCachePolicy()
{
    std::printf("[test] free-space cache policy\n");
    CHECK(kFreeSpaceCacheTtlMs>=1000 && kFreeSpaceCacheTtlMs<=10000);
    CHECK(freeSpaceCacheExpired(5000,0)); // no sample yet: refresh
    CHECK(freeSpaceCacheExpired(100,5000)); // clock regressed: refresh
    CHECK(freeSpaceCacheExpired(3000,0));
    CHECK(!freeSpaceCacheExpired(3999,1000)); // 2999ms < 3000ms TTL: reuse
    CHECK(freeSpaceCacheExpired(4000,1000)); // exactly TTL: refresh
    CHECK(freeSpaceCacheExpired(4001,1000));
    CHECK(freeSpaceCacheExpired(10000,1000,0)); // zero TTL: always refresh
    // The cached value is substituted ONLY on probe failure.  A successful
    // zero (genuinely full card) is authoritative: it must be cached and
    // returned as zero so makePlan() still rejects with "Not enough space".
    CHECK(selectCachedFreeBytes(8000,7000,true)==7000);
    CHECK(selectCachedFreeBytes(8000,0,true)==0); // successful 0: full card, not a failure
    CHECK(selectCachedFreeBytes(8000,1234,false)==8000); // failed probe: keep stale value
    CHECK(selectCachedFreeBytes(8000,0,false)==8000); // failed probe reads back as 0: keep stale value
    CHECK(selectCachedFreeBytes(0,0,false)==0); // first sample, probe failed: report as-is
    CHECK(selectCachedFreeBytes(0,1234,true)==1234);
    CHECK(selectCachedFreeBytes(0,0,true)==0); // first sample, genuinely full: report 0
    std::printf("[test] free-space cache policy OK\n");
}

static void testTlsCaBundleCache()
{
    std::printf("[test] TLS CA bundle cache\n");
    // Setup failures must abort before any file is created: CHECK does not
    // stop the test, so continuing after a failed mkdtemp/chdir would run
    // the body in the repo root and truncate the real tracked cacert.pem.
    char cwd[4096];
    if (::getcwd(cwd,sizeof(cwd))==nullptr) { CHECK(false); return; }
    char tmpl[]="/tmp/miyoofin-tls-XXXXXX";
    if (::mkdtemp(tmpl)==nullptr) { CHECK(false); return; }
    if (::chdir(tmpl)!=0) { CHECK(false); return; }
    const std::string bundlePath=std::string(tmpl)+"/cacert.pem";
    tls_detail::resetCaBundleCacheForTest();
    CHECK(!tlsCaBundleReady()); // genuinely missing: still a failure
    std::string error;
    CURL *curl=curl_easy_init(); CHECK(curl!=nullptr);
    if (curl==nullptr) { CHECK(::chdir(cwd)==0); return; }
    CHECK(!configureTls(curl,"https://example.invalid",&error));
    CHECK(!error.empty());
    CHECK(configureTls(curl,"http://192.168.1.2:8096")); // LAN HTTP needs no CA
    // Bundle appears, but the cached verdict is reused without re-probing.
    FILE *bundle=std::fopen(bundlePath.c_str(),"wb");
    CHECK(bundle!=nullptr);
    if(bundle){std::fwrite("x",1,1,bundle);std::fclose(bundle);}
    CHECK(!tlsCaBundleReady());
    CHECK(!configureTls(curl,"https://example.invalid",&error));
    // After a reset the new file is picked up and handles configure fully.
    tls_detail::resetCaBundleCacheForTest();
    CHECK(tlsCaBundleReady());
    CHECK(configureTls(curl,"https://example.invalid",&error));
    // ...and a later removal does not flip the cached verdict mid-run.
    CHECK(std::remove(bundlePath.c_str())==0);
    CHECK(tlsCaBundleReady());
    curl_easy_cleanup(curl);
    tls_detail::resetCaBundleCacheForTest();
    CHECK(::chdir(cwd)==0);
    CHECK(::rmdir(tmpl)==0);
    std::printf("[test] TLS CA bundle cache OK\n");
}

static void testTransferSourceIdentityGuard()
{
    std::printf("[test] transfer source-identity guard\n");
    // Behavioural: the guard decision itself.  A live item the reconciler
    // reset to a new source must read as changed against the old transfer's
    // captured identity, in either field; an unchanged item must not.
    DownloadItem live; live.itemId="x"; live.mediaSourceId="new"; live.sourceEtag="etag-new";
    live.state=DownloadState::Queued; live.downloadedBytes=0;
    CHECK(transferSourceChanged(live,"old","etag-old")); // both changed
    CHECK(transferSourceChanged(live,"new","etag-old")); // etag changed
    CHECK(transferSourceChanged(live,"old","etag-new")); // id changed
    CHECK(!transferSourceChanged(live,"new","etag-new")); // same source: proceed
    DownloadItem empty; empty.itemId="y";
    CHECK(!transferSourceChanged(empty,"","")); // both empty: same
    CHECK(transferSourceChanged(empty,"s",""));
    // Behavioural: why the guard matters.  Stale progress from the old source
    // applied to the reset live item would force Queued->Downloading and copy
    // old-source counters under the NEW identity (mislabelled completion).
    DownloadItem stale; stale.itemId="x"; stale.mediaSourceId="old"; stale.sourceEtag="etag-old";
    stale.state=DownloadState::Downloading; stale.hlsStorage=true; stale.hlsSegmentCount=4;
    stale.hlsCompletedSegments=3; stale.downloadedBytes=300; stale.hlsActivePercent=75;
    CHECK(transferSourceChanged(live,stale.mediaSourceId,stale.sourceEtag)); // abort, do not publish
    DownloadItem unguarded=live;
    publishTransferProgress(unguarded,stale);
    CHECK(unguarded.state==DownloadState::Downloading); // the hazard the guard prevents
    CHECK(unguarded.downloadedBytes==300);
    CHECK(unguarded.mediaSourceId=="new"); // counters now disagree with the preserved identity
    // Guarded path: the live item keeps the reconciler's reset untouched.
    CHECK(live.state==DownloadState::Queued);
    CHECK(live.downloadedBytes==0);
    CHECK(live.mediaSourceId=="new"&&live.sourceEtag=="etag-new");
    std::printf("[test] transfer source-identity guard OK\n");
}

static void testTransferFinishDecision()
{
    std::printf("[test] transfer completion decision\n");
    using TF=TransferFinish;
    // Behavioural: delete requested during the final segment wins over every
    // other outcome (B) — removal, never Complete.
    CHECK(decideTransferFinish(true,false,false)==TF::Removed);
    CHECK(decideTransferFinish(true,false,true)==TF::Removed);
    CHECK(decideTransferFinish(true,true,false)==TF::Removed);
    CHECK(decideTransferFinish(true,true,true)==TF::Removed);
    // Behavioural: a source change aborts the stale transfer (A).
    CHECK(decideTransferFinish(false,true,false)==TF::StaleAborted);
    CHECK(decideTransferFinish(false,true,true)==TF::StaleAborted);
    // Behavioural: validation failure retries as a recoverable failure (C),
    // never Complete and never stranded in Downloading.
    CHECK(decideTransferFinish(false,false,false)==TF::RetryFailed);
    CHECK(decideTransferFinish(false,false,true)==TF::Complete);
    // Behavioural: the validation-failure transition itself.
    DownloadItem live; live.itemId="v"; live.state=DownloadState::Downloading;
    live.recentBytesPerSec=999; live.lastError.clear();
    applyTransferValidationFailure(live,"missing or empty segment");
    CHECK(live.state==DownloadState::Failed);
    CHECK(live.state!=DownloadState::Downloading); // worker() only dequeues Queued
    CHECK(live.recentBytesPerSec==0);
    CHECK(!live.lastError.empty());
    CHECK(live.lastError=="missing or empty segment");
    DownloadItem noDetail; noDetail.itemId="w"; noDetail.state=DownloadState::Downloading;
    applyTransferValidationFailure(noDetail,"");
    CHECK(noDetail.state==DownloadState::Failed);
    CHECK(!noDetail.lastError.empty());
    // Interrupts that landed during final validation survive, as on the
    // segment-failure path.
    live.state=DownloadState::Paused; live.lastError.clear();
    applyTransferValidationFailure(live,"x");
    CHECK(live.state==DownloadState::Paused);
    live.state=DownloadState::PausedForPlayback;
    applyTransferValidationFailure(live,"x");
    CHECK(live.state==DownloadState::PausedForPlayback);
    std::printf("[test] transfer completion decision OK\n");
}

static void testWorkerPersistMatchesLiveState()
{
    std::printf("[test] worker persist matches live state\n");
    // Deterministic final-state test (replaces the old transient poll, which
    // succeeded the moment it observed Paused even when a later stale write
    // reverted it).  With playback held the worker runs only id-set persist
    // passes and never dequeues.  The bulk enqueue stretches the worker's
    // pass across milliseconds; pausing/erasing the tail item once the pass
    // is in flight lands the racing transition mid-pass.  The assertions
    // below read the FINAL on-disk state after settling, so a stale
    // whole-library write landing last (the old unlocked design) fails.
    for(int round=0;round<3;++round){
        const std::string base="/tmp/miyoofin-persist-race-"+std::to_string((long long)getpid())+"-"+std::to_string(round);
        // pause() must survive on disk.
        {
            DownloadManager manager({},base+"-p");
            manager.setPlaybackActive(true);
            DownloadStore store(base+"-p");
            std::vector<DownloadItem> items;
            for(int k=0;k<64;++k)items.push_back(restartFixture("bulk-"+std::to_string(k)));
            items.push_back(restartFixture("race-item"));
            manager.enqueue(items);
            // Wait until the pass is in flight (head persisted, tail not yet).
            for(unsigned n=0;n<2000;++n){DownloadItem m;if(store.loadManifest("anonymous","bulk-0",m,nullptr))break;usleep(1000);}
            manager.pause("race-item");
            usleep(200000); // settle: any in-flight worker write has landed
            DownloadItem manifest;CHECK(store.loadManifest("anonymous","race-item",manifest,nullptr));
            CHECK(manifest.state==DownloadState::Paused);
            bool livePaused=false;
            for(const auto&i:manager.snapshot().items)if(i.itemId=="race-item")livePaused=(i.state==DownloadState::Paused);
            CHECK(livePaused);
        }
        // erase() must not resurrect on disk (no ghost manifest or index entry).
        {
            DownloadManager manager({},base+"-e");
            manager.setPlaybackActive(true);
            DownloadStore store(base+"-e");
            std::vector<DownloadItem> items;
            for(int k=0;k<64;++k)items.push_back(restartFixture("bulk-"+std::to_string(k)));
            items.push_back(restartFixture("race-item"));
            manager.enqueue(items);
            for(unsigned n=0;n<2000;++n){DownloadItem m;if(store.loadManifest("anonymous","bulk-0",m,nullptr))break;usleep(1000);}
            CHECK(manager.erase("race-item"));
            usleep(200000);
            DownloadItem ghost;CHECK(!store.loadManifest("anonymous","race-item",ghost,nullptr));
            std::vector<DownloadItem> idx;CHECK(store.loadIndex("anonymous",idx,nullptr));
            for(const auto&i:idx)CHECK(i.itemId!="race-item");
        }
    }
    std::printf("[test] worker persist matches live state OK\n");
}

static void testStalePlaylistDiscoverySuppressed()
{
    std::printf("[test] stale playlist-discovery publish suppressed\n");
    // Behavioural: the guard decision for the discovery window.  The
    // reconciler reset the live item to Queued under a NEW source while the
    // old transfer was still fetching the playlist.
    DownloadItem live; live.itemId="d"; live.mediaSourceId="new"; live.sourceEtag="etag-new";
    live.state=DownloadState::Queued; live.downloadedBytes=0; live.hlsSegmentCount=0;
    CHECK(transferSourceChanged(live,"old","etag-old")); // stale: suppress failure + discovery publish
    DownloadItem same; same.itemId="d"; same.mediaSourceId="old"; same.sourceEtag="etag-old";
    same.state=DownloadState::Downloading;
    CHECK(!transferSourceChanged(same,"old","etag-old")); // unchanged: discovery proceeds
    // The guard must not swallow pause handling: a paused same-source item
    // still reads as unchanged, so the existing Paused check after it stays
    // responsible for preserving the interrupt.
    DownloadItem paused=same; paused.state=DownloadState::Paused;
    CHECK(!transferSourceChanged(paused,"old","etag-old"));
    paused.state=DownloadState::PausedForPlayback;
    CHECK(!transferSourceChanged(paused,"old","etag-old"));
    std::printf("[test] stale playlist-discovery publish suppressed OK\n");
}

// Abandoned-delete-flag consumption, stale-segment removal under lock, and
// the transfer I/O persist shapes are only observable through a live
// transfer racing the reconciler/erase paths over real network; they cannot
// be reproduced deterministically on the host.  The decision helpers those
// paths call (decideTransferFinish delete-wins, applyTransferValidationFailure,
// publishTransferProgress, applyReconciledSource) are covered behaviourally in
// the neighbouring tests, and the final on-disk persist invariants are covered
// by testWorkerPersistMatchesLiveState.  The former call-site source pins are
// removed rather than pretending a unit test covers the race.

static std::string manifestIdentity(DownloadStore&store,const std::string&scope,const std::string&id)
{
    // Atomic rename always mints a new inode, so an untouched manifest keeps
    // its identity even on filesystems with coarse mtime granularity.
    struct stat st{};
    if(::stat(store.manifestPath(scope,id).c_str(),&st))return {};
    return std::to_string((unsigned long long)st.st_ino)+":"+std::to_string((long long)st.st_mtime)+":"+std::to_string((long long)st.st_size);
}

static void testPersistTouchesOnlyAffectedItem()
{
    std::printf("[test] persist touches only the affected item\n");
    const std::string root="/tmp/miyoofin-peritem-"+std::to_string((long long)getpid());
    DownloadManager manager({},root);
    manager.setPlaybackActive(true); // worker runs persist passes only, never dequeues
    DownloadStore store(root);
    manager.enqueue({restartFixture("item-a"),restartFixture("item-b"),restartFixture("item-c")});
    // Quiesce: the worker's id-set pass finished (index lists all three).
    for(unsigned n=0;n<400;++n){std::vector<DownloadItem> idx;if(store.loadIndex("anonymous",idx,nullptr)&&idx.size()==3)break;usleep(5000);}
    const std::string bBefore=manifestIdentity(store,"anonymous","item-b"), cBefore=manifestIdentity(store,"anonymous","item-c");
    CHECK(!bBefore.empty()&&!cBefore.empty());
    const std::string aContentBefore=readFixture(store.manifestPath("anonymous","item-a"));
    const std::string bContentBefore=readFixture(store.manifestPath("anonymous","item-b"));
    // pause() rewrites only item-a's manifest.
    manager.pause("item-a");
    DownloadItem a;CHECK(store.loadManifest("anonymous","item-a",a,nullptr));CHECK(a.state==DownloadState::Paused);
    CHECK(readFixture(store.manifestPath("anonymous","item-a"))!=aContentBefore);
    CHECK(manifestIdentity(store,"anonymous","item-b")==bBefore);
    CHECK(manifestIdentity(store,"anonymous","item-c")==cBefore);
    CHECK(readFixture(store.manifestPath("anonymous","item-b"))==bContentBefore);
    // erase() removes only item-c (plus the index rewrite); the survivors'
    // manifests are untouched.
    const std::string aBefore=manifestIdentity(store,"anonymous","item-a"), bBefore2=manifestIdentity(store,"anonymous","item-b");
    CHECK(manager.erase("item-c"));
    CHECK(::access(store.manifestPath("anonymous","item-c").c_str(),F_OK)!=0);
    std::vector<DownloadItem> idx;CHECK(store.loadIndex("anonymous",idx,nullptr));CHECK(idx.size()==2);
    CHECK(manifestIdentity(store,"anonymous","item-a")==aBefore);
    CHECK(manifestIdentity(store,"anonymous","item-b")==bBefore2);
    std::printf("[test] persist touches only the affected item OK\n");
}

static void testPersistPendingCrashDurabilityAndTmpSweep()
{
    std::printf("[test] pending-index durability and tmp sweep\n");
    // Behavioural: pausing an item the worker's id-set pass has not indexed
    // yet still lands it in the index (final-state either way: the pass
    // converging first changes nothing, so no timing gate is needed).
    {
        const std::string root="/tmp/miyoofin-pending-idx-"+std::to_string((long long)getpid());
        DownloadManager manager({},root);
        manager.setPlaybackActive(true); // worker runs persist passes only, never dequeues
        DownloadStore store(root);
        manager.enqueue(restartFixture("pending-one"));
        manager.pause("pending-one");
        usleep(200000); // settle: any in-flight worker write has landed
        std::vector<DownloadItem> idx;CHECK(store.loadIndex("anonymous",idx,nullptr));
        bool listed=false;for(const auto&i:idx)if(i.itemId=="pending-one")listed=true;
        CHECK(listed);
        DownloadItem m;CHECK(store.loadManifest("anonymous","pending-one",m,nullptr));
        CHECK(m.state==DownloadState::Paused);
    }
    // Behavioural: a stale unique temp never pins the item directory.
    {
        const std::string root="/tmp/miyoofin-tmp-sweep-"+std::to_string((long long)getpid());
        DownloadStore store(root);
        CHECK(store.saveManifest("anonymous",restartFixture("tmp-item"),nullptr));
        const std::string stale=store.itemPath("anonymous","tmp-item")+"/manifest.v2.tmp.12345.678";
        {FILE*f=std::fopen(stale.c_str(),"wb");CHECK(f);CHECK(std::fputs("stale",f)>=0);CHECK(std::fclose(f)==0);}
        CHECK(store.removeItem("anonymous","tmp-item",nullptr));
        CHECK(::access(store.itemPath("anonymous","tmp-item").c_str(),F_OK)!=0);
    }
    // Behavioural: legacy p+".tmp" leftovers are swept too (they used to pin
    // removeItem's final rmdir and survive rebuildIndex).
    {
        const std::string root="/tmp/miyoofin-tmp-legacy-"+std::to_string((long long)getpid());
        DownloadStore store(root);
        CHECK(store.saveManifest("anonymous",restartFixture("legacy-item"),nullptr));
        const std::string legacy=store.itemPath("anonymous","legacy-item")+"/manifest.v2.tmp";
        {FILE*f=std::fopen(legacy.c_str(),"wb");CHECK(f);CHECK(std::fputs("stale",f)>=0);CHECK(std::fclose(f)==0);}
        CHECK(store.removeItem("anonymous","legacy-item",nullptr));
        CHECK(::access(store.itemPath("anonymous","legacy-item").c_str(),F_OK)!=0);
        // rebuildIndex sweeps the scope dir: a legacy index.v1.tmp must not
        // survive it.
        CHECK(store.saveManifest("anonymous",restartFixture("kept"),nullptr));
        const std::string scopeTmp=store.scopePath("anonymous")+"/index.v1.tmp";
        {FILE*f=std::fopen(scopeTmp.c_str(),"wb");CHECK(f);CHECK(std::fputs("stale",f)>=0);CHECK(std::fclose(f)==0);}
        std::vector<DownloadItem> rebuilt;CHECK(store.rebuildIndex("anonymous",rebuilt,nullptr));
        CHECK(::access(scopeTmp.c_str(),F_OK)!=0);
        bool kept=false;for(const auto&i:rebuilt)if(i.itemId=="kept")kept=true;
        CHECK(kept);
    }
    std::printf("[test] pending-index durability and tmp sweep OK\n");}

static void testIndexNeverNamesMissingManifest()
{
    std::printf("[test] index never names a missing manifest\n");
    // Regression pin for the crash-consistency inversion: the index used to
    // be written from raw m_items, so an id enqueued after the pass's
    // pending-swap (or a still-pending id named by pause/resume) could reach
    // the index before its manifest existed.  A crash in that window made
    // loadIndex fail for ALL readers.  The invariant is behavioural: every
    // id in the on-disk index has a readable manifest, and loadIndex itself
    // succeeds — after a bulk enqueue racing the pass and after pause/resume
    // racing it.
    const std::string root="/tmp/miyoofin-index-invariant-"+std::to_string((long long)getpid());
    DownloadManager manager({},root);
    manager.setPlaybackActive(true); // worker runs persist passes only, never dequeues
    DownloadStore store(root);
    auto indexIds=[](const std::string&raw){
        std::vector<std::string> ids;size_t p=raw.find('\n');
        if(p==std::string::npos)return ids;
        p+=1;
        while(p<raw.size()){size_t z=raw.find('\n',p);std::string id=raw.substr(p,z-p);if(!id.empty())ids.push_back(id);if(z==std::string::npos)break;p=z+1;}
        return ids;};
    auto checkInvariant=[&](const char*stage){
        const std::string raw=readFixture(store.scopePath("anonymous")+"/index.v1");
        CHECK(!raw.empty());
        if(raw.empty())return std::vector<std::string>{};
        CHECK(raw.rfind("MFDI=1\n",0)==0);
        const std::vector<std::string> ids=indexIds(raw);
        CHECK(!ids.empty());
        for(const auto&id:ids){DownloadItem m;CHECK(store.loadManifest("anonymous",id,m,nullptr));}
        std::vector<DownloadItem> idx;CHECK(store.loadIndex("anonymous",idx,nullptr));
        (void)stage;return ids;};
    std::vector<DownloadItem> items;
    for(int k=0;k<64;++k)items.push_back(restartFixture("bulk-"+std::to_string(k)));
    items.push_back(restartFixture("race-item"));
    manager.enqueue(items);
    // Pause/resume land mid-pass (head persisted, tail not yet): the racing
    // transitions used to be exactly the window that indexed manifestless
    // ids or orphaned durable manifests.
    for(unsigned n=0;n<2000;++n){DownloadItem m;if(store.loadManifest("anonymous","bulk-0",m,nullptr))break;usleep(1000);}
    manager.pause("race-item");
    manager.resume("bulk-1");
    usleep(300000); // settle: any in-flight worker write has landed
    const std::vector<std::string> ids=checkInvariant("bulk race");
    bool raceListed=false, bulkListed=false;
    for(const auto&id:ids){if(id=="race-item")raceListed=true;if(id=="bulk-1")bulkListed=true;}
    CHECK(raceListed);CHECK(bulkListed);
    DownloadItem race;CHECK(store.loadManifest("anonymous","race-item",race,nullptr));
    CHECK(race.state==DownloadState::Paused);
    // A late enqueue followed by an immediate pause/resume must keep the
    // same invariant: the paused id is durable AND listed, never one without
    // the other.
    manager.enqueue(restartFixture("late-one"));
    manager.pause("late-one");
    usleep(300000);
    const std::vector<std::string> ids2=checkInvariant("late pause");
    bool lateListed=false;for(const auto&id:ids2)if(id=="late-one")lateListed=true;
    CHECK(lateListed);
    DownloadItem late;CHECK(store.loadManifest("anonymous","late-one",late,nullptr));
    CHECK(late.state==DownloadState::Paused);
    std::printf("[test] index never names a missing manifest OK\n");
}

static void testPlaybackToggleTouchesOnlyAffectedItems()
{
    std::printf("[test] playback toggle touches only affected items\n");
    const std::string root="/tmp/miyoofin-peritem-pb-"+std::to_string((long long)getpid());
    DownloadManager manager({},root);
    DownloadStore store(root);
    // One item reaches Downloading without network: the invalid session makes
    // transfer() return immediately, leaving the dequeued item Downloading.
    manager.enqueue(restartFixture("dl-x"));
    for(unsigned n=0;n<400;++n){bool downloading=false;for(const auto&i:manager.snapshot().items)if(i.itemId=="dl-x"&&i.state==DownloadState::Downloading)downloading=true;if(downloading)break;usleep(5000);}
    manager.setPlaybackActive(true); // interrupts only dl-x
    DownloadItem x;CHECK(store.loadManifest("anonymous","dl-x",x,nullptr));CHECK(x.state==DownloadState::PausedForPlayback);
    // A later enqueue stays Queued (playback held); pausing it makes it
    // undequeueable, so the release path below can never touch it — the
    // "unrelated item untouched" assertions are then timing-independent
    // (previously they raced the worker dequeuing both Queued items the
    // moment playback was released).
    manager.enqueue(restartFixture("idle-y"));
    for(unsigned n=0;n<400;++n){std::vector<DownloadItem> items;if(store.loadIndex("anonymous",items,nullptr)&&items.size()==2)break;usleep(5000);}
    manager.pause("idle-y");
    DownloadItem yPaused;CHECK(store.loadManifest("anonymous","idle-y",yPaused,nullptr));CHECK(yPaused.state==DownloadState::Paused);
    const std::string yBefore=manifestIdentity(store,"anonymous","idle-y");
    CHECK(!yBefore.empty());
    const std::string yContentBefore=readFixture(store.manifestPath("anonymous","idle-y"));
    manager.setPlaybackActive(false);
    // dl-x leaves PausedForPlayback for Queued and is dequeued to Downloading
    // (invalid session: transfer() returns immediately, so the state sticks);
    // idle-y stays Paused in memory and its manifest is never rewritten by
    // the release path or the worker.
    for(unsigned n=0;n<400;++n){bool done=false;for(const auto&i:manager.snapshot().items)if(i.itemId=="dl-x"&&i.state==DownloadState::Downloading)done=true;if(done)break;usleep(5000);}
    bool xDownloading=false,yPausedLive=false;
    for(const auto&i:manager.snapshot().items){if(i.itemId=="dl-x")xDownloading=(i.state==DownloadState::Downloading);if(i.itemId=="idle-y")yPausedLive=(i.state==DownloadState::Paused);}
    CHECK(xDownloading);
    CHECK(yPausedLive);
    DownloadItem back;CHECK(store.loadManifest("anonymous","dl-x",back,nullptr));CHECK(back.state!=DownloadState::PausedForPlayback);
    CHECK(manifestIdentity(store,"anonymous","idle-y")==yBefore);
    CHECK(readFixture(store.manifestPath("anonymous","idle-y"))==yContentBefore);
    std::printf("[test] playback toggle touches only affected items OK\n");
}
