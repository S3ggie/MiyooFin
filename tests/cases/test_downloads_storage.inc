static void testStorageWriteFailureDoesNotLeakDescriptors()
{
    std::printf("[test] failed metadata/journal writes do not leak descriptors\n");
    const std::string root="/tmp/miyoofin-fdleak-"+std::to_string((long long)getpid());
    DownloadStore store(root);
    DownloadItem item; item.itemId="leak"; item.mediaSourceId="s"; item.hlsStorage=true; item.hlsSegmentCount=9;
    item.hlsProfile=std::string(4096,'p'); // large enough that the write cannot fit under the limit
    CHECK(store.ensureHlsDirectories("scope",item.itemId));
    std::vector<OfflinePlaybackEntry> entries(40);
    for(size_t i=0;i<entries.size();i++){entries[i].itemId="item-"+std::to_string(i);entries[i].itemType="movie";}
    const std::string journal=root+"/journal.txt";
    signal(SIGXFSZ,SIG_IGN);
    struct rlimit saved{}; CHECK(getrlimit(RLIMIT_FSIZE,&saved)==0);
    struct rlimit tight=saved; tight.rlim_cur=64; CHECK(setrlimit(RLIMIT_FSIZE,&tight)==0);
    const int before=openFdCount();
    int failures=0;
    for(int i=0;i<32;i++){
        if(!store.saveManifest("scope",item))failures++;
        if(!OfflinePlaybackJournal::save(journal,entries,nullptr))failures++;
    }
    const int after=openFdCount();
    CHECK(setrlimit(RLIMIT_FSIZE,&saved)==0);
    CHECK(failures==64);   // every write really failed ...
    CHECK(after==before);  // ... and none of them left a descriptor behind
    CHECK(store.removeItem("scope",item.itemId,nullptr));
    std::printf("[test] failed metadata/journal writes do not leak descriptors OK\n");
}

static void testPauseResumeDoNotWaitForTheDisk()
{
    std::printf("[test] pause/resume do not wait for the disk\n");
    const std::string root="/tmp/miyoofin-asyncpause-"+std::to_string((long long)getpid());
    DownloadManager manager({},root);
    manager.setPlaybackActive(true); // keep the worker from dequeuing
    DownloadStore store(root);
    manager.enqueue(restartFixture("slow-a"));
    for(unsigned n=0;n<400;++n){std::vector<DownloadItem> idx;if(store.loadIndex("anonymous",idx,nullptr)&&idx.size()==1)break;usleep(5000);}
    manager.flushPersistence();

    DiskStall stall(manager);
    const auto start=std::chrono::steady_clock::now();
    manager.pause("slow-a");
    CHECK(stall.waitReached());
    // The write is stuck on the "disk". Pause again, resume, and read the snapshot: none may wait.
    manager.resume("slow-a");
    manager.pause("slow-a");
    DownloadState live=DownloadState::Queued;for(const auto&i:manager.snapshot().items)if(i.itemId=="slow-a")live=i.state;
    CHECK(live==DownloadState::Paused);
    CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(2));
    stall.release();
    manager.flushPersistence();
    DownloadItem onDisk;CHECK(store.loadManifest("anonymous","slow-a",onDisk,nullptr));
    CHECK(onDisk.state==DownloadState::Paused); // the newest state won, not the one the stalled write held
    std::printf("[test] pause/resume do not wait for the disk OK\n");
}

static void testErasureAndRedownloadDoNotWaitForTheDisk()
{
    std::printf("[test] erase/redownload/snapshot/hasComplete do not wait for the disk\n");
    const std::string root="/tmp/miyoofin-asyncerase-"+std::to_string((long long)getpid());
    {
        DownloadStore store(root);
        DownloadItem done=restartFixture("done",DownloadState::Complete);
        DownloadItem gone=restartFixture("gone",DownloadState::Paused);
        DownloadItem readd=restartFixture("readd",DownloadState::Paused);
        DownloadItem upd=restartFixture("upd",DownloadState::UpdateAvailable);
        upd.availableMediaSourceId="ms2"; upd.updateAvailable=true;
        DownloadItem poke=restartFixture("poke",DownloadState::Paused);
        for(const DownloadItem&i:{done,gone,readd,upd,poke})CHECK(seedItem(store,"anonymous",i,i.itemId!="poke"));
    }
    DownloadStore store(root);
    const std::string goneSeg=store.segmentPath("anonymous","gone",0), readdSeg=store.segmentPath("anonymous","readd",0),
                      updSeg=store.segmentPath("anonymous","upd",0);
    DownloadManager manager({},root);
    manager.setPlaybackActive(true);
    manager.flushPersistence();
    CHECK(manager.snapshot().items.size()==5);

    DiskStall stall(manager);
    manager.pause("poke");          // wakes the storage thread, which now hangs in its first write
    CHECK(stall.waitReached());
    const auto start=std::chrono::steady_clock::now();
    CHECK(manager.erase("gone"));
    CHECK(manager.erase("readd"));
    manager.enqueue(restartFixture("readd"));      // deleted and added back while the disk is stuck
    CHECK(manager.redownload("upd"));
    CHECK(manager.hasComplete("done"));
    CHECK(!manager.hasComplete("gone"));
    bool sawGone=false, updQueued=false, readdQueued=false;
    for(const auto&i:manager.snapshot().items){
        sawGone=sawGone||i.itemId=="gone";
        if(i.itemId=="upd")updQueued=i.state==DownloadState::Queued&&i.mediaSourceId=="ms2";
        if(i.itemId=="readd")readdQueued=i.state==DownloadState::Queued;
    }
    CHECK(!sawGone); CHECK(updQueued); CHECK(readdQueued);
    CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(2));
    // Nothing has touched the disk yet: the work is queued, not done by the UI caller.
    CHECK(::access(goneSeg.c_str(),F_OK)==0); CHECK(::access(updSeg.c_str(),F_OK)==0);

    stall.release();
    manager.flushPersistence();
    CHECK(::access(goneSeg.c_str(),F_OK)!=0);
    CHECK(::access(store.manifestPath("anonymous","gone").c_str(),F_OK)!=0);
    CHECK(::access(updSeg.c_str(),F_OK)!=0);               // re-download starts from clean
    CHECK(::access(readdSeg.c_str(),F_OK)!=0);             // the old copy did not survive the re-add
    DownloadItem m;
    CHECK(store.loadManifest("anonymous","readd",m,nullptr)); CHECK(m.state==DownloadState::Queued); // the re-add was written after the removal
    CHECK(store.loadManifest("anonymous","upd",m,nullptr)); CHECK(m.state==DownloadState::Queued&&m.mediaSourceId=="ms2");
    std::vector<DownloadItem> idx;CHECK(store.loadIndex("anonymous",idx,nullptr));
    std::set<std::string> ids;for(const auto&i:idx)ids.insert(i.itemId);
    CHECK(ids==(std::set<std::string>{"done","readd","upd","poke"}));
    std::printf("[test] erase/redownload/snapshot/hasComplete do not wait for the disk OK\n");
}

static void testConfigureDoesNotWaitForTheDisk()
{
    std::printf("[test] configure does not wait for the disk\n");
    const std::string root="/tmp/miyoofin-asyncconfigure-"+std::to_string((long long)getpid());
    {
        DownloadStore store(root);
        CHECK(seedItem(store,"anonymous",restartFixture("old-a",DownloadState::Paused),false));
        CHECK(seedItem(store,"anonymous",restartFixture("old-b",DownloadState::Paused),false));
    }
    DownloadStore store(root);
    DownloadManager manager({},root);
    manager.setPlaybackActive(true);
    manager.flushPersistence();
    DiskStall stall(manager);
    manager.resume("old-a");
    CHECK(stall.waitReached());
    Session other; other.serverUrl="http://127.0.0.1:1"; other.userId="u2"; other.accessToken="t"; other.deviceId="d";
    const auto start=std::chrono::steady_clock::now();
    manager.configure(other);
    CHECK(manager.snapshot().items.empty());                  // the other account's (empty) library
    manager.enqueue(restartFixture("new-1"));
    CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(2));
    stall.release();
    manager.flushPersistence();
    // The account we left was written out whole (including the change made just before the
    // switch); the new account's item went to its own scope only.
    DownloadItem m;
    CHECK(store.loadManifest("anonymous","old-a",m,nullptr)); CHECK(m.state==DownloadState::Queued);
    CHECK(store.loadManifest("anonymous","old-b",m,nullptr));
    CHECK(!store.loadManifest("anonymous","new-1",m,nullptr));
    const std::string scope=DownloadStore::scopeKey(other.serverUrl,other.userId);
    CHECK(store.loadManifest(scope,"new-1",m,nullptr));
    std::vector<DownloadItem> idx;CHECK(store.loadIndex("anonymous",idx,nullptr));CHECK(idx.size()==2);
    std::vector<DownloadItem> idx2;CHECK(store.loadIndex(scope,idx2,nullptr));CHECK(idx2.size()==1);
    // And switching back reads the first account's library again.
    Session none; manager.configure(none);
    manager.flushPersistence();
    CHECK(manager.snapshot().items.size()==2);
    std::printf("[test] configure does not wait for the disk OK\n");
}

// An erase in one account must still delete that account's files, and must not come back when the
// account is selected again, even though the account was switched away before the storage thread
// reached the removal.
static void testRemovalSurvivesAccountSwitch()
{
    std::printf("[test] queued removal survives an account switch\n");
    const std::string root="/tmp/miyoofin-switchremove-"+std::to_string((long long)getpid());
    {
        DownloadStore store(root);
        CHECK(seedItem(store,"anonymous",restartFixture("victim",DownloadState::Paused),true));
        CHECK(seedItem(store,"anonymous",restartFixture("keeper",DownloadState::Paused),true));
    }
    DownloadStore store(root);
    DownloadManager manager({},root);
    manager.setPlaybackActive(true);
    manager.flushPersistence();
    CHECK(manager.snapshot().items.size()==2);
    const Session other=fixtureAccount("other");
    {
        DiskStall stall(manager);
        manager.pause("keeper");                 // storage thread now stuck in a manifest write
        CHECK(stall.waitReached());
        CHECK(manager.erase("victim"));
        manager.configure(other);                // switched away before the removal ran
        stall.release();
    }
    manager.flushPersistence();
    CHECK(::access(store.itemPath("anonymous","victim").c_str(),F_OK)!=0);
    std::vector<DownloadItem> idx;CHECK(store.loadIndex("anonymous",idx,nullptr));
    CHECK(idx.size()==2||idx.size()==1);
    for(const auto&i:idx)CHECK(i.itemId!="victim");
    DownloadItem m;CHECK(store.loadManifest("anonymous","keeper",m,nullptr));
    manager.configure({});
    manager.flushPersistence();
    std::set<std::string> ids;for(const auto&i:manager.snapshot().items)ids.insert(i.itemId);
    CHECK(ids==(std::set<std::string>{"keeper"}));
    // The id is not left busy: it can be added again and is written.
    manager.enqueue(restartFixture("victim"));
    manager.flushPersistence();
    CHECK(store.loadManifest("anonymous","victim",m,nullptr));
    // Same sequence, but the switch away and back both happen before the disk moves.
    {
        DiskStall stall(manager);
        manager.pause("keeper");
        CHECK(stall.waitReached());
        CHECK(manager.erase("victim"));
        manager.configure(other);
        manager.configure({});
        CHECK(manager.snapshot().items.empty());   // not showing another account's items, nor stale ones
        stall.release();
    }
    manager.flushPersistence();
    ids.clear();for(const auto&i:manager.snapshot().items)ids.insert(i.itemId);
    CHECK(ids==(std::set<std::string>{"keeper"}));
    CHECK(::access(store.itemPath("anonymous","victim").c_str(),F_OK)!=0);
    std::printf("[test] queued removal survives an account switch OK\n");
}

// configure() and the constructor read nothing on the caller: a stalled index/manifest read (or a
// rebuild) leaves callers responsive, and publishes once, merging what was added meanwhile.
static void testAccountLibraryLoadIsAsynchronous()
{
    std::printf("[test] account library load is asynchronous\n");
    const std::string root="/tmp/miyoofin-asyncload-"+std::to_string((long long)getpid());
    const Session other=fixtureAccount("other");
    const std::string otherScope=DownloadStore::scopeKey(other.serverUrl,other.userId);
    {
        DownloadStore store(root);
        CHECK(seedItem(store,otherScope,restartFixture("o1",DownloadState::Paused),true));
        CHECK(seedItem(store,otherScope,restartFixture("o2",DownloadState::Paused),true));
        CHECK(seedItem(store,"anonymous",restartFixture("a1",DownloadState::Paused),true));
    }
    DownloadStore store(root);
    DownloadManager manager({},root);
    manager.setPlaybackActive(true);
    manager.flushPersistence();
    for(int mode=0;mode<2;++mode){
        if(mode==1){ // a damaged index: the library is rebuilt from the manifests (also on the storage thread)
            std::remove((store.scopePath(otherScope)+"/index.v1").c_str());
        }
        manager.configure({});
        manager.flushPersistence();
        DiskStall stall(manager);
        manager.pause("a1");                              // storage thread stuck in a write...
        CHECK(stall.waitReached());
        const auto start=std::chrono::steady_clock::now();
        manager.configure(other);                          // ...and the new library's read queued behind it
        CHECK(manager.snapshot().items.empty());           // no stale data from the previous account
        manager.enqueue(restartFixture("o3"));            // added while the library is still loading
        CHECK(!manager.hasComplete("o1"));
        CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(2));
        stall.release();
        manager.flushPersistence();
        std::set<std::string> ids;for(const auto&i:manager.snapshot().items)ids.insert(i.itemId);
        CHECK(ids==(std::set<std::string>{"o1","o2","o3"}));
        std::vector<DownloadItem> idx;CHECK(store.loadIndex(otherScope,idx,nullptr));
        std::set<std::string> onDisk;for(const auto&i:idx)onDisk.insert(i.itemId);
        CHECK(onDisk==ids);                               // the index lists all three, none lost to the early write
        // Leaving the account while its library is still being read must not shrink its index.
        manager.configure({});
        manager.flushPersistence();
        std::vector<DownloadItem> idx2;CHECK(store.loadIndex(otherScope,idx2,nullptr));
        CHECK(idx2.size()==3);
        {
            DownloadItem x;CHECK(store.loadManifest(otherScope,"o3",x,nullptr));
        }
        // remove o3 so the second round starts the same
        manager.configure(other);manager.flushPersistence();
        CHECK(manager.erase("o3"));manager.flushPersistence();
    }
    // Switch away before the read is even attempted: the index keeps listing everything.
    {
        manager.configure({});manager.flushPersistence();
        DiskStall stall(manager);
        manager.pause("a1");
        CHECK(stall.waitReached());
        manager.configure(other);
        manager.enqueue(restartFixture("o4"));
        manager.configure({});
        stall.release();
        manager.flushPersistence();
        std::vector<DownloadItem> idx;CHECK(store.loadIndex(otherScope,idx,nullptr));
        std::set<std::string> onDisk;for(const auto&i:idx)onDisk.insert(i.itemId);
        CHECK(onDisk==(std::set<std::string>{"o1","o2","o4"}));
    }
    // A read that really blocks in the kernel (a FIFO standing in for the index of a stuck disk):
    // configure() on the caller must not touch it.
    {
        const Session third=fixtureAccount("third");
        const std::string thirdScope=DownloadStore::scopeKey(third.serverUrl,third.userId);
        CHECK(store.ensureHlsDirectories(thirdScope,"placeholder"));
        const std::string fifo=store.scopePath(thirdScope)+"/index.v1";
        CHECK(::mkfifo(fifo.c_str(),0600)==0);
        manager.configure({});manager.flushPersistence();
        std::thread writer([&]{std::this_thread::sleep_for(std::chrono::milliseconds(800));FILE*f=std::fopen(fifo.c_str(),"w");if(f){std::fputs("MFDI=1\n",f);std::fclose(f);}});
        const auto t0=std::chrono::steady_clock::now();
        manager.configure(third);
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-t0).count();
        CHECK(ms<300);
        CHECK(manager.snapshot().items.empty());
        writer.join();
        manager.flushPersistence();
        ::unlink(fifo.c_str());
    }
    std::printf("[test] account library load is asynchronous OK\n");
}

// An account's in-flight transfer (its network response still held) must never publish into,
// delete, or fail an item of the same id in the account that is current when the response arrives
// -- the same item and source ids are normal for two users of one Jellyfin server.
static void testStaleTransferNeverTouchesAnotherAccount()
{
    std::printf("[test] stale transfer never touches another account\n");
    for (int stage = 0; stage < 2; ++stage) {      // 0: playlist discovery fails, 1: a segment is in flight
        for (int bounce = 0; bounce < 2; ++bounce) { // 0: A->B, 1: A->B->A
            Gate gate;
            const std::string seg = legitSegment(true);
            HeldServer server([&](const std::string& req) -> std::string {
                if (req.find("/seg") != std::string::npos) {
                    gate.hold();
                    return httpReply(200, seg);
                }
                if (stage == 0) {
                    gate.hold();
                    return httpReply(401, "");
                }
                return httpReply(200, "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\nseg0.ts\n#EXTINF:2,\nseg1.ts\n#EXT-X-ENDLIST\n");
            });
            const std::string root = "/tmp/miyoofin-staletransfer-" + std::to_string((long long)getpid()) + "-" +
                                     std::to_string(stage) + std::to_string(bounce);
            Session a; a.serverUrl = server.base(); a.userId = "A"; a.accessToken = "fake-A"; a.deviceId = "t";
            Session b = a; b.userId = "B"; b.accessToken = "fake-B";
            const std::string bScope = DownloadStore::scopeKey(b.serverUrl, b.userId);
            const std::string aScope = DownloadStore::scopeKey(a.serverUrl, a.userId);
            {
                DownloadStore store(root);
                DownloadItem bi = restartFixture("shared", DownloadState::Failed);
                bi.lastError = "B-original";
                CHECK(seedItem(store, bScope, bi, false));
            }
            DownloadManager manager(a, root);
            manager.setPlaybackActive(true);
            manager.flushPersistence();
            DownloadItem ai; ai.itemId = "shared"; ai.hlsStorage = true; ai.runtimeTicks = 20000000;
            manager.enqueue(ai);
            manager.setPlaybackActive(false);     // A's transfer starts and its response is held
            CHECK(gate.waitReached());
            manager.setPlaybackActive(true);
            manager.configure(b);
            manager.flushPersistence();
            const DownloadItem bBefore = pickItem(manager, "shared");
            CHECK(bBefore.state == DownloadState::Failed && bBefore.lastError == "B-original");
            if (bounce == 1) {                     // and back to A before the held response arrives
                manager.configure(a);
                manager.flushPersistence();
            }
            const int hitsBefore = server.hits();
            gate.release();
            // Let the stale transfer run to its end (its next request would show it did not stop).
            for (int n = 0; n < 40; ++n) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            CHECK(server.hits() <= hitsBefore + 3);
            if (bounce == 0) {
                const DownloadItem bAfter = pickItem(manager, "shared");
                CHECK(bAfter.state == bBefore.state);
                CHECK(bAfter.lastError == "B-original");
                CHECK(bAfter.downloadedBytes == bBefore.downloadedBytes);
                CHECK(manager.snapshot().items.size() == 1);
            } else {
                const DownloadItem again = pickItem(manager, "shared");
                CHECK(again.state != DownloadState::Unauthorized && again.state != DownloadState::Complete &&
                      again.state != DownloadState::Failed); // the reloaded A item is not the stale transfer's
            }
            manager.setPlaybackActive(true);
            manager.flushPersistence();
            {
                // B's item on disk is untouched as well.
                DownloadStore store(root);
                DownloadItem onDisk;
                if (store.loadManifest(bScope, "shared", onDisk, nullptr)) {
                    CHECK(onDisk.state == DownloadState::Failed && onDisk.lastError == "B-original");
                }
            }
            (void)aScope;
        }
    }
    std::printf("[test] stale transfer never touches another account OK\n");
}
