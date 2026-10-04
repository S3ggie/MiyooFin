// --- BatteryMonitor: header battery icon level from OnionOS batmon -----------

static void testBatteryParsePercent()
{
    std::printf("[test] BatteryMonitor::parsePercent\n");
    CHECK(BatteryMonitor::parsePercent("48") == 48);
    CHECK(BatteryMonitor::parsePercent("48\n") == 48);
    CHECK(BatteryMonitor::parsePercent("  7 \r\n") == 7);
    CHECK(BatteryMonitor::parsePercent("0") == 0);
    CHECK(BatteryMonitor::parsePercent("100") == 100);
    CHECK(BatteryMonitor::parsePercent(std::string("55\0\0", 4)) == 55); // stale NULs
    // Anything that is not a whole number in 0..100 is unknown.
    CHECK(BatteryMonitor::parsePercent("") == -1);
    CHECK(BatteryMonitor::parsePercent("\n") == -1);
    CHECK(BatteryMonitor::parsePercent("101") == -1);
    CHECK(BatteryMonitor::parsePercent("-5") == -1);
    CHECK(BatteryMonitor::parsePercent("abc") == -1);
    CHECK(BatteryMonitor::parsePercent("48%") == -1);
    CHECK(BatteryMonitor::parsePercent("48 49") == -1);
    CHECK(BatteryMonitor::parsePercent("99999") == -1);
    std::printf("[test] BatteryMonitor::parsePercent OK\n");
}

static void testBatteryIconMath()
{
    std::printf("[test] BatteryMonitor fill width and colours\n");
    CHECK(BatteryMonitor::fillWidth(-1, 18) == 0); // unknown: empty
    CHECK(BatteryMonitor::fillWidth(0, 18) == 0);
    CHECK(BatteryMonitor::fillWidth(1, 18) == 1);  // any charge shows a sliver
    CHECK(BatteryMonitor::fillWidth(48, 18) == 9); // 48% of 18 = 8.64 -> 9
    CHECK(BatteryMonitor::fillWidth(50, 18) == 9);
    CHECK(BatteryMonitor::fillWidth(99, 18) == 18); // rounds up to full
    CHECK(BatteryMonitor::fillWidth(100, 18) == 18);
    CHECK(BatteryMonitor::fillWidth(50, 0) == 0);
    // Monotonic: more charge never draws a shorter bar.
    int previous = 0;
    for (int p = 0; p <= 100; ++p) {
        const int w = BatteryMonitor::fillWidth(p, 18);
        CHECK(w >= previous && w <= 18);
        previous = w;
    }
    // Colour thresholds: red <= 15, amber 16-40, green above.
    auto isColor = [](BatteryMonitor::Color c, int r, int g, int b) {
        return c.r == r && c.g == g && c.b == b;
    };
    CHECK(isColor(BatteryMonitor::levelColor(15), 225, 80, 80));
    CHECK(isColor(BatteryMonitor::levelColor(16), 235, 190, 70));
    CHECK(isColor(BatteryMonitor::levelColor(40), 235, 190, 70));
    CHECK(isColor(BatteryMonitor::levelColor(41), 110, 210, 120));
    CHECK(isColor(BatteryMonitor::levelColor(100), 110, 210, 120));
    std::printf("[test] BatteryMonitor fill width and colours OK\n");
}

static void testBatteryMonitorReadsAndThrottles()
{
    std::printf("[test] BatteryMonitor reads and throttles\n");
    const std::string path = "/tmp/miyoofin-battery-test-" + std::to_string(::getpid());
    auto write = [&](const char* text) {
        std::FILE* f = std::fopen(path.c_str(), "w");
        CHECK(f != nullptr);
        if (!f)
            return;
        std::fputs(text, f);
        std::fclose(f);
    };
    std::remove(path.c_str());
    BatteryMonitor missing(path);
    missing.update(16);
    CHECK(missing.percent() == -1); // no file: unknown, not a fake full battery

    write("73\n");
    BatteryMonitor monitor(path);
    CHECK(monitor.percent() == -1); // nothing read before the first update
    monitor.update(16);
    CHECK(monitor.percent() == 73); // first update reads immediately
    write("12\n");
    monitor.update(BatteryMonitor::kRefreshMs - 1000);
    CHECK(monitor.percent() == 73); // still within the interval: not re-read
    monitor.update(999);
    CHECK(monitor.percent() == 73); // one ms short
    monitor.update(1);
    CHECK(monitor.percent() == 12); // interval elapsed: re-read
    // A garbage or vanished file drops back to unknown on the next read.
    write("not a number");
    monitor.update(BatteryMonitor::kRefreshMs);
    CHECK(monitor.percent() == -1);
    write("88");
    monitor.update(BatteryMonitor::kRefreshMs);
    CHECK(monitor.percent() == 88);
    std::remove(path.c_str());
    monitor.update(BatteryMonitor::kRefreshMs);
    CHECK(monitor.percent() == -1);
    std::printf("[test] BatteryMonitor reads and throttles OK\n");
}

// --- BatteryMonitor charging: axp_test parsing and the bounded probe --------

static void testBatteryParseCharging()
{
    std::printf("[test] BatteryMonitor::parseCharging\n");
    CHECK(BatteryMonitor::parseCharging("{\"battery\":48, \"voltage\":3900, \"charging\":1}") == 1);
    CHECK(BatteryMonitor::parseCharging("{\"battery\":48, \"voltage\":3900, \"charging\":0}\n") ==
          0);
    CHECK(BatteryMonitor::parseCharging("{\"charging\" : 1 }") == 1);
    // Anything else is unknown: error text (no root), missing key, bad values.
    CHECK(BatteryMonitor::parseCharging("error during I2C_RDWR ioctl with error code -1") == -1);
    CHECK(BatteryMonitor::parseCharging("") == -1);
    CHECK(BatteryMonitor::parseCharging("{\"battery\":48}") == -1);
    // Like batmon (sscanf %d): any non-zero value means charging, and the raw
    // number is reported (the chip may use values other than 1).
    int raw = -5;
    CHECK(BatteryMonitor::parseCharging("{\"charging\":2}", &raw) == 1 && raw == 2);
    CHECK(BatteryMonitor::parseCharging("{\"charging\":10}", &raw) == 1 && raw == 10);
    CHECK(BatteryMonitor::parseCharging("{\"charging\":0}", &raw) == 0 && raw == 0);
    CHECK(BatteryMonitor::parseCharging("{\"charging\":-1}", &raw) == -1 && raw == -1);
    CHECK(BatteryMonitor::parseCharging("no key here", &raw) == -1 && raw == -1);
    CHECK(BatteryMonitor::parseCharging("{\"charging\":") == -1);
    CHECK(BatteryMonitor::parseCharging("{\"charging\":x}") == -1);
    CHECK(BatteryMonitor::parseCharging("{\"charging\":}") == -1);
    std::printf("[test] BatteryMonitor::parseCharging OK\n");
}

static void testBatteryChargingProbe()
{
    std::printf("[test] BatteryMonitor charging probe\n");
    const std::string dir = "/tmp/miyoofin-axp-test-" + std::to_string(::getpid());
    ::mkdir(dir.c_str(), 0755);
    const std::string missingPercent = dir + "/nope";

    // Charging: the real fork/exec path reads the fake's JSON.
    std::string axp =
        writeFakeAxp(dir, "echo '{\"battery\":50, \"voltage\":4100, \"charging\":1}'\n");
    {
        BatteryMonitor monitor(missingPercent, axp, 1500);
        CHECK(!monitor.charging()); // nothing probed yet
        CHECK(pumpBattery(monitor, 16, [&] { return monitor.charging(); }));
    }
    // A failed query must not flicker the bolt: the last known state is kept
    // for a few probes and only a sustained failure means unknown.
    axp = writeFakeAxp(dir, "echo '{\"battery\":50, \"voltage\":4100, \"charging\":1}'\n");
    {
        BatteryMonitor monitor(missingPercent, axp, 1500);
        CHECK(pumpBattery(monitor, 16, [&] { return monitor.charging(); }));
        // The failing fake records when it starts and finishes, so a cycle waits on those events
        // instead of a fixed sleep (a loaded host can take far longer than any fixed delay to
        // fork/exec the script, and a probe that is not reaped yet would not count as a failure).
        const std::string started = dir + "/probe-started", finished = dir + "/probe-finished";
        std::remove(started.c_str());
        std::remove(finished.c_str());
        writeFakeAxp(dir, "echo s >> " + started + "\necho 'error during I2C_RDWR ioctl with error code -1'\necho f >> " + finished + "\n");
        auto lines = [](const std::string& path) {
            std::ifstream in(path);
            int n = 0;
            for (std::string line; std::getline(in, line);)
                ++n;
            return n;
        };
        // One cycle: the probe in flight (if any) has run to the end and is reaped, and the next
        // one is started. update(refresh) both reaps and starts, so it is only repeated until a
        // new probe is seen to start.
        auto cycle = [&] {
            for (int i = 0; i < 1000 && lines(finished) != lines(started); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const int before = lines(started);
            for (int i = 0; i < 1000 && lines(started) == before; ++i) {
                monitor.update(BatteryMonitor::kChargingRefreshMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        cycle(); // starts the first failing probe; each later cycle reaps the previous one
        for (int i = 0; i < BatteryMonitor::kChargingUnknownLimit - 2; ++i)
            cycle();
        CHECK(monitor.charging()); // a couple of failures: still charging
        for (int i = 0; i < 4; ++i)
            cycle();
        CHECK(!monitor.charging()); // sustained failure: unknown, no bolt
    }
    // A non-1 charging value (as OnionOS treats it) still means charging.
    axp = writeFakeAxp(dir, "echo '{\"battery\":50, \"voltage\":4100, \"charging\":3}'\n");
    {
        BatteryMonitor monitor(missingPercent, axp, 1500);
        CHECK(pumpBattery(monitor, 16, [&] { return monitor.charging(); }));
    }
    // Not charging, and it follows a change on the next probe interval.
    axp = writeFakeAxp(dir, "echo '{\"battery\":50, \"voltage\":4100, \"charging\":1}'\n");
    {
        BatteryMonitor monitor(missingPercent, axp, 1500);
        CHECK(pumpBattery(monitor, 16, [&] { return monitor.charging(); }));
        writeFakeAxp(dir, "echo '{\"battery\":50, \"voltage\":4100, \"charging\":0}'\n");
        CHECK(pumpBattery(monitor, BatteryMonitor::kChargingRefreshMs,
                          [&] { return !monitor.charging(); }));
    }
    // Error output without root: unknown, so no bolt.
    axp = writeFakeAxp(dir, "echo 'error during I2C_RDWR ioctl with error code -1'\n");
    {
        BatteryMonitor monitor(missingPercent, axp, 1500);
        for (int i = 0; i < 40; ++i) {
            monitor.update(16);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(!monitor.charging());
    }
    // A probe that cannot be executed (exit 127) is given up on: it runs once,
    // however many intervals pass.
    const std::string counter = dir + "/count";
    axp = writeFakeAxp(dir, "echo x >> " + counter + "\nexit 127\n");
    {
        BatteryMonitor monitor(missingPercent, axp, 1500);
        for (int i = 0; i < 6; ++i) {
            monitor.update(BatteryMonitor::kChargingRefreshMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::ifstream in(counter);
        std::string line;
        int runs = 0;
        while (std::getline(in, line))
            ++runs;
        CHECK(runs == 1);
        CHECK(!monitor.charging());
    }
    // A missing program is also just "not charging".
    {
        BatteryMonitor monitor(missingPercent, dir + "/does-not-exist", 1500);
        for (int i = 0; i < 20; ++i) {
            monitor.update(16);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(!monitor.charging());
    }
    // A hung program is killed at the timeout and never blocks teardown.
    axp = writeFakeAxp(dir, "sleep 30\n");
    {
        const auto start = std::chrono::steady_clock::now();
        {
            BatteryMonitor monitor(missingPercent, axp, 300);
            monitor.update(16);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CHECK(!monitor.charging());
        } // destructor cancels and joins the probe
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
        CHECK(elapsedMs < 2000);
    }
    std::remove((dir + "/axp_test").c_str());
    std::remove(counter.c_str());
    ::rmdir(dir.c_str());
    std::printf("[test] BatteryMonitor charging probe OK\n");
}

static void testConnectionMonitorStatesAndHysteresis()
{
    std::printf("[test] ConnectionMonitor states and hysteresis\n");
    std::atomic<int> mode{0}; // 0 connected, 1 offline, 2 session expired
    std::atomic<int> calls{0};
    auto probe = [&](const CancelToken&) {
        ++calls;
        return mode.load() == 0   ? LinkStatus::Connected
               : mode.load() == 1 ? LinkStatus::Offline
                                  : LinkStatus::SessionExpired;
    };
    ConnectionMonitor monitor(probe, 1000, 1000);
    CHECK(monitor.status() == LinkStatus::Checking); // nothing probed yet
    // First probe succeeds: connected.
    CHECK(pumpLink(monitor, 1000, [&] { return monitor.status() == LinkStatus::Connected; }));
    // One failed probe after a good one keeps "connected" (no flicker)...
    mode = 1;
    const int before = calls.load();
    CHECK(pumpLink(monitor, 1000, [&] { return calls.load() >= before + 1; }));
    for (int i = 0; i < 5; ++i) { // let that single failed result be reaped
        monitor.update(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(monitor.status() == LinkStatus::Connected);
    // ...the second failure in a row is offline.
    CHECK(pumpLink(monitor, 1000, [&] { return monitor.status() == LinkStatus::Offline; }));
    // Recovery is immediate.
    mode = 0;
    CHECK(pumpLink(monitor, 1000, [&] { return monitor.status() == LinkStatus::Connected; }));
    // A rejected session is definitive and shows at once.
    mode = 2;
    CHECK(pumpLink(monitor, 1000, [&] { return monitor.status() == LinkStatus::SessionExpired; }));
    std::printf("[test] ConnectionMonitor states and hysteresis OK\n");
}

static void testConnectionMonitorFirstFailureIsOffline()
{
    std::printf("[test] ConnectionMonitor first failure\n");
    ConnectionMonitor monitor([](const CancelToken&) { return LinkStatus::Offline; }, 1000, 1000);
    // Before any success there is nothing to keep "connected": offline at once.
    CHECK(pumpLink(monitor, 1000, [&] { return monitor.status() == LinkStatus::Offline; }));
    std::printf("[test] ConnectionMonitor first failure OK\n");
}

static void testConnectionMonitorIntervalsPokeAndOfflineMode()
{
    std::printf("[test] ConnectionMonitor intervals, poke and offline mode\n");
    std::atomic<int> calls{0};
    ConnectionMonitor monitor(
        [&](const CancelToken&) {
            ++calls;
            return LinkStatus::Connected;
        },
        500, 500);
    CHECK(pumpLink(monitor, 16,
                   [&] { return calls.load() == 1 && monitor.status() == LinkStatus::Connected; }));
    // Within the interval no further probe starts.
    for (int i = 0; i < 10; ++i) {
        monitor.update(30);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(calls.load() == 1);
    // pokeNow() probes without waiting for the interval.
    monitor.pokeNow();
    CHECK(pumpLink(monitor, 16, [&] { return calls.load() >= 2; }));
    // Offline mode reports OfflineMode and stops probing.
    monitor.setOfflineMode(true);
    CHECK(monitor.status() == LinkStatus::OfflineMode);
    for (int i = 0; i < 5; ++i) { // let an in-flight probe finish
        monitor.update(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const int frozen = calls.load();
    for (int i = 0; i < 20; ++i) {
        monitor.update(1000);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(calls.load() == frozen);
    CHECK(monitor.status() == LinkStatus::OfflineMode);
    // Leaving offline mode re-checks immediately.
    monitor.setOfflineMode(false);
    CHECK(monitor.status() == LinkStatus::Checking);
    CHECK(pumpLink(monitor, 16, [&] { return monitor.status() == LinkStatus::Connected; }));
    CHECK(calls.load() > frozen);
    std::printf("[test] ConnectionMonitor intervals, poke and offline mode OK\n");
}

static void testConnectionMonitorDestructorCancelsBlockedProbe()
{
    std::printf("[test] ConnectionMonitor destructor cancels a blocked probe\n");
    std::atomic<bool> started{false};
    const auto begin = std::chrono::steady_clock::now();
    {
        ConnectionMonitor monitor(
            [&](const CancelToken& cancel) {
                started = true;
                // A hung request that only ends when it is cancelled.
                for (int i = 0; i < 3000 && !cancel->load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return LinkStatus::Offline;
            },
            1000, 1000);
        for (int i = 0; i < 200 && !started.load(); ++i) {
            monitor.update(1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(started.load());
    } // destructor must cancel and join without waiting out the 3s probe
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - begin)
                        .count();
    CHECK(ms < 1500);
    std::printf("[test] ConnectionMonitor destructor cancels a blocked probe OK\n");
}
