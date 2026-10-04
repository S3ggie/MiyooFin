// A write that fails stays owed: flush says so (false), the snapshot carries a UI-readable error,
// and the work is retried by the storage thread on its own once storage recovers; shutdown with
// storage still failing completes.
static void testFailedStorageWritesStayOwedAndRetry()
{
    std::printf("[test] failed storage writes stay owed and are retried\n");
    const std::string base = "/tmp/miyoofin-owed-" + std::to_string((long long)getpid());
    const Session other = fixtureAccount("other");
    const std::string otherScope = DownloadStore::scopeKey(other.serverUrl, other.userId);

    // 1. Transient failure, recovery with no further command (the audit reproduction).
    {
        const std::string root = base + "-1";
        DownloadManager manager({}, root);
        manager.setRetryBaseMsForTest(20);
        manager.setPlaybackActive(true);
        CHECK(manager.flushPersistence());
        DownloadStore store(root);
        BlockedStorage disk(root);
        disk.block();
        manager.enqueue(restartFixture("x"));
        manager.pause("x");
        CHECK(!manager.flushPersistence());              // not on disk: not reported as success
        CHECK(!manager.storageError().empty());
        CHECK(!manager.snapshot().storageError.empty());
        CHECK(manager.snapshot().items.size() == 1);     // memory is intact
        disk.unblock();
        CHECK(waitUntil([&] { return manifestHas(store, "anonymous", "x", DownloadState::Paused); }));
        CHECK(waitUntil([&] { return indexLists(store, "anonymous", "x"); }));
        CHECK(manager.flushPersistence());
        CHECK(manager.storageError().empty());
    }
    // 2. Permanent failure: reported, and teardown does not hang or claim success. The storage
    //    fixture is declared BEFORE the manager, so the manager really dies while storage is
    //    still blocked (the fixture restores it afterwards).
    {
        const std::string root = base + "-2";
        CHECK(::mkdir(root.c_str(), 0755) == 0 || errno == EEXIST);
        BlockedStorage disk(root);
        auto manager = std::make_unique<DownloadManager>(Session{}, root);
        manager->setRetryBaseMsForTest(20);
        manager->setPlaybackActive(true);
        manager->flushPersistence();
        disk.block();
        manager->enqueue(restartFixture("x"));
        CHECK(!manager->flushPersistence());
        CHECK(!manager->flushPersistence());          // still failing, still honest
        CHECK(!manager->snapshot().storageError.empty());
        const auto t0 = std::chrono::steady_clock::now();
        manager.reset();                                // storage is still blocked here
        CHECK(disk.blocked);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    }
    // 3. The manifest lands but the index write fails.
    {
        const std::string root = base + "-3";
        DownloadManager manager({}, root);
        manager.setRetryBaseMsForTest(20);
        manager.setPlaybackActive(true);
        manager.flushPersistence();
        DownloadStore store(root);
        ::mkdir(root.c_str(), 0755);
        CHECK(::mkdir(store.scopePath("anonymous").c_str(), 0755) == 0 || errno == EEXIST);
        const std::string indexPath = store.scopePath("anonymous") + "/index.v1";
        ::unlink(indexPath.c_str());
        CHECK(::mkdir(indexPath.c_str(), 0755) == 0);    // a directory where the index file belongs
        manager.enqueue(restartFixture("x"));
        CHECK(!manager.flushPersistence());
        CHECK(!manager.storageError().empty());
        CHECK(manifestHas(store, "anonymous", "x", DownloadState::Queued));
        CHECK(::rmdir(indexPath.c_str()) == 0);
        CHECK(waitUntil([&] { return indexLists(store, "anonymous", "x"); }));
        CHECK(manager.flushPersistence());
    }
    // 4. A newer state arrives while the older one is failing: the newer one is what lands.
    {
        const std::string root = base + "-4";
        DownloadManager manager({}, root);
        manager.setRetryBaseMsForTest(20);
        manager.setPlaybackActive(true);
        manager.flushPersistence();
        DownloadStore store(root);
        BlockedStorage disk(root);
        {
            DiskStall stall(manager);                    // the Queued copy is in flight...
            manager.enqueue(restartFixture("x"));
            CHECK(stall.waitReached());
            disk.block();                                // ...and the disk fails under it
            manager.pause("x");                          // newer state while it fails
            stall.release();
        }
        CHECK(!manager.flushPersistence());
        manager.resume("x");
        manager.pause("x");
        disk.unblock();
        CHECK(waitUntil([&] { return manifestHas(store, "anonymous", "x", DownloadState::Paused); }));
        CHECK(manager.flushPersistence());
        DownloadItem live;for(const auto&i:manager.snapshot().items)if(i.itemId=="x")live=i;
        CHECK(live.state == DownloadState::Paused);
    }
    // 5. The account changes while its write is failing (and again while still failing).
    {
        const std::string root = base + "-5";
        DownloadManager manager({}, root);
        manager.setRetryBaseMsForTest(20);
        manager.setPlaybackActive(true);
        manager.flushPersistence();
        DownloadStore store(root);
        BlockedStorage disk(root);
        disk.block();
        manager.enqueue(restartFixture("x"));
        manager.pause("x");
        CHECK(!manager.flushPersistence());
        manager.configure(other);                        // leave the account whose write is owed
        CHECK(!manager.flushPersistence());
        CHECK(manager.snapshot().items.empty());         // not the other account's, not x
        manager.configure({});                           // and come back while it is still owed
        CHECK(!manager.flushPersistence());
        {
            std::set<std::string> ids;for(const auto&i:manager.snapshot().items)ids.insert(i.itemId);
            CHECK(ids == (std::set<std::string>{"x"}));  // the owed state was not lost
        }
        manager.configure(other);                        // leave again, then recover
        disk.unblock();
        CHECK(waitUntil([&] { return manifestHas(store, "anonymous", "x", DownloadState::Paused); }));
        CHECK(waitUntil([&] { return indexLists(store, "anonymous", "x"); }));
        CHECK(manager.flushPersistence());
        CHECK(!indexLists(store, otherScope, "x"));      // nothing crossed into the other account
        CHECK(::access(store.manifestPath(otherScope, "x").c_str(), F_OK) != 0);
        manager.configure({});
        manager.flushPersistence();
        CHECK(manager.snapshot().items.size() == 1);
    }
    std::printf("[test] failed storage writes stay owed and are retried OK\n");
}

// A read that fails is not an empty library: nothing is published or rewritten from it, the
// library stays "loading" with a plain error, and everything recovers by itself.
static void testUnavailableStorageIsNeverAnEmptyLibrary()
{
    std::printf("[test] unavailable storage is never an empty library\n");
    const std::string base = "/tmp/miyoofin-unreadable-" + std::to_string((long long)getpid());
    const Session other = fixtureAccount("other");
    const std::string otherScope = DownloadStore::scopeKey(other.serverUrl, other.userId);

    // 1. Startup with storage unavailable (the review reproduction), plus an enqueue meanwhile.
    {
        const std::string root = base + "-1";
        {
            DownloadStore store(root);
            CHECK(seedItem(store, "anonymous", restartFixture("kept", DownloadState::Paused), false));
        }
        DownloadStore store(root);
        const std::string indexBefore = slurp(store.scopePath("anonymous") + "/index.v1");
        CHECK(!indexBefore.empty());
        BlockedStorage disk(root);
        disk.block();
        DownloadManager manager({}, root);
        manager.setRetryBaseMsForTest(20);
        manager.setPlaybackActive(true);
        CHECK(!manager.flushPersistence());
        DownloadSnapshot snap = manager.snapshot();
        CHECK(snap.loading && snap.items.empty() && !snap.storageError.empty());
        CHECK(downloadsEmptyState(snap).title == "Can't read downloads");
        manager.enqueue(restartFixture("fresh"));          // while the library is still unread
        for (int n = 0; n < 5; ++n) {                      // several failed reads in a row
            usleep(60000);
            CHECK(!manager.flushPersistence());
        }
        disk.unblock();
        CHECK(waitUntil([&] { return !manager.snapshot().loading; }));
        CHECK(waitUntil([&] { return indexIds(store, "anonymous") == std::set<std::string>{"kept", "fresh"}; }));
        CHECK(manager.flushPersistence());
        CHECK(liveIds(manager) == (std::set<std::string>{"kept", "fresh"}));
        CHECK(manager.snapshot().storageError.empty());
        CHECK(manifestHas(store, "anonymous", "kept", DownloadState::Paused));
    }
    // 2. Account switch while storage is unavailable: no empty truth, no other account's data.
    {
        const std::string root = base + "-2";
        {
            DownloadStore store(root);
            CHECK(seedItem(store, "anonymous", restartFixture("kept", DownloadState::Paused), false));
            CHECK(seedItem(store, otherScope, restartFixture("o1", DownloadState::Paused), false));
        }
        DownloadStore store(root);
        DownloadManager manager({}, root);
        manager.setRetryBaseMsForTest(20);
        manager.setPlaybackActive(true);
        CHECK(manager.flushPersistence());
        CHECK(liveIds(manager) == (std::set<std::string>{"kept"}));
        BlockedStorage disk(root);
        disk.block();
        manager.configure(other);
        CHECK(!manager.flushPersistence());
        DownloadSnapshot snap = manager.snapshot();
        CHECK(snap.loading && snap.items.empty());          // not "kept", not an authoritative empty
        disk.unblock();
        CHECK(waitUntil([&] { return liveIds(manager) == std::set<std::string>{"o1"}; }));
        CHECK(manager.flushPersistence());
        CHECK(indexIds(store, otherScope) == (std::set<std::string>{"o1"}));
        CHECK(indexIds(store, "anonymous") == (std::set<std::string>{"kept"}));
    }
    // 3. Genuinely new things initialise normally: a root that does not exist yet, and a new account.
    {
        const std::string root = base + "-3";          // never created
        DownloadManager manager({}, root);
        manager.setPlaybackActive(true);
        CHECK(manager.flushPersistence());
        DownloadSnapshot snap = manager.snapshot();
        CHECK(!snap.loading && snap.items.empty() && snap.storageError.empty());
        CHECK(downloadsEmptyState(snap).title == "No downloads yet");
        manager.configure(fixtureAccount("brand-new"));
        CHECK(manager.flushPersistence());
        snap = manager.snapshot();
        CHECK(!snap.loading && snap.items.empty() && snap.storageError.empty());
        manager.enqueue(restartFixture("n1"));
        CHECK(manager.flushPersistence());
        DownloadStore store(root);
        CHECK(indexIds(store, DownloadStore::scopeKey(fixtureAccount("brand-new").serverUrl, "brand-new")) ==
              (std::set<std::string>{"n1"}));
    }
    // 4. A corrupted index with readable manifests is rebuilt; an unreadable manifest makes the
    //    rebuild fail instead, leaving the (damaged but present) metadata alone until it recovers.
    {
        const std::string root = base + "-4";
        {
            DownloadStore store(root);
            CHECK(seedItem(store, "anonymous", restartFixture("a", DownloadState::Paused), false));
            CHECK(seedItem(store, "anonymous", restartFixture("b", DownloadState::Paused), false));
        }
        DownloadStore store(root);
        const std::string indexPath = store.scopePath("anonymous") + "/index.v1";
        {
            FILE* f = std::fopen(indexPath.c_str(), "wb");
            CHECK(f != nullptr);
            if (f) { std::fputs("garbage\n", f); std::fclose(f); }
        }
        // 4a. rebuild succeeds
        {
            DownloadManager manager({}, root);
            manager.setPlaybackActive(true);
            CHECK(manager.flushPersistence());
            CHECK(liveIds(manager) == (std::set<std::string>{"a", "b"}));
            CHECK(indexIds(store, "anonymous") == (std::set<std::string>{"a", "b"}));
        }
        // 4b. damaged index and one manifest that cannot be read: the scan fails as a whole
        {
            FILE* f = std::fopen(indexPath.c_str(), "wb");
            if (f) { std::fputs("garbage\n", f); std::fclose(f); }
            const std::string bManifest = store.manifestPath("anonymous", "b");
            CHECK(::rename(bManifest.c_str(), (bManifest + ".keep").c_str()) == 0);
            CHECK(::mkdir(bManifest.c_str(), 0755) == 0);  // reading a directory fails (EISDIR)
            DownloadManager manager({}, root);
            manager.setRetryBaseMsForTest(20);
            manager.setPlaybackActive(true);
            CHECK(!manager.flushPersistence());
            CHECK(manager.snapshot().loading && manager.snapshot().items.empty());
            CHECK(slurp(indexPath) == "garbage\n");         // nothing was written from the failed scan
            CHECK(::rmdir(bManifest.c_str()) == 0);
            CHECK(::rename((bManifest + ".keep").c_str(), bManifest.c_str()) == 0);
            CHECK(waitUntil([&] { return liveIds(manager) == std::set<std::string>{"a", "b"}; }));
            CHECK(waitUntil([&] { return indexIds(store, "anonymous") == std::set<std::string>{"a", "b"}; }));
        }
    }
    // 5. Leaving an account whose library was never read, and a removal whose index patch cannot
    //    be read, must not drop entries they have not seen.
    {
        const std::string root = base + "-5";
        {
            DownloadStore store(root);
            CHECK(seedItem(store, "anonymous", restartFixture("kept", DownloadState::Paused), false));
            CHECK(seedItem(store, "anonymous", restartFixture("gone", DownloadState::Paused), false));
        }
        DownloadStore store(root);
        {
            BlockedStorage disk(root);
            disk.block();
            DownloadManager manager({}, root);              // library unread
            manager.setRetryBaseMsForTest(20);
            manager.setPlaybackActive(true);
            manager.enqueue(restartFixture("fresh"));
            manager.configure(other);                       // leave it unread (partial final write)
            CHECK(!manager.flushPersistence());
            disk.unblock();
            CHECK(waitUntil([&] {
                return indexIds(store, "anonymous") == std::set<std::string>{"kept", "gone", "fresh"};
            }));
            manager.configure({});
            CHECK(waitUntil([&] { return liveIds(manager).size() == 3; }));
            CHECK(manager.flushPersistence());
            // Now an erase whose cleanup cannot run, followed by an account switch.
            disk.block();
            CHECK(manager.erase("gone"));
            manager.configure(other);
            CHECK(!manager.flushPersistence());
            disk.unblock();
            CHECK(waitUntil([&] { return ::access(store.itemPath("anonymous", "gone").c_str(), F_OK) != 0; }));
            CHECK(waitUntil([&] { return indexIds(store, "anonymous") == std::set<std::string>{"kept", "fresh"}; }));
            CHECK(manager.flushPersistence());
        }
    }
    std::printf("[test] unavailable storage is never an empty library OK\n");
}


// The root is gone (or replaced) while its parent is still there. For storage that never held a
// library that is a first run; for one that did -- a marker beside the root, written after the
// first successful write, survives a restart -- it is unavailable storage and must neither be
// initialised as empty nor written to.
static void testEstablishedStorageIsNeverMistakenForAFirstRun()
{
    std::printf("[test] established storage is never mistaken for a first run\n");
    const std::string base = "/tmp/miyoofin-established-" + std::to_string((long long)getpid());
    const Session other = fixtureAccount("other");
    const std::string otherScope = DownloadStore::scopeKey(other.serverUrl, other.userId);

    // 1. Fresh process, library on disk, root absent, parent present.
    {
        const std::string root = base + "-1";
        {
            DownloadStore store(root);
            CHECK(seedItem(store, "anonymous", restartFixture("kept", DownloadState::Paused), false));
            CHECK(::access(store.markerPath().c_str(), F_OK) == 0); // written by the first write
        }
        DownloadStore store(root);
        CHECK(store.storageReadable());
        BlockedStorage disk(root);
        CHECK(::rename(root.c_str(), (root + ".aside").c_str()) == 0); // root absent, parent there
        disk.blocked = false;                                          // (we restore by hand below)
        CHECK(!store.storageReadable());
        DownloadManager manager({}, root);
        manager.setRetryBaseMsForTest(20);
        manager.setPlaybackActive(true);
        CHECK(!manager.flushPersistence());
        DownloadSnapshot snap = manager.snapshot();
        CHECK(snap.loading && snap.items.empty() && !snap.storageError.empty());
        manager.enqueue(restartFixture("while-away"));  // queued while the library cannot be read
        CHECK(!manager.flushPersistence());
        CHECK(::access(root.c_str(), F_OK) != 0);        // nothing re-created the root...
        CHECK(::access((root + ".aside").c_str(), F_OK) == 0);
        // ...so nothing was initialised or overwritten. The storage comes back:
        CHECK(::rename((root + ".aside").c_str(), root.c_str()) == 0);
        CHECK(waitUntil([&] { return liveIds(manager) == std::set<std::string>{"kept", "while-away"}; }));
        CHECK(waitUntil([&] { return indexIds(store, "anonymous") == std::set<std::string>{"kept", "while-away"}; }));
        CHECK(manager.flushPersistence());
        CHECK(manager.snapshot().storageError.empty() && !manager.snapshot().loading);
    }
    // 2. Account switch after the root has disappeared under a running manager.
    {
        const std::string root = base + "-2";
        {
            DownloadStore store(root);
            CHECK(seedItem(store, "anonymous", restartFixture("kept", DownloadState::Paused), false));
            CHECK(seedItem(store, otherScope, restartFixture("o1", DownloadState::Paused), false));
        }
        DownloadStore store(root);
        DownloadManager manager({}, root);
        manager.setRetryBaseMsForTest(20);
        manager.setPlaybackActive(true);
        CHECK(manager.flushPersistence());
        CHECK(::rename(root.c_str(), (root + ".aside").c_str()) == 0);
        manager.configure(fixtureAccount("third"));      // a scope with no folder yet: still unreadable
        CHECK(!manager.flushPersistence());
        CHECK(manager.snapshot().loading && manager.snapshot().items.empty());
        CHECK(::access(root.c_str(), F_OK) != 0);
        CHECK(::rename((root + ".aside").c_str(), root.c_str()) == 0);
        CHECK(waitUntil([&] { return !manager.snapshot().loading; }));
        CHECK(manager.snapshot().items.empty());          // 'third' really is new, now that it can be seen
        manager.configure({});
        CHECK(waitUntil([&] { return liveIds(manager) == std::set<std::string>{"kept"}; }));
        CHECK(indexIds(store, "anonymous") == (std::set<std::string>{"kept"}));
    }
    // 3. An empty mount point left behind by storage that went away (a different device than the
    //    one the library was on), and a dangling link in place of the root.
    {
        const std::string root = base + "-3";
        {
            DownloadStore store(root);
            CHECK(seedItem(store, "anonymous", restartFixture("kept", DownloadState::Paused), false));
        }
        DownloadStore store(root);
        const std::string marker = slurp(store.markerPath());
        CHECK(marker.find("root_dev=") != std::string::npos);
        {   // pretend the library lived on another device
            FILE* f = std::fopen(store.markerPath().c_str(), "wb");
            CHECK(f != nullptr);
            if (f) { std::fputs("MFRT=1\nroot_dev=1\n", f); std::fclose(f); }
        }
        CHECK(!store.storageReadable());
        DownloadManager manager({}, root);
        manager.setRetryBaseMsForTest(20);
        manager.setPlaybackActive(true);
        CHECK(!manager.flushPersistence());
        CHECK(manager.snapshot().loading && manager.snapshot().items.empty());
        {
            FILE* f = std::fopen(store.markerPath().c_str(), "wb");
            if (f) { std::fputs(marker.c_str(), f); std::fclose(f); }   // the device is back
        }
        CHECK(waitUntil([&] { return liveIds(manager) == std::set<std::string>{"kept"}; }));
        CHECK(manager.flushPersistence());

        const std::string linkRoot = base + "-3link";
        CHECK(::symlink((base + "-3-nowhere").c_str(), linkRoot.c_str()) == 0);
        DownloadStore linked(linkRoot);
        CHECK(!linked.storageReadable());
        ::unlink(linkRoot.c_str());
    }
    // 4. Genuine first run and genuine new account keep working.
    {
        const std::string root = base + "-4";
        DownloadStore store(root);
        CHECK(store.storageReadable());                  // never held a library, not there yet
        DownloadManager manager({}, root);
        manager.setPlaybackActive(true);
        CHECK(manager.flushPersistence());
        manager.enqueue(restartFixture("first"));
        CHECK(manager.flushPersistence());
        CHECK(::access(store.markerPath().c_str(), F_OK) == 0);
        manager.configure(other);
        CHECK(manager.flushPersistence());
        CHECK(!manager.snapshot().loading && manager.snapshot().storageError.empty());
    }
    std::printf("[test] established storage is never mistaken for a first run OK\n");
}

// Storage that comes back after the library was read as "nothing there" and before its first
// index write must be merged, never replaced by an index of only the new items.
static void testStorageReturningBeforeTheFirstIndexWriteIsMerged()
{
    std::printf("[test] storage returning before the first index write is merged\n");
    const std::string base = "/tmp/miyoofin-returning-" + std::to_string((long long)getpid());
    for (int legacy = 0; legacy < 2; ++legacy) { // 1: the returning library has no marker
        const std::string root = base + "-" + std::to_string(legacy);
        DownloadStore probe(root);
        std::atomic<bool> seeded{false};
        std::unique_ptr<DownloadManager> manager;
        {
            manager = std::make_unique<DownloadManager>(Session{}, root);
            manager->setPlaybackActive(true);
            CHECK(manager->flushPersistence()); // empty first run: nothing there, nothing written yet...
        }
        // ...except what a first run writes (an empty index). Start over so the race is the first write.
        manager.reset();
        {   // remove only this test's own folder content, by exact path
            std::map<std::string, std::string> files;
            snapshotTree(root, files);
            for (const auto& f : files)
                ::unlink(f.first.c_str());
            ::unlink(probe.markerPath().c_str());
        }
        manager = std::make_unique<DownloadManager>(Session{}, root);
        manager->setPlaybackActive(true);
        DownloadStore returning(root);
        // The barrier: the first storage operation after new1's manifest is down is the index
        // write; just before it, the storage "comes back" holding a library.
        manager->setAsyncWriteHookForTest([&] {
            if (!seeded.load() && ::access(probe.manifestPath("anonymous", "new1").c_str(), F_OK) == 0) {
                seeded = true;
                CHECK(seedItem(returning, "anonymous", restartFixture("old1", DownloadState::Paused), false));
                CHECK(seedItem(returning, "anonymous", restartFixture("old2", DownloadState::Paused), false));
                if (legacy)
                    ::unlink(probe.markerPath().c_str());
            }
        });
        manager->enqueue(restartFixture("new1"));
        CHECK(waitUntil([&] { return seeded.load(); }));
        manager->flushPersistence();
        manager->setAsyncWriteHookForTest(nullptr);
        CHECK(waitUntil([&] { return !manager->snapshot().loading; }));
        manager->flushPersistence();
        const std::set<std::string> all{"new1", "old1", "old2"};
        CHECK(liveIds(*manager) == all);
        CHECK(indexIds(probe, "anonymous") == all);           // merged, not replaced
        CHECK(manifestHas(probe, "anonymous", "old1", DownloadState::Paused));
        // An account switch and back, and a later removal, stay consistent (no resurrection).
        manager->configure(fixtureAccount("other"));
        manager->flushPersistence();
        manager->configure({});
        CHECK(waitUntil([&] { return liveIds(*manager) == all; }));
        CHECK(manager->erase("old1"));
        manager->flushPersistence();
        CHECK(indexIds(probe, "anonymous") == (std::set<std::string>{"new1", "old2"}));
        CHECK(::access(probe.itemPath("anonymous", "old1").c_str(), F_OK) != 0);
    }
    std::printf("[test] storage returning before the first index write is merged OK\n");
}

// The admitted window: the storage comes back after the first-index scan found nothing and before
// the exclusive create. The index then made lists only our items; the returned library must still
// be found and merged without a restart or a manual rebuild.
static void testStorageReturningInTheIndexCreateGapIsMerged()
{
    std::printf("[test] storage returning in the index create gap is merged\n");
    const std::string base = "/tmp/miyoofin-gap-" + std::to_string((long long)getpid());
    for (int legacy = 0; legacy < 2; ++legacy) { // 1: the returning library has no marker
        const std::string root = base + "-" + std::to_string(legacy);
        DownloadStore probe(root);
        std::atomic<bool> seeded{false};
        std::unique_ptr<DownloadManager> manager;
        {
            manager = std::make_unique<DownloadManager>(Session{}, root);
            manager->setPlaybackActive(true);
            CHECK(manager->flushPersistence()); // empty first run: nothing there, nothing written yet...
        }
        // ...except what a first run writes (an empty index). Start over so the race is the first write.
        manager.reset();
        {   // remove only this test's own folder content, by exact path
            std::map<std::string, std::string> files;
            snapshotTree(root, files);
            for (const auto& f : files)
                ::unlink(f.first.c_str());
            ::unlink(probe.markerPath().c_str());
        }
        manager = std::make_unique<DownloadManager>(Session{}, root);
        manager->setPlaybackActive(true);
        DownloadStore returning(root);
        // The barrier: the first storage operation after new1's manifest is down is the index
        // write; just before it, the storage "comes back" holding a library.
        manager->setIndexGapHookForTest([&] {
            if (!seeded.load() && ::access(probe.manifestPath("anonymous", "new1").c_str(), F_OK) == 0) {
                seeded = true;
                { // manifests only: no index beside them
                    DownloadItem item = restartFixture("old1", DownloadState::Paused);
                    CHECK(returning.ensureHlsDirectories("anonymous", item.itemId));
                    CHECK(returning.saveManifest("anonymous", item, nullptr));
                }
                { // manifests only: no index beside them
                    DownloadItem item = restartFixture("old2", DownloadState::Paused);
                    CHECK(returning.ensureHlsDirectories("anonymous", item.itemId));
                    CHECK(returning.saveManifest("anonymous", item, nullptr));
                }
                if (legacy)
                    ::unlink(probe.markerPath().c_str());
            }
        });
        manager->enqueue(restartFixture("new1"));
        CHECK(waitUntil([&] { return seeded.load(); }));
        manager->flushPersistence();
        manager->setIndexGapHookForTest(nullptr);
        CHECK(waitUntil([&] { return !manager->snapshot().loading; }));
        manager->flushPersistence();
        const std::set<std::string> all{"new1", "old1", "old2"};
        CHECK(liveIds(*manager) == all);
        CHECK(indexIds(probe, "anonymous") == all);           // merged, not replaced
        CHECK(manifestHas(probe, "anonymous", "old1", DownloadState::Paused));
        // An account switch and back, and a later removal, stay consistent (no resurrection).
        manager->configure(fixtureAccount("other"));
        manager->flushPersistence();
        manager->configure({});
        CHECK(waitUntil([&] { return liveIds(*manager) == all; }));
        CHECK(manager->erase("old1"));
        manager->flushPersistence();
        CHECK(indexIds(probe, "anonymous") == (std::set<std::string>{"new1", "old2"}));
        CHECK(::access(probe.itemPath("anonymous", "old1").c_str(), F_OK) != 0);
    }
    std::printf("[test] storage returning in the index create gap is merged OK\n");
}

// The storage goes away right after the first index is created, before the late manifest scan:
// the scan must report Unavailable (not an empty library), stay owed, and merge the returned
// library once the storage is back, with no command, restart or forced flush.
static void testLateScanStaysOwedWhileStorageIsGone()
{
    std::printf("[test] late scan stays owed while storage is gone\n");
    const std::string root = "/tmp/miyoofin-latescan-" + std::to_string((long long)getpid());
    const std::string away = root + "-away";
    DownloadStore probe(root);
    auto manager = std::make_unique<DownloadManager>(Session{}, root);
    manager->setPlaybackActive(true);
    manager->setRetryBaseMsForTest(20);
    CHECK(manager->flushPersistence());
    std::atomic<bool> seeded{false}, blocked{false};
    manager->setIndexGapHookForTest([&] {
        if (seeded.load())
            return;
        DownloadStore returning(root);
        for (const char* id : {"old1", "old2"}) {
            DownloadItem item = restartFixture(id, DownloadState::Paused);
            CHECK(returning.ensureHlsDirectories("anonymous", id));
            CHECK(returning.saveManifest("anonymous", item, nullptr));
        }
        seeded = true;
    });
    manager->setAsyncWriteHookForTest([&] {
        if (seeded.load() && !blocked.load() &&
            ::access((probe.scopePath("anonymous") + "/index.v1").c_str(), F_OK) == 0) {
            CHECK(::rename(root.c_str(), away.c_str()) == 0);
            blocked = true;
        }
    });
    manager->enqueue(restartFixture("new1"));
    CHECK(waitUntil([&] { return blocked.load() && !manager->storageError().empty(); }));
    std::vector<DownloadItem> scan;
    CHECK(!probe.storageReadable());
    CHECK(probe.scanManifests("anonymous", scan, nullptr) == LibraryStatus::Unavailable);
    CHECK(::rename(away.c_str(), root.c_str()) == 0);
    const std::set<std::string> all{"new1", "old1", "old2"};
    CHECK(waitUntil([&] { return liveIds(*manager) == all && indexIds(probe, "anonymous") == all; }));
    manager->setAsyncWriteHookForTest(nullptr);
    manager->setIndexGapHookForTest(nullptr);
    manager.reset();
    std::printf("[test] late scan stays owed while storage is gone OK\n");
}

// Storage whose identity does not match the library's (same ids and manifests, different volume):
// nothing on it may be created, rewritten or deleted, while an erase stays owed and completes only
// once the intended storage is back.
static void testMismatchedStorageIsNeverMutated()
{
    std::printf("[test] mismatched storage is never mutated\n");
    const std::string root = "/tmp/miyoofin-mismatch-" + std::to_string((long long)getpid());
    {
        DownloadStore store(root);
        DownloadItem a = restartFixture("keep", DownloadState::Paused);
        DownloadItem b = restartFixture("victim", DownloadState::Paused);
        CHECK(seedItem(store, "anonymous", a, true));
        CHECK(seedItem(store, "anonymous", b, true));
    }
    DownloadStore store(root);
    const std::string good = slurp(store.markerPath());
    CHECK(good.find("root_dev=") != std::string::npos);
    DownloadManager manager({}, root);
    manager.setRetryBaseMsForTest(20);
    manager.setPlaybackActive(true);
    CHECK(manager.flushPersistence());
    CHECK(liveIds(manager) == (std::set<std::string>{"keep", "victim"}));
    {   // now the root is "another volume": same ids, same manifests, wrong device
        FILE* f = std::fopen(store.markerPath().c_str(), "wb");
        CHECK(f != nullptr);
        if (f) { std::fputs("MFRT=1\nroot_dev=1\n", f); std::fclose(f); }
    }
    std::map<std::string, std::string> before;
    snapshotTree(root, before);
    CHECK(!before.empty());
    CHECK(manager.erase("victim"));                       // owed, not performed
    CHECK(!manager.flushPersistence());
    manager.pause("keep");                                // a write, also owed
    CHECK(!manager.flushPersistence());
    CHECK(!store.removeItem("anonymous", "victim"));      // the store itself refuses, mutating nothing
    CHECK(!store.removePartialBytes("anonymous", "keep"));
    std::vector<DownloadItem> unused;
    CHECK(!store.rebuildIndex("anonymous", unused));
    std::map<std::string, std::string> during;
    snapshotTree(root, during);
    CHECK(during == before);                              // ALL existing bytes unchanged
    {   // the intended storage is back
        FILE* f = std::fopen(store.markerPath().c_str(), "wb");
        if (f) { std::fputs(good.c_str(), f); std::fclose(f); }
    }
    CHECK(waitUntil([&] { return ::access(store.itemPath("anonymous", "victim").c_str(), F_OK) != 0; }));
    CHECK(manager.flushPersistence());
    CHECK(indexIds(store, "anonymous") == (std::set<std::string>{"keep"}));
    {
        DownloadItem m;
        CHECK(store.loadManifest("anonymous", "keep", m, nullptr));
    }
    std::printf("[test] mismatched storage is never mutated OK\n");
}
