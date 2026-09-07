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
    DownloadManager d({}, "output/test/download-plan-fixture-"+std::to_string((long long)getpid()));
    d.setPlaybackActive(true); // preserve fixture queue state; no transfer worker races
    // enqueue is called by the SDL confirmation handler.  With the worker
    // paused, no manifest or item directory may exist until that worker runs.
    DownloadItem deferred=planItem("deferred",100);
    d.enqueue(std::vector<DownloadItem>{deferred,planItem("deferred-two",100)});
    DownloadStore deferredStore("output/test/download-plan-fixture-"+std::to_string((long long)getpid()));
    CHECK(::access(deferredStore.manifestPath(d.scope(),"deferred").c_str(),F_OK)!=0);
    CHECK(::access(deferredStore.itemPath(d.scope(),"deferred").c_str(),F_OK)!=0);
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
    CHECK(DownloadManager::updateRecentSpeed(speed,600,3001,bytesPerSec));
    CHECK(bytesPerSec==0);
    // A new download (a lower byte count) resets rather than carrying speed.
    CHECK(!DownloadManager::updateRecentSpeed(speed,0,3100,bytesPerSec));
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
