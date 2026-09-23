static void testServerEntryKeyboardCaps()
{
    std::printf("[test] Server Entry keyboard caps and navigation\n");
    ServerEntryScreen screen;
    CHECK_EQ(screen.entryText(), "");
    CHECK(!screen.capsEnabled());
    CHECK(screen.keyboardSelectionValid());

    // Numeric characters remain unchanged in caps mode.
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.entryText(), "1");
    screen.handleAction(Action::PrevPage);  // L2
    CHECK(screen.capsEnabled());
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.entryText(), "11");

    // The first alphabetic key is q; L2 changes only its case.
    screen.handleAction(Action::Down);
    CHECK(screen.keyboardSelectionValid());
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.entryText(), "11Q");
    screen.handleAction(Action::PrevPage);
    CHECK(!screen.capsEnabled());
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.entryText(), "11Qq");

    screen.handleAction(Action::Search);
    CHECK_EQ(screen.entryText(), "");
    screen.handleAction(Action::Confirm);
    screen.handleAction(Action::Back);
    CHECK_EQ(screen.entryText(), "");

    // Every navigation transition remains on a key, including the action row.
    for (int i = 0; i < 8; ++i) {
        screen.handleAction(Action::Down);
        CHECK(screen.keyboardSelectionValid());
    }
    for (int i = 0; i < 12; ++i) {
        screen.handleAction(Action::Right);
        CHECK(screen.keyboardSelectionValid());
    }
    for (int i = 0; i < 12; ++i) {
        screen.handleAction(Action::Left);
        CHECK(screen.keyboardSelectionValid());
    }

    // Leaving and each new text-entry session start in lowercase mode.
    screen.handleAction(Action::PrevPage);
    CHECK(screen.capsEnabled());
    screen.leave();
    CHECK(!screen.capsEnabled());
    ServerEntryScreen reopened;
    CHECK(!reopened.capsEnabled());
    std::printf("[test] Server Entry keyboard caps OK\n");
}

static void testSettingsAddressEntryCancel()
{
    std::printf("[test] Settings address entry cancel\n");
    Session session;
    session.serverUrl = "http://192.168.1.212:8096";
    session.localServerUrl = "http://192.168.1.213:8096";
    session.publicServerUrl = "https://public.example.com";
    session.serverId = "server-id";
    const Session before = session;

    // Cancelling a new Public Address does not begin verification or alter
    // the transaction's pre-existing Session state.
    ServerEntryScreen newPublic("", "", session.serverId, false, true);
    newPublic.handleAction(Action::Back);
    CHECK(newPublic.addressEntryCancelled());
    CHECK(!newPublic.connected());
    CHECK(!newPublic.finished());
    CHECK_EQ(session.serverUrl, before.serverUrl);
    CHECK_EQ(session.localServerUrl, before.localServerUrl);
    CHECK_EQ(session.publicServerUrl, before.publicServerUrl);
    CHECK_EQ(session.serverId, before.serverId);

    // Editing an existing public address remains equally non-mutating.
    ServerEntryScreen existingPublic(session.publicServerUrl, "", session.serverId,
                                     false, true);
    existingPublic.handleAction(Action::Back);
    CHECK(existingPublic.addressEntryCancelled());
    CHECK_EQ(existingPublic.entryText(), before.publicServerUrl);
    CHECK_EQ(session.publicServerUrl, before.publicServerUrl);

    ServerEntryScreen local(session.localServerUrl, "", session.serverId, true);
    local.handleAction(Action::Back);
    CHECK(local.addressEntryCancelled());
    CHECK_EQ(local.entryText(), before.localServerUrl);
    CHECK_EQ(session.localServerUrl, before.localServerUrl);

    // On the actual Settings stack B returns to the prior Settings screen.
    ScreenStack stack;
    stack.push(std::make_unique<ServerEntryScreen>());
    stack.push(std::make_unique<ServerEntryScreen>("", "", session.serverId,
                                                   false, true));
    CHECK(stack.size() == 2);
    stack.top()->handleAction(Action::Back);
    CHECK(stack.size() == 1);
    CHECK_EQ(session.serverUrl, before.serverUrl);
    CHECK_EQ(session.localServerUrl, before.localServerUrl);
    CHECK_EQ(session.publicServerUrl, before.publicServerUrl);
    CHECK_EQ(session.serverId, before.serverId);
}

static void testLoginKeyboardCaps()
{
    std::printf("[test] Login keyboard caps and fields\n");
    LoginScreen screen("https://server", "Server", "device");
    CHECK(!screen.capsEnabled());
    screen.handleAction(Action::Down);
    CHECK(screen.keyboardSelectionValid());
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.username(), "1");
    screen.handleAction(Action::PrevPage);
    CHECK(screen.capsEnabled());
    screen.handleAction(Action::Down);
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.username(), "1Q");
    screen.handleAction(Action::Up);
    screen.handleAction(Action::Up);
    screen.handleAction(Action::Right);
    screen.handleAction(Action::Down);
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.password(), "1");
    screen.handleAction(Action::Down);
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.password(), "1Q");
    screen.leave();
    CHECK(!screen.capsEnabled());
    std::printf("[test] Login keyboard caps and fields OK\n");
}

static void testOnScreenKeyboardGrid()
{
    std::printf("[test] OnScreenKeyboard 10-column grid geometry\n");
    OnScreenKeyboard kb;
    CHECK(OnScreenKeyboard::GRID_COLS == 10);
    CHECK(OnScreenKeyboard::NUM_ROWS == 6);

    // Count keys per row by tracking position changes
    int totalKeys = 0;
    for (int row = 0; row < 6; ++row) {
        kb.reset();
        for (int i = 0; i < row; ++i) kb.handleAction(Action::Down);
        for (int i = 0; i < 12; ++i) kb.handleAction(Action::Left); // go to leftmost
        int n = 0;
        int prevCol = -1;
        for (int i = 0; i < 12; ++i) {
            const auto *k = kb.activeKey();
            if (k && k->col != prevCol) {
                n++;
                prevCol = k->col;
            }
            kb.handleAction(Action::Right);
        }
        if (row < 5) CHECK(n == 10); else CHECK(n == 5);
        totalKeys += n;
    }
    CHECK(totalKeys == 55);

    // Action row: five 2-column keys
    kb.reset();
    for (int i = 0; i < 6; ++i) kb.handleAction(Action::Down);
    for (int i = 0; i < 12; ++i) kb.handleAction(Action::Left);
    const OnScreenKeyboard::Key *k = kb.activeKey();
    CHECK(k != nullptr);
    CHECK(k->colSpan == 2);
    for (int i = 0; i < 4; ++i) {
        kb.handleAction(Action::Right);
        k = kb.activeKey();
        CHECK(k != nullptr);
        CHECK(k->colSpan == 2);
    }
    CHECK(k->col == 8);
    std::printf("[test] OnScreenKeyboard 10-column grid geometry OK\n");
}

static void testOnScreenKeyboardSpace()
{
    std::printf("[test] OnScreenKeyboard space key\n");
    OnScreenKeyboard kb;
    for (int i = 0; i < 6; ++i) kb.handleAction(Action::Down);
    for (int i = 0; i < 12; ++i) kb.handleAction(Action::Left);
    const OnScreenKeyboard::Key *k = kb.activeKey();
    CHECK(k != nullptr);
    CHECK(k->ch == ' ');
    CHECK(k->row == 5);
    CHECK(k->col == 0);
    CHECK(kb.processKey(*k) == ' ');
    CHECK_EQ(kb.keyLabel(*k), "SPACE");
    std::printf("[test] OnScreenKeyboard space key OK\n");
}

static void testServerEntrySpace()
{
    std::printf("[test] ServerEntryScreen space insertion\n");
    ServerEntryScreen screen;
    CHECK_EQ(screen.entryText(), "");
    for (int i = 0; i < 6; ++i) screen.handleAction(Action::Down);
    for (int i = 0; i < 12; ++i) screen.handleAction(Action::Left);
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.entryText(), " ");
    for (int i = 0; i < 6; ++i) screen.handleAction(Action::Up);
    for (int i = 0; i < 12; ++i) screen.handleAction(Action::Left);
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.entryText(), " 1");
    screen.handleAction(Action::Back);
    CHECK_EQ(screen.entryText(), " ");
    screen.handleAction(Action::Search);
    CHECK_EQ(screen.entryText(), "");
    std::printf("[test] ServerEntryScreen space insertion OK\n");
}

static void testLoginUsernameSpace()
{
    std::printf("[test] LoginScreen username space insertion\n");
    LoginScreen screen("https://server", "Server", "device");
    screen.handleAction(Action::Down);
    for (int i = 0; i < 6; ++i) screen.handleAction(Action::Down);
    for (int i = 0; i < 12; ++i) screen.handleAction(Action::Left);
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.username(), " ");
    for (int i = 0; i < 5; ++i) screen.handleAction(Action::Up);
    for (int i = 0; i < 12; ++i) screen.handleAction(Action::Left);
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.username(), " 1");
    std::printf("[test] LoginScreen username space insertion OK\n");
}

static void testLoginPasswordSpace()
{
    std::printf("[test] LoginScreen password space insertion\n");
    LoginScreen screen("https://server", "Server", "device");
    screen.handleAction(Action::Right);
    screen.handleAction(Action::Down);
    for (int i = 0; i < 6; ++i) screen.handleAction(Action::Down);
    for (int i = 0; i < 12; ++i) screen.handleAction(Action::Left);
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.password(), " ");
    CHECK(screen.password().size() == (size_t)1);
    CHECK(screen.password()[0] == ' ');
    std::printf("[test] LoginScreen password space insertion OK\n");
}

static void testSharedKeyboardLayoutConsistency()
{
    std::printf("[test] Shared keyboard layout consistency\n");
    OnScreenKeyboard kbA(OnScreenKeyboard::Config{"[DONE]", 104});
    OnScreenKeyboard kbB(OnScreenKeyboard::Config{"[SIGN IN]", 134});

    auto getChar = [](OnScreenKeyboard &kb, int r, int c) -> char {
        kb.reset();
        for (int i = 0; i < r; ++i) kb.handleAction(Action::Down);
        for (int i = 0; i < 12; ++i) kb.handleAction(Action::Left);
        for (int i = 0; i < c; ++i) kb.handleAction(Action::Right);
        const auto *k = kb.activeKey();
        return k ? k->ch : '\0';
    };
    for (int row = 0; row < 5; ++row)
        for (int col = 0; col < 10; ++col)
            CHECK(getChar(kbA, row, col) == getChar(kbB, row, col));

    // Navigate to submit key on action row (3rd Right from left = SUBMIT)
    // Verify configs are different
    {
        OnScreenKeyboard::Config cA{"[DONE]", 104};
        OnScreenKeyboard::Config cB{"[SIGN IN]", 134};
        CHECK(std::string(cA.submitLabel) == "[DONE]");
        CHECK(std::string(cB.submitLabel) == "[SIGN IN]");
    }

    auto getSubmitInfo = [](OnScreenKeyboard &kb) -> std::pair<char, std::string> {
        kb.reset();
        for (int i = 0; i < 6; ++i) kb.handleAction(Action::Down);
        for (int i = 0; i < 12; ++i) kb.handleAction(Action::Left);
        for (int i = 0; i < 3; ++i) kb.handleAction(Action::Right); // 3rd key
        const auto *k = kb.activeKey();
        return k ? std::make_pair(k->ch, std::string(k->label))
                 : std::make_pair('\0', std::string());
    };
    auto subA = getSubmitInfo(kbA);
    auto subB = getSubmitInfo(kbB);
    CHECK(subA.first == OnScreenKeyboard::KEY_SUBMIT);
    CHECK(subB.first == OnScreenKeyboard::KEY_SUBMIT);
    // Labels differ between screens
    CHECK(subA.second != subB.second);

    // Other action labels are shared
    auto getActionLabel = [](OnScreenKeyboard &kb, int keyIdx) -> std::string {
        kb.reset();
        for (int i = 0; i < 6; ++i) kb.handleAction(Action::Down);
        for (int i = 0; i < 12; ++i) kb.handleAction(Action::Left);
        for (int i = 0; i < keyIdx; ++i) kb.handleAction(Action::Right);
        const auto *k = kb.activeKey();
        return k ? k->label : "";
    };
    CHECK(getActionLabel(kbA, 0) == getActionLabel(kbB, 0));
    CHECK(getActionLabel(kbA, 1) == getActionLabel(kbB, 1));
    CHECK(getActionLabel(kbA, 2) == getActionLabel(kbB, 2));
    CHECK(getActionLabel(kbA, 4) == getActionLabel(kbB, 4));

    std::printf("[test] Shared keyboard layout consistency OK\n");
}

static void testKeyboardVerticalNavActionRow()
{
    std::printf("[test] Keyboard vertical navigation to action row\n");
    OnScreenKeyboard kb;
    const OnScreenKeyboard::Key *k;

    // Row 0 col 0 (center 0.5) → SPACE (center 1.0)
    kb.reset();
    for (int i = 0; i < 6; ++i) kb.handleAction(Action::Down);
    k = kb.activeKey();
    CHECK(k != nullptr);
    CHECK(k->row == 5);
    CHECK(k->col == 0);

    // Row 0 col 9 (center 9.5) → CANCEL (center 9.0)
    kb.reset();
    for (int i = 0; i < 9; ++i) kb.handleAction(Action::Right);
    for (int i = 0; i < 6; ++i) kb.handleAction(Action::Down);
    k = kb.activeKey();
    CHECK(k != nullptr);
    CHECK(k->col == 8);

    // DEL on action row → Up lands near col 2–3 on row 4
    kb.reset();
    for (int i = 0; i < 6; ++i) kb.handleAction(Action::Down);
    for (int i = 0; i < 12; ++i) kb.handleAction(Action::Left);
    kb.handleAction(Action::Right); // DEL at col 2
    k = kb.activeKey();
    CHECK(k != nullptr);
    CHECK(k->col == 2);
    kb.handleAction(Action::Up);
    k = kb.activeKey();
    CHECK(k != nullptr);
    CHECK(k->row == 4);
    CHECK(k->col == 2 || k->col == 3);

    // Up at row 0 stays at row 0
    kb.reset();
    CHECK(kb.selectionRow() == 0);
    kb.handleAction(Action::Up);
    CHECK(kb.selectionRow() == 0);

    // Down at row 5 stays at row 5
    for (int i = 0; i < 6; ++i) kb.handleAction(Action::Down);
    CHECK(kb.selectionRow() == 5);
    kb.handleAction(Action::Down);
    CHECK(kb.selectionRow() == 5);

    std::printf("[test] Keyboard vertical navigation to action row OK\n");
}

static void testUiDiagnostics()
{
    std::printf("[test] UI diagnostics watchdog/ring\n");
    UiDiagnostics::Watchdog w; uint64_t duration=0; w.seen=1000;
    CHECK(w.poll(1100,1200,false,duration)==0);
    w.seen=1000; CHECK(w.poll(1000,1501,false,duration)==1);
    CHECK(w.poll(1000,1700,false,duration)==0);
    CHECK(w.poll(1800,1850,false,duration)==2 && duration==850);
    w.seen=1000; CHECK(w.poll(1000,1600,true,duration)==0);
    UiDiagnostics d; for(size_t i=0;i<UiDiagnostics::EVENT_CAPACITY+2;i++) d.event(("event"+std::to_string(i)).c_str());
    const auto events=d.recentEvents();
    CHECK(events.size()==UiDiagnostics::EVENT_CAPACITY && events.front()=="event2");
    { UiDiagnostics::Scope scope("test-fast-scope"); }

    const std::string path = "/tmp/miyoofin-ui-diagnostics-"
        + std::to_string(static_cast<long long>(getpid())) + ".log";
    std::remove(path.c_str());
    d.start(path);
    d.log("[CatalogDb] diagnostic test line");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    d.stop();
    const std::string persisted = readTestBytes(path);
    CHECK(persisted.find("[CatalogDb] diagnostic test line") != std::string::npos);
    std::remove(path.c_str());
    std::printf("[test] UI diagnostics watchdog/ring OK\n");
}

static void testLoginBackRequestsServerEntry()
{
    std::printf("[test] LoginScreen Back requests server entry\n");
    LoginScreen screen("https://server", "Server", "device");
    CHECK(!screen.wantsServerEntry());
    // Field mode: Back must not be swallowed — the stack holds only this
    // screen, so App pushes a fresh ServerEntryScreen with the URL carried
    // back for editing (see App:: LoginScreen back -> ServerEntry).
    CHECK(screen.handleAction(Action::Back));
    CHECK(screen.wantsServerEntry());
    CHECK(!screen.finished());
    std::printf("[test] LoginScreen Back requests server entry OK\n");
}

static void testLoginKeyboardBackDeletes()
{
    std::printf("[test] LoginScreen keyboard Back deletes, not exits\n");
    LoginScreen screen("https://server", "Server", "device");
    screen.handleAction(Action::Down);
    screen.handleAction(Action::Confirm);
    CHECK_EQ(screen.username(), "1");
    CHECK(!screen.wantsServerEntry());
    // Keyboard mode: Back is delete-character (mirrors the on-screen B hint).
    screen.handleAction(Action::Back);
    CHECK_EQ(screen.username(), "");
    CHECK(!screen.wantsServerEntry());
    std::printf("[test] LoginScreen keyboard Back deletes, not exits OK\n");
}

// Thread-restart guard audit: assigning over a joinable std::thread calls
// std::terminate() (SIGABRT, "exited unexpectedly (134)" on a second
// Sign In / Connect). Every `Member = std::thread(` site in the audited
// translation units below must carry a guard in the same-function
// neighbourhood before the assignment — one of:
//   - restart: `M.joinable()` ... `M.join()` for the same member M (the
//     join reclaims an already-finished worker; a still-running worker is
//     refused via an early return instead of blocking the UI thread), or
//   - refuse: `if (... M.joinable() ...) ... return` (a joinable worker
//     refuses the new attempt, so the assignment never runs joinable), or
//   - gate-once: `if (!M.joinable()) M = std::thread(` (long-lived workers
//     started exactly once; the target is never joinable here).
// Matching runs on comment-stripped token text (string literals preserved,
// so a URL cannot end the strip early), so explanatory comments mentioning
// joinable/join can never satisfy the check. Single-shot workers (one
// assignment per owner lifetime, joined in leave()/dtor) are covered by the
// explicit allowlist below — each row pins the member plus its proof token —
// never by silence. A new `= std::thread(` site in any audited file that matches
// none of these fails the test, and a new TU that spawns threads must be
// added to kThreadAuditFiles (the audit scans listed files; it cannot
// discover files it is not told about). Member-init (`: m_x(...)`) and
// emplace_back constructions are single-construction by language semantics
// (never a re-assignment) and out of scope here.
static const std::size_t kThreadGuardRawWindow = 4000;

struct ThreadAuditAllow {
    const char *file;
    const char *member;
};

static const char *const kThreadAuditFiles[] = {
    "src/app/AppPlayback.cpp",
    "src/app/AppSession.cpp",
    "src/diagnostics/PerformanceTelemetryService.cpp",
    "src/diagnostics/UiDiagnostics.cpp",
    "src/download/DownloadManager.cpp",
    "src/library/LibrarySyncEvents.cpp",
    "src/ui/screens/AuthCheckScreen.cpp",
    "src/ui/screens/ConnectScreen.cpp",
    "src/ui/screens/EpisodeBrowserArtwork.cpp",
    "src/ui/screens/EpisodeBrowserData.cpp",
    "src/ui/screens/HomeScreen.cpp",
    "src/ui/screens/HomeScreenRefresh.cpp",
    "src/ui/screens/HomeScreenSync.cpp",
    "src/ui/screens/LoginScreen.cpp",
    "src/ui/screens/MovieDetailsScreen.cpp",
    "src/ui/screens/SeriesScreenWorker.cpp",
    "src/ui/screens/ServerEntryScreen.cpp",
    "src/update/UpdateManager.cpp",
};

// Single-shot workers: exactly one assignment per owner lifetime, joined
// in leave()/dtor paths — no restart guard applies.
static const ThreadAuditAllow kThreadSingleShot[] = {
    {"src/ui/screens/AuthCheckScreen.cpp", "m_checkThread"},
    {"src/ui/screens/MovieDetailsScreen.cpp", "m_prepareThread"},
    {"src/ui/screens/HomeScreen.cpp", "m_hierarchyThread"},
    {"src/ui/screens/HomeScreen.cpp", "m_decodeThread"},
    {"src/app/AppSession.cpp", "m_savedValidationThread"},
    {"src/download/DownloadManager.cpp", "m_thread"},
    {"src/download/DownloadManager.cpp", "m_planThread"},
    {"src/download/DownloadManager.cpp", "m_reconcileThread"},
};

// stop()-then-start: the assigning function calls stop() first and stop()
// joins the worker (PerformanceTelemetry::start).
static const ThreadAuditAllow kThreadStopDelegated[] = {
    {"src/diagnostics/PerformanceTelemetryService.cpp", "m_serviceThread"},
};

static bool threadAuditAllowed(const ThreadAuditAllow *list, std::size_t n,
                               const std::string &file,
                               const std::string &member)
{
    for (std::size_t i = 0; i < n; ++i)
        if (file == list[i].file && member == list[i].member) return true;
    return false;
}

static std::size_t threadTokenCount(const std::string &hay,
                                    const std::string &token)
{
    std::size_t count = 0, pos = 0;
    while ((pos = hay.find(token, pos)) != std::string::npos) {
        ++count;
        pos += token.size();
    }
    return count;
}

// Token-space guard probe over `seg` (comment-stripped token text ending at
// the `M = std::thread(` assignment) for member `member`.
static bool threadSiteGuarded(const std::string &seg,
                              const std::string &member)
{
    // Gate-once: `if (!M.joinable()) M = std::thread(` (seg ends at the
    // assignment, so the token tail is `...M=`).
    const std::string gate = "if(!" + member + ".joinable())" + member + "=";
    if (seg.size() >= gate.size() &&
        seg.compare(seg.size() - gate.size(), gate.size(), gate) == 0)
        return true;
    // Restart: `M.joinable()` ... `M.join()` in order before the assign.
    const std::string joinable = member + ".joinable()";
    const std::string joined = member + ".join()";
    const std::size_t seen = seg.find(joinable);
    if (seen == std::string::npos) return false;
    if (seg.find(joined, seen) != std::string::npos) return true;
    // Refuse: `if (... M.joinable() ...` with a return before any `;`.
    const std::size_t iff = seg.rfind("if(", seen);
    if (iff == std::string::npos) return false;
    const std::size_t semi = seg.find(";", iff);
    if (semi != std::string::npos && semi < seen) return false;
    const std::size_t ret = seg.find("return", seen);
    if (ret == std::string::npos) return false;
    const std::size_t semi2 = seg.find(";", seen);
    return semi2 == std::string::npos || semi2 > ret;
}

static void auditThreadFile(const char *path)
{
    const std::string raw = readTestBytes(path);
    CHECK(!raw.empty());
    if (raw.empty()) return;
    // Comment-stripped with newlines preserved, so raw offsets stay stable
    // and the guard window can stop at the enclosing function's end.
    const std::string stripped = stripSourceComments(raw);
    const std::string hay = sourceTokenString(stripped);
    const std::string file = path;
    static const std::string kSpawn = "std::thread(";
    std::size_t rawPos = 0;
    while ((rawPos = stripped.find(kSpawn, rawPos)) != std::string::npos) {
        // Walk left over whitespace to `=` and on to the member name, so
        // `M=std::thread(`, `M = std::thread(`, and reflowed variants all
        // resolve to the assigned member.
        std::size_t p = rawPos;
        while (p > 0 && (stripped[p - 1] == ' ' || stripped[p - 1] == '\t' ||
                         stripped[p - 1] == '\r' || stripped[p - 1] == '\n'))
            --p;
        bool shaped = (p > 0 && stripped[p - 1] == '=');
        std::string member;
        if (shaped) {
            --p;
            while (p > 0 && (stripped[p - 1] == ' ' || stripped[p - 1] == '\t' ||
                             stripped[p - 1] == '\r' || stripped[p - 1] == '\n'))
                --p;
            const std::size_t end = p;
            while (p > 0 && (std::isalnum(
                                 static_cast<unsigned char>(stripped[p - 1])) ||
                             stripped[p - 1] == '_'))
                --p;
            member = stripped.substr(p, end - p);
            shaped = !member.empty();
        }
        if (!shaped) {
            std::printf("  FAIL %s: non-assignment std::thread( site "
                        "(needs explicit handling)\n", path);
            CHECK(false);
            rawPos += kSpawn.size();
            continue;
        }
        const std::string assignTok = member + "=std::thread(";
        bool pass = false;
        if (threadAuditAllowed(
                       kThreadSingleShot,
                       sizeof(kThreadSingleShot) / sizeof(kThreadSingleShot[0]),
                       file, member)) {
            pass = threadTokenCount(hay, assignTok) == 1u &&
                   hay.find(member + ".join()") != std::string::npos;
        } else {
            // Same-function neighbourhood: back to the previous top-level
            // `}` (a column-0 close ends the enclosing function), capped at
            // kThreadGuardRawWindow chars so one-line functions cannot
            // borrow a guard from an unrelated earlier function.
            std::size_t winStart =
                (rawPos > kThreadGuardRawWindow) ? rawPos - kThreadGuardRawWindow
                                                 : 0;
            const std::size_t close = stripped.rfind("\n}", rawPos);
            if (close != std::string::npos && close + 2 > winStart)
                winStart = close + 2;
            const std::string seg = sourceTokenString(
                stripped.substr(winStart, rawPos - winStart));
            if (threadAuditAllowed(kThreadStopDelegated,
                                   sizeof(kThreadStopDelegated) /
                                       sizeof(kThreadStopDelegated[0]),
                                   file, member)) {
                // Delegation proof spans the whole assigning function head
                // (stop() runs at its top, far above the assignment), while
                // the joinable()/join() proof lives TU-wide in stop().
                const std::size_t fnStart =
                    (close == std::string::npos) ? 0 : close + 2;
                const std::string fnHead = sourceTokenString(
                    stripped.substr(fnStart, rawPos - fnStart));
                pass = fnHead.find("stop()") != std::string::npos &&
                       hay.find(member + ".joinable()") != std::string::npos &&
                       hay.find(member + ".join()") != std::string::npos;
            } else {
                pass = threadSiteGuarded(seg, member);
            }
        }
        if (!pass)
            std::printf("  thread-guard MISSING: %s %s\n", path,
                        member.c_str());
        CHECK(pass);
        rawPos += kSpawn.size();
    }
}

static void testThreadRestartGuards()
{
    std::printf("[test] std::thread restart guards (double-submit SIGABRT pin)\n");
    // Assigning over a joinable std::thread calls std::terminate() (SIGABRT,
    // "exited unexpectedly (134)" on a second Sign In / Connect). Each
    // audited TU is scanned for real `= std::thread(` sites above, so a new
    // unguarded restart fails here instead of slipping past a
    // hand-maintained function list. Behavioural proof lives in the
    // login-400 ui-script flow (double submit plus Back to the URL screen).
    for (const char *path : kThreadAuditFiles) auditThreadFile(path);
    std::printf("[test] std::thread restart guards OK\n");
}

static void testConnectScreenWorkerLifecycle()
{
    std::printf("[test] ConnectScreen cancellation and destruction lifecycle\n");

    std::atomic<bool> leaveStarted{false};
    std::atomic<bool> leaveCancelled{false};
    {
        ConnectScreen screen(
            "http://blocked.test",
            [&](const std::string&, ServerInfo&, std::string& error,
                const std::atomic<bool>* cancelled) {
                leaveStarted.store(true);
                while (!cancelled->load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                leaveCancelled.store(true);
                error = "cancelled";
                return false;
            });
        screen.enter();
        for (int i = 0; i < 100 && !leaveStarted.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(leaveStarted.load());
        screen.leave();
        for (int i = 0; i < 100 && !leaveCancelled.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(leaveCancelled.load());
    }

    std::atomic<bool> destructionStarted{false};
    std::atomic<bool> destructionCancelled{false};
    {
        auto screen = std::make_unique<ConnectScreen>(
            "http://blocked.test",
            [&](const std::string&, ServerInfo&, std::string& error,
                const std::atomic<bool>* cancelled) {
                destructionStarted.store(true);
                while (!cancelled->load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                destructionCancelled.store(true);
                error = "cancelled";
                return false;
            });
        screen->enter();
        for (int i = 0; i < 100 && !destructionStarted.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(destructionStarted.load());
    }
    CHECK(destructionCancelled.load());
    std::printf("[test] ConnectScreen cancellation and destruction lifecycle OK\n");
}

static void testNoDetachedProductionThreads()
{
    std::printf("[test] no detached production threads\n");
    for (const char *path : kThreadAuditFiles) {
        const std::string source = sourceTokenString(stripSourceComments(readTestBytes(path)));
        CHECK(source.find(".detach(") == std::string::npos);
        CHECK(source.find("pthread_detach(") == std::string::npos);
    }
    std::printf("[test] no detached production threads OK\n");
}

// The "one Home rail refresh == one coordinator operation" accounting is a
// telemetry property of a live fetch; the coordinator rail tests exercise the
// single in-flight slot and coalescing, and test_home_library_controller
// exercises the Home fetch.  The former source-shape pin is removed.
