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

