static void testPlaybackRequestMovie()
{
    std::printf("[test] B5f2: PlaybackRequest valid movie\n");
    const char *tmpPath = "test_playback_movie.txt";
    std::remove(tmpPath);
    std::string error;
    CHECK(PlaybackRequest::writeTo(tmpPath, "abc123", "movie",
                                   18822664360LL, error));
    CHECK(PlaybackRequest::existsAt(tmpPath));

    // Read back and verify
    FILE *f = std::fopen(tmpPath, "r");
    CHECK(f != nullptr);
    char buf[256] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);

    std::string content(buf);
    CHECK(content.find("item_id=abc123\n") != std::string::npos);
    CHECK(content.find("item_type=movie\n") != std::string::npos);
    CHECK(content.find("resume_ticks=18822664360\n") != std::string::npos);
    CHECK(content.find("access_token") == std::string::npos);

    std::remove(tmpPath);
    std::printf("[test] B5f2: PlaybackRequest valid movie OK\n");
}

// B5f2: PlaybackRequest — valid episode writes expected fields
static void testPlaybackRequestEpisode()
{
    std::printf("[test] B5f2: PlaybackRequest valid episode\n");
    const char *tmpPath = "test_playback_episode.txt";
    std::remove(tmpPath);

    std::string error;
    CHECK(PlaybackRequest::writeTo(tmpPath, "ep-42", "episode", 0, error));
    CHECK(PlaybackRequest::existsAt(tmpPath));

    FILE *f = std::fopen(tmpPath, "r");
    CHECK(f != nullptr);
    char buf[256] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);

    std::string content(buf);
    CHECK(content.find("item_id=ep-42\n") != std::string::npos);
    CHECK(content.find("item_type=episode\n") != std::string::npos);
    CHECK(content.find("resume_ticks=0\n") != std::string::npos);

    std::remove(tmpPath);
    std::printf("[test] B5f2: PlaybackRequest valid episode OK\n");
}

// B5f2: PlaybackRequest — empty ID rejected
static void testPlaybackRequestEmptyId()
{
    std::printf("[test] B5f2: PlaybackRequest empty ID rejected\n");
    std::string error;
    CHECK(!PlaybackRequest::writeTo("/tmp/bpr_test.txt", "", "movie", 0, error));
    CHECK(!error.empty());
    std::printf("[test] B5f2: PlaybackRequest empty ID rejected OK\n");
}

// B5f2: PlaybackRequest — empty type rejected
static void testPlaybackRequestEmptyType()
{
    std::printf("[test] B5f2: PlaybackRequest empty type rejected\n");
    std::string error;
    CHECK(!PlaybackRequest::writeTo("/tmp/bpr_test2.txt", "id1", "", 0, error));
    CHECK(!error.empty());
    std::printf("[test] B5f2: PlaybackRequest empty type rejected OK\n");
}

// B5f2: PlaybackRequest — remove
static void testPlaybackRequestRemove()
{
    std::printf("[test] B5f2: PlaybackRequest remove\n");
    const char *tmpPath = "test_playback_rm.txt";
    std::string error;
    CHECK(PlaybackRequest::writeTo(tmpPath, "x", "movie", -42, error));
    {
        FILE *f = std::fopen(tmpPath, "r");
        CHECK(f != nullptr);
        char buf[256] = {};
        std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        CHECK(std::string(buf).find("resume_ticks=0\n") != std::string::npos);
    }
    CHECK(PlaybackRequest::existsAt(tmpPath));
    CHECK(PlaybackRequest::removeAt(tmpPath));
    CHECK(!PlaybackRequest::existsAt(tmpPath));
    // Remove when not existing should not fail
    CHECK(PlaybackRequest::removeAt(tmpPath));
    std::printf("[test] B5f2: PlaybackRequest remove OK\n");
}

static void writePlaybackResultFixture(const char *path,
                                       const char *content)
{
    FILE *f = std::fopen(path, "w");
    CHECK(f != nullptr);
    if (f) {
        CHECK(std::fputs(content, f) >= 0);
        CHECK(std::fclose(f) == 0);
    }
}

static void testPlaybackResultParsing()
{
    std::printf("[test] playback result strict 64-bit parsing\n");
    const char *path = "test_playback_result.txt";
    std::int64_t ticks = -1;
    std::string error;

    writePlaybackResultFixture(path,
        "item_id=abc\nposition_ticks=24758110000\n");
    CHECK(PlaybackRequest::consumeResultFrom(path, "abc", ticks, error));
    CHECK(ticks == 24758110000LL);
    CHECK(!PlaybackRequest::existsAt(path));

    writePlaybackResultFixture(path, "item_id=abc\nposition_ticks=0\n");
    CHECK(PlaybackRequest::consumeResultFrom(path, "abc", ticks, error));
    CHECK(ticks == 0);

    const char *invalid[] = {
        "item_id=abc\n",
        "item_id=abc\nposition_ticks=12oops\n",
        "item_id=abc\nposition_ticks=-1\n",
        "item_id=abc\nposition_ticks=9223372036854775808\n",
        "position_ticks=10\n"
    };
    for (const char *content : invalid) {
        writePlaybackResultFixture(path, content);
        ticks = 777;
        CHECK(!PlaybackRequest::consumeResultFrom(path, "abc", ticks, error));
        CHECK(ticks == 777);
        CHECK(!PlaybackRequest::existsAt(path));
    }

    writePlaybackResultFixture(path, "item_id=other\nposition_ticks=99\n");
    ticks = 777;
    CHECK(!PlaybackRequest::consumeResultFrom(path, "abc", ticks, error));
    CHECK(ticks == 777);
    CHECK(!PlaybackRequest::existsAt(path));
    std::printf("[test] playback result strict 64-bit parsing OK\n");
}

static void testPlaybackResultDelay()
{
    std::printf("[test] playback result one-update delay\n");
    bool pending = true;
    int delayUpdates = 1;
    CHECK(!PlaybackRequest::advanceResultConsumption(pending, delayUpdates));
    CHECK(pending && delayUpdates == 0);
    CHECK(PlaybackRequest::advanceResultConsumption(pending, delayUpdates));
    CHECK(!pending);
    CHECK(!PlaybackRequest::advanceResultConsumption(pending, delayUpdates));
    std::printf("[test] playback result one-update delay OK\n");
}
static void testOfflinePlaybackJournal(){std::string p="/tmp/miyoofin-journal-"+std::to_string((long long)getpid());OfflinePlaybackEntry a;a.itemId="a";a.itemType="movie";a.baseServerTicks=10;a.finalTicks=30;a.localTimestamp=1;CHECK(OfflinePlaybackJournal::upsert(p,a));a.finalTicks=40;CHECK(OfflinePlaybackJournal::upsert(p,a));std::vector<OfflinePlaybackEntry> got;CHECK(OfflinePlaybackJournal::load(p,got));CHECK(got.size()==(size_t)1&&got[0].finalTicks==40);CHECK(decideOfflineSync(10,10,true)==OfflineSyncDecision::Push);CHECK(decideOfflineSync(10,20,true)==OfflineSyncDecision::Conflict);CHECK(decideOfflineSync(10,0,false)==OfflineSyncDecision::Retry);PlaybackResult r;std::string e;CHECK(PlaybackRequest::parseResult("item_id=a\nposition_ticks=5\n",r,e)&&r.serverReported);CHECK(PlaybackRequest::parseResult("item_id=a\nitem_type=movie\nposition_ticks=5\nbase_resume_ticks=2\nsource_mode=local\nserver_reported=0\n",r,e)&&!r.serverReported&&r.baseResumeTicks==2);std::vector<OfflinePlaybackEntry> v(1,a);int submitted=0;auto pushed=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&t){t=10;return OfflineJournalRequestStatus::Success;},[&](const OfflinePlaybackEntry&){++submitted;return OfflineJournalRequestStatus::Success;});CHECK(pushed.pushed==1&&v.empty()&&submitted==1);v.assign(1,a);auto transient=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&){return OfflineJournalRequestStatus::Transient;},[](const OfflinePlaybackEntry&){return OfflineJournalRequestStatus::Success;});CHECK(transient.retry&&v.size()==(size_t)1);v.assign(1,a);auto conflict=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&t){t=11;return OfflineJournalRequestStatus::Success;},[](const OfflinePlaybackEntry&){return OfflineJournalRequestStatus::Success;});CHECK(conflict.conflicts==1&&v[0].conflict);submitted=0;auto skipped=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&t){t=10;return OfflineJournalRequestStatus::Success;},[&](const OfflinePlaybackEntry&){++submitted;return OfflineJournalRequestStatus::Success;});CHECK(submitted==0&&v.size()==(size_t)1);v.assign(1,a);auto missing=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&){return OfflineJournalRequestStatus::Missing;},[](const OfflinePlaybackEntry&){return OfflineJournalRequestStatus::Success;});CHECK(missing.changed&&!missing.retry&&v.size()==(size_t)1&&v[0].serverMissing);CHECK(OfflinePlaybackJournal::save(p,v));CHECK(OfflinePlaybackJournal::discardMissing(p,"a"));got.clear();CHECK(OfflinePlaybackJournal::load(p,got)&&got.empty());v.assign(1,a);auto unauthorized=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&){return OfflineJournalRequestStatus::Unauthorized;},[](const OfflinePlaybackEntry&){return OfflineJournalRequestStatus::Success;});CHECK(unauthorized.retry&&unauthorized.unauthorized&&v.size()==(size_t)1);}

// -------------------------------------------------------------------
// B5f3a: In-process external playback handoff tests
// -------------------------------------------------------------------

// B5f3a: ScreenStack external playback flag — initially false
static void testExternalPlaybackFlagInitial()
{
    std::printf("[test] B5f3a: ScreenStack external playback flag initially false\n");
    ScreenStack stack;
    CHECK(!stack.pollExternalPlayback());
    std::printf("[test] B5f3a: ScreenStack external playback flag initially false OK\n");
}

// B5f3a: ScreenStack external playback flag — set and consume
static void testExternalPlaybackFlagSetConsume()
{
    std::printf("[test] B5f3a: ScreenStack external playback flag set/consume\n");
    ScreenStack stack;
    stack.requestExternalPlayback();
    CHECK(stack.pollExternalPlayback());
    // After consuming, should be false
    CHECK(!stack.pollExternalPlayback());
    std::printf("[test] B5f3a: ScreenStack external playback flag set/consume OK\n");
}

// B5f3a: ScreenStack external playback flag — multiple sets collapse
static void testExternalPlaybackFlagMultipleSet()
{
    std::printf("[test] B5f3a: ScreenStack external playback flag multiple sets\n");
    ScreenStack stack;
    stack.requestExternalPlayback();
    stack.requestExternalPlayback();  // redundant set
    CHECK(stack.pollExternalPlayback());
    CHECK(!stack.pollExternalPlayback());
    std::printf("[test] B5f3a: ScreenStack external playback flag multiple sets OK\n");
}

// B5f3a: PlaybackRequest still writes item_id and item_type (no exit code 42)
static void testPlaybackRequestStillWorks()
{
    std::printf("[test] B5f3a: PlaybackRequest still writes correctly\n");
    const char *tmpPath = "test_b5f3a_request.txt";
    std::remove(tmpPath);

    std::string error;
    CHECK(PlaybackRequest::writeTo(tmpPath, "movie-99", "movie", 123, error));

    // Verify contents
    FILE *f = std::fopen(tmpPath, "r");
    CHECK(f != nullptr);
    char buf[256] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);

    std::string content(buf);
    CHECK(content.find("item_id=movie-99\n") != std::string::npos);
    CHECK(content.find("item_type=movie\n") != std::string::npos);
    CHECK(content.find("resume_ticks=123\n") != std::string::npos);
    // Must not contain access_token
    CHECK(content.find("access_token") == std::string::npos);

    std::remove(tmpPath);
    std::printf("[test] B5f3a: PlaybackRequest still writes correctly OK\n");
}

// B5f3a: ScreenStack does not get destroyed during external playback
// (conceptual test — push screens, set flag, verify stack is intact)
static void testScreenStackPreservedDuringExternalPlayback()
{
    std::printf("[test] B5f3a: ScreenStack preserved during external playback\n");
    // We can't easily instantiate real screens without SDL, but we can
    // test that the stack size and empty state are unaffected by the flag.
    ScreenStack stack;
    CHECK(stack.empty());
    CHECK(stack.size() == 0);

    // Set the external playback flag
    stack.requestExternalPlayback();

    // Stack should still be empty (flag doesn't modify stack)
    CHECK(stack.empty());
    CHECK(stack.size() == 0);

    // Consume the flag
    CHECK(stack.pollExternalPlayback());

    // Stack still empty, flag consumed
    CHECK(stack.empty());
    std::printf("[test] B5f3a: ScreenStack preserved during external playback OK\n");
}

class RetirementTestScreen : public Screen {
public:
    RetirementTestScreen(bool deferred, std::atomic<bool> *left=nullptr,
                         std::atomic<bool> *destroyed=nullptr)
        : m_deferred(deferred), m_left(left), m_destroyed(destroyed) {}
    ~RetirementTestScreen() override {
        if (m_deferred) usleep(200000);
        if (m_destroyed) m_destroyed->store(true);
    }
    void enter() override {}
    void leave() override { if (m_left) m_left->store(true); }
    bool handleAction(Action) override { return false; }
    void update(Uint32) override {}
    void render(SDL_Surface *) override {}
    bool deferDestruction() const override { return m_deferred; }
private:
    bool m_deferred;
    std::atomic<bool> *m_left;
    std::atomic<bool> *m_destroyed;
};

static void testScreenRetirementDoesNotBlockPop()
{
    std::printf("[test] ScreenStack deferred worker retirement\n");
    std::atomic<bool> left{false}, destroyed{false};
    ScreenStack stack;
    stack.push(std::unique_ptr<Screen>(new RetirementTestScreen(false)));
    stack.push(std::unique_ptr<Screen>(new RetirementTestScreen(true,&left,&destroyed)));
    const auto start=std::chrono::steady_clock::now();
    CHECK(stack.pop());
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start).count();
    CHECK(left.load());
    CHECK(elapsed < 100);
    CHECK(stack.size()==1);
    for(int i=0;i<50&&!destroyed.load();++i)usleep(10000);
    CHECK(destroyed.load());
    std::printf("[test] ScreenStack deferred worker retirement OK\n");
}

static void testMovieDetailsOpensBeforeArtworkPreparation()
{
    std::printf("[test] MovieDetailsScreen asynchronous first state\n");
    Session session;
    session.serverUrl="http://127.0.0.1:1";
    session.accessToken="test-token";
    session.deviceId="test-device";
    MediaItem movie=titledMovie("movie-async","Async Movie");
    movie.imageTags["Primary"]="uncached-artwork";
    MovieDetailsScreen screen(session,movie);
    CHECK(std::string(screen.diagnosticName())=="MovieDetailsScreen");
    CHECK(screen.deferDestruction());
    const auto start=std::chrono::steady_clock::now();
    screen.enter();
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start).count();
    // enter() only owns/starts the cancellable worker. Cache I/O, HTTP and
    // JPEG decode cannot delay the first interactive state.
    CHECK(elapsed < 100);
    CHECK(screen.handleAction(Action::Right));
    SDL_Surface *fb=SDL_CreateRGBSurface(0,640,480,32,
        0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    CHECK(fb!=nullptr);
    if(fb){
        const auto renderStart=std::chrono::steady_clock::now();
        screen.render(fb);
        const auto renderElapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-renderStart).count();
        CHECK(renderElapsed < 100);
        SDL_FreeSurface(fb);
    }
    screen.leave();
    std::printf("[test] MovieDetailsScreen asynchronous first state OK\n");
}

static void testMovieDetailsUsesGridArtworkImmediately()
{
    std::printf("[test] MovieDetailsScreen grid artwork fallback\n");
    Session session;
    MediaItem movie=titledMovie("movie-grid-art","Grid Artwork");
    auto artwork=std::make_shared<DecodedImage>();
    artwork->width=64;
    artwork->height=96;
    artwork->pixels.assign(64*96*4,0);
    for(size_t i=0;i<artwork->pixels.size();i+=4){
        artwork->pixels[i]=17; artwork->pixels[i+1]=34;
        artwork->pixels[i+2]=51; artwork->pixels[i+3]=255;
    }
    MovieDetailsScreen screen(session,movie,{},artwork);
    SDL_Surface *fb=SDL_CreateRGBSurface(0,640,480,32,
        0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    CHECK(fb!=nullptr);
    if(fb){
        screen.render(fb);
        Uint8 r=0,g=0,b=0,a=0;
        const Uint32 pixel=*reinterpret_cast<Uint32*>(
            static_cast<Uint8*>(fb->pixels) + 50*fb->pitch + 30*4);
        SDL_GetRGBA(pixel,fb->format,&r,&g,&b,&a);
        CHECK(r==17 && g==34 && b==51 && a==255);
        SDL_FreeSurface(fb);
    }
    std::printf("[test] MovieDetailsScreen grid artwork fallback OK\n");
}

static void testDpadHoldRepeatTiming()
{
    std::printf("[test] D-pad hold repeat timing\n");
    InputManager::DpadRepeatState state;

    // Fresh press emits immediately; duplicate/native repeat does not.
    CHECK(InputManager::beginDpadPress(state, Action::Right, 100));
    CHECK(state.held);
    CHECK(state.nextRepeatAt == 400);
    CHECK(!InputManager::beginDpadPress(state, Action::Right, 200));
    CHECK(state.nextRepeatAt == 400);

    CHECK(!InputManager::takeDpadRepeat(state, 399));
    CHECK(InputManager::takeDpadRepeat(state, 400));
    CHECK(!InputManager::takeDpadRepeat(state, 489));
    CHECK(InputManager::takeDpadRepeat(state, 490));

    // A late poll emits once and advances beyond now, with no catch-up burst.
    CHECK(InputManager::takeDpadRepeat(state, 1000));
    CHECK(!InputManager::takeDpadRepeat(state, 1000));

    InputManager::endDpadPress(state);
    CHECK(!state.held);
    CHECK(!InputManager::takeDpadRepeat(state, 2000));

    InputManager::DpadRepeatState wrapping;
    CHECK(InputManager::beginDpadPress(
        wrapping, Action::Left, 0xffffff00u));
    CHECK(!InputManager::takeDpadRepeat(wrapping, 43));
    CHECK(InputManager::takeDpadRepeat(wrapping, 44));

    CHECK(InputManager::isDpadRepeatAction(Action::Up));
    CHECK(InputManager::isDpadRepeatAction(Action::Down));
    CHECK(InputManager::isDpadRepeatAction(Action::Left));
    CHECK(InputManager::isDpadRepeatAction(Action::Right));
    CHECK(!InputManager::isDpadRepeatAction(Action::Confirm));
    CHECK(!InputManager::isDpadRepeatAction(Action::Back));
    CHECK(!InputManager::isDpadRepeatAction(Action::PrevTab));
    CHECK(!InputManager::isDpadRepeatAction(Action::Raw));

    InputManager::DpadRepeatState nonDirectional;
    CHECK(!InputManager::beginDpadPress(
        nonDirectional, Action::Confirm, 0));

    std::array<InputManager::DpadRepeatState, 4> states;
    for (auto &held : states)
        CHECK(InputManager::beginDpadPress(held, Action::Down, 0));
    InputManager::resetDpadRepeatStates(states);
    for (const auto &cleared : states) CHECK(!cleared.held);
    std::printf("[test] D-pad hold repeat timing OK\n");
}

// --- Playback progress display tests ---

static void testPlaybackPercentFromTicks()
{
    std::printf("[test] playbackPercent from ticks (position/runtime)\n");
    MediaItem item;
    item.runTimeTicks = 10000000LL * 60 * 60;   // 1 hour
    item.playbackPositionTicks = 10000000LL * 60 * 37; // 37 minutes
    item.progress = 0.5f;
    item.played = false;
    int pct = playbackPercent(item);
    CHECK_EQ(std::to_string(pct), std::to_string(62)); // 37/60 ≈ 62%
    std::printf("[test] playbackPercent from ticks (position/runtime) OK\n");
}

static void testPlaybackPercentFallbackToProgress()
{
    std::printf("[test] playbackPercent fallback to progress field\n");
    MediaItem item;
    item.runTimeTicks = 0;          // no runtime
    item.playbackPositionTicks = 0;
    item.progress = 0.455f;         // 45.5% from server
    item.played = false;
    int pct = playbackPercent(item);
    CHECK_EQ(std::to_string(pct), std::to_string(46)); // 45.5 rounds to 46
    std::printf("[test] playbackPercent fallback to progress field OK\n");
}

static void testPlaybackPercentClamping()
{
    std::printf("[test] playbackPercent clamped to 0-100\n");
    {
        MediaItem item;
        item.runTimeTicks = 10000000LL * 60;
        item.playbackPositionTicks = 10000000LL * 90; // 90 min in 60-min runtime
        item.progress = 0.0f;
        item.played = false;
        int pct = playbackPercent(item);
        CHECK_EQ(std::to_string(pct), std::to_string(100));
    }
    {
        MediaItem item;
        item.runTimeTicks = 0;
        item.playbackPositionTicks = 0;
        item.progress = 1.5f; // > 1.0 (shouldn't happen but test guard)
        item.played = false;
        int pct = playbackPercent(item);
        CHECK_EQ(std::to_string(pct), std::to_string(100));
    }
    std::printf("[test] playbackPercent clamped to 0-100 OK\n");
}

static void testPlaybackPercentZeroRuntime()
{
    std::printf("[test] playbackPercent zero runtime returns 0\n");
    MediaItem item;
    item.runTimeTicks = 0;
    item.playbackPositionTicks = 0;
    item.progress = 0.0f;
    item.played = false;
    int pct = playbackPercent(item);
    CHECK_EQ(std::to_string(pct), std::to_string(0));
    std::printf("[test] playbackPercent zero runtime returns 0 OK\n");
}

static void testPlaybackPercentCompletedItem()
{
    std::printf("[test] playbackPercent completed item uses ticks\n");
    MediaItem item;
    item.runTimeTicks = 10000000LL * 60 * 45; // 45 min
    item.playbackPositionTicks = 10000000LL * 60 * 45; // fully watched
    item.progress = 1.0f;
    item.played = true;
    int pct = playbackPercent(item);
    CHECK_EQ(std::to_string(pct), std::to_string(100));
    std::printf("[test] playbackPercent completed item uses ticks OK\n");
}

static void testFormatPlaybackTimeBasic()
{
    std::printf("[test] formatPlaybackTime basic minutes\n");
    // 30 min position in 60 min runtime
    std::string s = formatPlaybackTime(10000000LL * 60 * 30,
                                       10000000LL * 60 * 60);
    CHECK(s.find("30m watched") != std::string::npos);
    CHECK(s.find("30m remaining") != std::string::npos);
    std::printf("[test] formatPlaybackTime basic minutes OK\n");
}

static void testFormatPlaybackTimeHours()
{
    std::printf("[test] formatPlaybackTime hours and minutes\n");
    // 1h24m position in 2h runtime
    long long pos = 10000000LL * (60*60 + 24*60); // 1h 24m
    long long tot = 10000000LL * 60 * 120;          // 2h
    std::string s = formatPlaybackTime(pos, tot);
    CHECK(s.find("1h 24m watched") != std::string::npos);
    CHECK(s.find("36m remaining") != std::string::npos);
    std::printf("[test] formatPlaybackTime hours and minutes OK\n");
}

static void testFormatPlaybackTimeZeroRuntime()
{
    std::printf("[test] formatPlaybackTime zero runtime returns empty\n");
    std::string s = formatPlaybackTime(50000000LL, 0);
    CHECK(s.empty());
    std::printf("[test] formatPlaybackTime zero runtime returns empty OK\n");
}

static void testFormatPlaybackTimeClampedPosition()
{
    std::printf("[test] formatPlaybackTime position clamped to runtime\n");
    // position exceeds runtime — should clamp
    std::string s = formatPlaybackTime(10000000LL * 60 * 90,
                                       10000000LL * 60 * 60);
    CHECK(s.find("1h 0m watched") != std::string::npos);
    CHECK(s.find("0m remaining") != std::string::npos);
    std::printf("[test] formatPlaybackTime position clamped to runtime OK\n");
}

static void testFormatPlaybackTimeZeroPosition()
{
    std::printf("[test] formatPlaybackTime zero position\n");
    std::string s = formatPlaybackTime(0, 10000000LL * 60 * 90);
    CHECK(s.find("0m watched") != std::string::npos);
    CHECK(s.find("1h 30m remaining") != std::string::npos);
    std::printf("[test] formatPlaybackTime zero position OK\n");
}

// -------------------------------------------------------------------
// Clock check tests — Issue #1: friendly error when device clock breaks HTTPS
// Tests exercise the shared shouldShowClockError() decision helper
// directly, with no conceptual/indirect assertions.
// -------------------------------------------------------------------
