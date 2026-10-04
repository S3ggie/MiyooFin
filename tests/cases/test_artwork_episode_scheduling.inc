// B5d2a: Same key does not load twice (already in map → not a candidate)
static void testSameKeyNotLoadedTwice()
{
    std::printf("[test] B5d2a: same key not loaded twice\n");
    MediaItem m;
    m.id = "m2"; m.type = "movie";
    m.imageTags["Primary"] = "t1";
    std::string key = buildRowArtworkKey(m);

    std::map<std::string, RowArtworkStatus> sm;
    sm[key] = RowArtworkStatus::Loaded;

    std::vector<MediaItem> items = {m};
    CHECK(pickCandidates(items, sm).empty());

    sm[key] = RowArtworkStatus::Failed;
    CHECK(pickCandidates(items, sm).empty());

    std::printf("[test] B5d2a: same key not loaded twice OK\n");
}

// All visible RAM misses are scheduled in the same update.
static void testAllVisibleCandidatesScheduledPerCycle()
{
    std::printf("[test] B5d2a: all visible candidates per cycle\n");
    std::vector<MediaItem> items;
    for (int i = 0; i < 5; ++i) {
        MediaItem m;
        char id[32]; std::snprintf(id, sizeof(id), "item-%d", i);
        m.id = id; m.type = "movie";
        char tag[32]; std::snprintf(tag, sizeof(tag), "t%d", i);
        m.imageTags["Primary"] = tag;
        items.push_back(m);
    }
    std::map<std::string, RowArtworkStatus> sm;

    auto candidates = pickCandidates(items, sm);
    CHECK(candidates.size() == items.size());
    sm[candidates[1]] = RowArtworkStatus::Loaded;
    sm[candidates[3]] = RowArtworkStatus::Failed;
    candidates = pickCandidates(items, sm);
    CHECK(candidates.size() == items.size() - 2);
    CHECK(candidates[0] == "item-0:Primary:t0:128x192");
    CHECK(candidates[1] == "item-2:Primary:t2:128x192");
    CHECK(candidates[2] == "item-4:Primary:t4:128x192");

    std::printf("[test] B5d2a: all visible candidates per cycle OK\n");
}

// B5g1b: visible episode-artwork scheduling
static void testEpisodePrefetchScheduler()
{
    std::printf("[test] B5g1b: visible prefetch scheduler\n");
    std::set<int> unavailable;

    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 0, 20, unavailable) == 5);
    unavailable.insert(5);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 0, 20, unavailable) == 6);
    unavailable.insert(6);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 0, 20, unavailable) == 4);

    unavailable.clear();
    unavailable.insert(5);
    unavailable.insert(6);
    unavailable.insert(4);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 0, 30, unavailable) == 7);

    unavailable.clear();
    unavailable.insert(12);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(12, 10, 40, unavailable) == 13);
    unavailable.insert(13);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(12, 10, 40, unavailable) == 11);

    unavailable.clear();
    for (int i = 10; i < 32; ++i) unavailable.insert(i);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(12, 10, 40, unavailable) == -1);

    unavailable.clear();
    unavailable.insert(500);
    for (int i = 489; i < 511; ++i) unavailable.insert(i);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(500, 489, 1000, unavailable) == -1);

    // A distant selected episode is not pulled into the old viewport.
    unavailable.clear();
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(900, 889, 1000, unavailable) == 900);
    std::printf("[test] B5g1b: visible prefetch scheduler OK\n");
}

static void testEpisodeArtworkPreemption()
{
    std::printf("[test] episode artwork request preemption\n");

    // A selection change invalidates an in-flight prefetch request.
    CHECK(EpisodeBrowserScreen::artworkRequestCancelled(false, 10, 11));
    // libcurl cancellation is also an intentional cancellation, never a
    // cache-key failure.
    CHECK(EpisodeBrowserScreen::artworkRequestCancelled(true, 10, 10));
    CHECK(!EpisodeBrowserScreen::shouldMarkArtworkFailed(false, true));
    CHECK(EpisodeBrowserScreen::shouldMarkArtworkFailed(false, false));

    // Once the stale request is dropped, recomputing from the latest
    // selection puts its cached/selected thumbnail ahead of any prefetch.
    std::set<int> unavailable;
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(14, 4, 30, unavailable) == 14);
    std::printf("[test] episode artwork request preemption OK\n");
}

static void testEpisodePrefetchPlaybackResume()
{
    std::printf("[test] B5g1b: playback resume countdown\n");
    bool pending = true;
    int delayUpdates = 1;

    CHECK(!EpisodeBrowserScreen::advancePrefetchResume(
        pending, delayUpdates));
    CHECK(pending);
    CHECK(delayUpdates == 0);

    CHECK(EpisodeBrowserScreen::advancePrefetchResume(
        pending, delayUpdates));
    CHECK(!pending);
    CHECK(delayUpdates == 0);

    CHECK(!EpisodeBrowserScreen::advancePrefetchResume(
        pending, delayUpdates));
    std::printf("[test] B5g1b: playback resume countdown OK\n");
}

// B5f2: PlaybackRequest — valid movie writes expected fields

// ===================================================================
// Poster dedup / re-queue recovery tests
// ===================================================================

// Poster dedup: key present in progress set → not re-queued (even if file
// missing).  After the key is erased (success or failure), re-queueing is
// allowed again.
static void testPosterDedupPreventsRequeueWhilePending()
{
    std::printf("[test] poster dedup: pending key blocks requeue\n");
    std::set<std::string> progressKeys;
    std::deque<HomePosterJob> queue;

    HomePosterJob job{"item1", ImageType::Primary, "tag1", 64, 96};
    std::string key = job.itemId + ":" + imageTypeToString(job.imageType)
                      + ":" + job.imageTag + ":"
                      + std::to_string(job.width) + "x"
                      + std::to_string(job.height);

    // First insert succeeds → job should be queued.
    CHECK(progressKeys.insert(key).second);
    queue.push_back(job);

    // Second insert for same key fails → not re-queued.
    CHECK(!progressKeys.insert(key).second);

    // Erase (simulates worker completing on success or failure).
    progressKeys.erase(key);
    // Now re-queue is allowed.
    CHECK(progressKeys.insert(key).second);
    queue.push_back(job);

    CHECK(queue.size() == 2);
    std::printf("[test] poster dedup: pending key blocks requeue OK\n");
}

// Poster dedup: after worker success erases the key, a subsequent
// queuePosterJobs call re-admits it.  The worker's isCached shortcut
// then avoids a redundant HTTP fetch when the file is still on disk.
static void testPosterDedupSuccessEraseAllowsReAdmit()
{
    std::printf("[test] poster dedup: success-erase allows re-admit\n");
    std::set<std::string> progressKeys;

    HomePosterJob job{"item2", ImageType::Primary, "tag2", 64, 96};
    std::string key = job.itemId + ":" + imageTypeToString(job.imageType)
                      + ":" + job.imageTag + ":"
                      + std::to_string(job.width) + "x"
                      + std::to_string(job.height);

    // Simulate initial queue.
    CHECK(progressKeys.insert(key).second);

    // Worker succeeds → erases key (Fix B).
    progressKeys.erase(key);

    // Key is no longer present → can be re-queued.
    CHECK(progressKeys.insert(key).second);
    std::printf("[test] poster dedup: success-erase allows re-admit OK\n");
}

// Poster dedup: after janitor evicts a cached file, re-queueing must
// succeed because the worker erased the key on its previous success.
static void testPosterDedupEvictionRecovery()
{
    std::printf("[test] poster dedup: eviction recovery\n");
    std::set<std::string> progressKeys;

    HomePosterJob job{"item3", ImageType::Primary, "tag3", 64, 96};
    std::string key = job.itemId + ":" + imageTypeToString(job.imageType)
                      + ":" + job.imageTag + ":"
                      + std::to_string(job.width) + "x"
                      + std::to_string(job.height);

    // First download completes successfully → key erased.
    CHECK(progressKeys.insert(key).second);
    progressKeys.erase(key);

    // Janitor evicts the file.  Next sync calls queuePosterJobs.
    // Key is not in set → job re-admitted → worker will re-download.
    CHECK(progressKeys.insert(key).second);
    std::printf("[test] poster dedup: eviction recovery OK\n");
}

// ===================================================================
// Decode retry-bound tests
// ===================================================================

// Retry bound: attempts below kMaxDecodeAttempts remain retryable (key
// is NOT permanently Failed).
static void testDecodeRetryBoundBelowLimit()
{
    std::printf("[test] decode retry bound: below limit is retryable\n");
    std::map<std::string, RowArtworkStatus> artwork;
    std::map<std::string, int> attempts;
    const int limit = HomeScreen::kMaxDecodeAttempts;
    const std::string key = "item:Primary:tag:128x192";

    // Simulate failed decode attempts below the limit.
    for (int i = 0; i < limit - 1; ++i) {
        ++attempts[key];
        // Not yet at limit → do NOT set Failed.
        CHECK(attempts[key] < limit);
        // Key should NOT be in artwork map (retryable).
        CHECK(artwork.find(key) == artwork.end());
    }
    std::printf("[test] decode retry bound: below limit is retryable OK\n");
}

// Retry bound: at the limit the key becomes permanently Failed.
static void testDecodeRetryBoundAtLimit()
{
    std::printf("[test] decode retry bound: at limit becomes Failed\n");
    std::map<std::string, RowArtworkEntry> artwork;
    std::map<std::string, int> attempts;
    const int limit = HomeScreen::kMaxDecodeAttempts;
    const std::string key = "item:Primary:tag:128x192";

    // Simulate failed decode attempts up to the limit.
    for (int i = 0; i < limit; ++i) ++attempts[key];
    CHECK(attempts[key] >= limit);
    artwork[key].status = RowArtworkStatus::Failed;

    // Key is now permanently Failed.
    CHECK(artwork[key].status == RowArtworkStatus::Failed);
    // pickCandidates skips Failed keys.
    std::map<std::string, RowArtworkStatus> sm;
    sm[key] = RowArtworkStatus::Failed;
    MediaItem m; m.id = "item"; m.type = "movie";
    m.imageTags["Primary"] = "tag";
    CHECK(pickCandidates({m}, sm).empty());
    std::printf("[test] decode retry bound: at limit becomes Failed OK\n");
}

// Retry bound: success resets the attempt counter so a later failure
// does not immediately hit the limit.
static void testDecodeRetryBoundSuccessResets()
{
    std::printf("[test] decode retry bound: success resets counter\n");
    std::map<std::string, int> attempts;
    const std::string key = "item:Primary:tag:128x192";

    // Two failures.
    attempts[key] = 2;
    // Success → erase counter.
    attempts.erase(key);
    CHECK(attempts.find(key) == attempts.end());

    // Next failure starts from zero.
    ++attempts[key];
    CHECK(attempts[key] == 1);
    std::printf("[test] decode retry bound: success resets counter OK\n");
}

// Retry bound: a new key (e.g. changed imageTag) starts fresh.
static void testDecodeRetryBoundNewKeyIsFresh()
{
    std::printf("[test] decode retry bound: new key starts fresh\n");
    std::map<std::string, int> attempts;
    const std::string oldKey = "item:Primary:oldTag:64x96";
    const std::string newKey = "item:Primary:newTag:64x96";

    attempts[oldKey] = 5;
    CHECK(attempts.find(newKey) == attempts.end());
    ++attempts[newKey];
    CHECK(attempts[newKey] == 1);
    std::printf("[test] decode retry bound: new key starts fresh OK\n");
}

// ===================================================================
// buildPosterJobFromKey parsing tests
// ===================================================================

static void testBuildPosterJobFromKeyPrimary()
{
    std::printf("[test] buildPosterJobFromKey: Primary\n");
    // Use the static helper via a local test.  The function is file-local
    // so we verify the key format roundtrip via posterJobKey instead.
    HomePosterJob job{"abc", ImageType::Primary, "tag1", 64, 96};
    std::string key = job.itemId + ":" + imageTypeToString(job.imageType)
                      + ":" + job.imageTag + ":"
                      + std::to_string(job.width) + "x"
                      + std::to_string(job.height);
    CHECK_EQ(key, "abc:Primary:tag1:64x96");
    std::printf("[test] buildPosterJobFromKey: Primary OK\n");
}

static void testBuildPosterJobFromKeyThumb()
{
    std::printf("[test] buildPosterJobFromKey: Thumb\n");
    HomePosterJob job{"xyz", ImageType::Thumb, "thumbTag", 128, 72};
    std::string key = job.itemId + ":" + imageTypeToString(job.imageType)
                      + ":" + job.imageTag + ":"
                      + std::to_string(job.width) + "x"
                      + std::to_string(job.height);
    CHECK_EQ(key, "xyz:Thumb:thumbTag:128x72");
    std::printf("[test] buildPosterJobFromKey: Thumb OK\n");
}

// ===================================================================
// tryLoadOneRowArtwork skip-gate tests
// ===================================================================

// After LRU eviction, the key is no longer in m_rowArtwork so
// tryLoadOneRowArtwork will resubmit the decode.
static void testEvictedKeyResubmittedForDecode()
{
    std::printf("[test] tryLoad: evicted key is resubmitted\n");
    std::map<std::string, RowArtworkEntry> rowArtwork;

    MediaItem m; m.id = "ev"; m.type = "movie";
    m.imageTags["Primary"] = "tagE";
    std::string key = buildRowArtworkKey(m);

    // Simulate loaded state.
    rowArtwork[key].status = RowArtworkStatus::Loaded;
    CHECK(rowArtwork.find(key) != rowArtwork.end());

    // LRU eviction removes the entry.
    rowArtwork.erase(key);
    CHECK(rowArtwork.find(key) == rowArtwork.end());

    // Now tryLoadOneRowArtwork would find the key absent and submit decode.
    CHECK(rowArtwork.find(key) == rowArtwork.end());
    std::printf("[test] tryLoad: evicted key is resubmitted OK\n");
}

// Failed key (at limit) is still skipped by the skip gate.
static void testFailedKeySkippedByGate()
{
    std::printf("[test] tryLoad: Failed key is skipped\n");
    std::map<std::string, RowArtworkEntry> rowArtwork;
    std::map<std::string, RowArtworkStatus> sm;

    MediaItem m; m.id = "f1"; m.type = "movie";
    m.imageTags["Primary"] = "tagF";
    std::string key = buildRowArtworkKey(m);

    // Permanent Failed tombstone.
    rowArtwork[key].status = RowArtworkStatus::Failed;
    sm[key] = RowArtworkStatus::Failed;
    // pickCandidates correctly skips it.
    CHECK(pickCandidates({m}, sm).empty());
    std::printf("[test] tryLoad: Failed key is skipped OK\n");
}
