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
            manager.flushPersistence();
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
            manager.flushPersistence();
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
    manager.flushPersistence(); // the storage thread writes it; wait for the disk
    DownloadItem a;CHECK(store.loadManifest("anonymous","item-a",a,nullptr));CHECK(a.state==DownloadState::Paused);
    CHECK(readFixture(store.manifestPath("anonymous","item-a"))!=aContentBefore);
    CHECK(manifestIdentity(store,"anonymous","item-b")==bBefore);
    CHECK(manifestIdentity(store,"anonymous","item-c")==cBefore);
    CHECK(readFixture(store.manifestPath("anonymous","item-b"))==bContentBefore);
    // erase() removes only item-c (plus the index rewrite); the survivors'
    // manifests are untouched.
    const std::string aBefore=manifestIdentity(store,"anonymous","item-a"), bBefore2=manifestIdentity(store,"anonymous","item-b");
    CHECK(manager.erase("item-c"));
    manager.flushPersistence();
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
        manager.pause("pending-one"); manager.flushPersistence();
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
    manager.pause("race-item"); manager.flushPersistence();
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
    manager.pause("late-one"); manager.flushPersistence();
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
    manager.flushPersistence();
    DownloadItem x;CHECK(store.loadManifest("anonymous","dl-x",x,nullptr));CHECK(x.state==DownloadState::PausedForPlayback);
    // A later enqueue stays Queued (playback held); pausing it makes it
    // undequeueable, so the release path below can never touch it — the
    // "unrelated item untouched" assertions are then timing-independent
    // (previously they raced the worker dequeuing both Queued items the
    // moment playback was released).
    manager.enqueue(restartFixture("idle-y"));
    for(unsigned n=0;n<400;++n){std::vector<DownloadItem> items;if(store.loadIndex("anonymous",items,nullptr)&&items.size()==2)break;usleep(5000);}
    manager.pause("idle-y");
    manager.flushPersistence();
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
    manager.flushPersistence();
    DownloadItem back;CHECK(store.loadManifest("anonymous","dl-x",back,nullptr));CHECK(back.state!=DownloadState::PausedForPlayback);
    CHECK(manifestIdentity(store,"anonymous","idle-y")==yBefore);
    CHECK(readFixture(store.manifestPath("anonymous","idle-y"))==yContentBefore);
    std::printf("[test] playback toggle touches only affected items OK\n");
}
