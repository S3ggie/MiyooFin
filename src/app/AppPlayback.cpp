#include "App.hpp"
#include "RemoteExitSignal.hpp"
#include "DisplaySizing.hpp"
#include "../diagnostics/UiDiagnostics.hpp"
#include "../playback/PlaybackRequest.hpp"
#include "../playback/OfflinePlaybackJournal.hpp"
#include "../cache/LibraryCache.hpp"
#include "../net/RouteRequest.hpp"
#include "../net/JellyfinApi.hpp"
#include "../ui/Theme.hpp"
#include "../ui/BitmapFont.hpp"
#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <cerrno>
#include <cstdlib>
#include <thread>

namespace miyoofin {

namespace {
const char *playbackStartingLabel(Uint32 elapsedMs)
{
    static const char *const labels[] = {"Loading...", "Loading..", "Loading.", "Loading", "Loading.", "Loading..", "Loading..."};
    const Uint32 phase = elapsedMs / 150;
    return labels[phase >= 7 ? 6 : phase];
}
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
PlaybackSourceKind telemetryPlaybackSource(ScreenStack::ExternalPlaybackSource source) noexcept
{
    switch (source) {
    case ScreenStack::ExternalPlaybackSource::Jellyfin: return PlaybackSourceKind::Jellyfin;
    case ScreenStack::ExternalPlaybackSource::Local: return PlaybackSourceKind::Local;
    default: return PlaybackSourceKind::Unknown;
    }
}
void emitPlaybackEvent(PerformanceTelemetry &telemetry, uint32_t sequence,
                       PlaybackStage stage, PlaybackSourceKind source,
                       uint8_t exitKind, int32_t exitCode, uint64_t durationUs) noexcept
{
    if (!telemetry.enabledFast() || sequence == 0) return;
    TelemetryRecord record{}; record.header.record_type = RecordType::PlaybackEvent;
    record.payload.playback_event.stage=static_cast<uint8_t>(stage);
    record.payload.playback_event.source=static_cast<uint8_t>(source);
    record.payload.playback_event.child_exit_kind=exitKind;
    record.payload.playback_event.duration_us=durationUs;
    record.payload.playback_event.child_exit_code=exitCode;
    record.payload.playback_event.playback_seq=sequence;
    telemetry.emitRecord(record);
}
#endif

bool reapPlaybackChildWithin(pid_t pid, int &status,
                             std::chrono::milliseconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid)
            return true;
        if (result < 0 && errno != EINTR)
            return false;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
}

bool App::ingestPlaybackResult(bool removeAfterIngest)
{
    if (!m_session.valid()) return false;
    PlaybackResult r; std::string error;
    if (!PlaybackRequest::readResultFrom("playback-result.txt", r, error)) return false;
    bool pending = false;
    if (!r.serverReported) {
        OfflinePlaybackEntry entry;
        entry.itemId=r.itemId; entry.itemType=r.itemType; entry.baseServerTicks=r.baseResumeTicks;
        entry.finalTicks=r.positionTicks; entry.localTimestamp=(std::uint64_t)time(nullptr);
        const std::string journal=OfflinePlaybackJournal::path("cache", LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
        std::lock_guard<std::mutex> lock(m_journalMutex);
        pending=OfflinePlaybackJournal::upsert(journal, entry, nullptr);
    }
    if (removeAfterIngest) PlaybackRequest::removeAt("playback-result.txt");
    return pending;
}
void App::recoverPlaybackResult(){ if (ingestPlaybackResult(true)) scheduleJournalSync(); }

void App::scheduleJournalSync()
{
    if (!m_session.valid()) return;
    if (!m_journalSyncThread.joinable()) m_journalSyncThread=std::thread(&App::journalSyncLoop, this);
    { std::lock_guard<std::mutex> lock(m_journalMutex); m_journalWake=true; }
    m_journalCv.notify_one();
}

void App::journalSyncLoop()
{
    std::unique_lock<std::mutex> lock(m_journalMutex);
    while (!m_journalStop) {
        m_journalCv.wait(lock, [this]{ return m_journalStop || m_journalWake; });
        if (m_journalStop) break;
        m_journalWake=false;
        const unsigned failures=m_journalFailures;
        if (failures) {
            const unsigned seconds=failures > 5 ? 30 : (1u << failures);
            m_journalCv.wait_for(lock, std::chrono::seconds(seconds), [this]{ return m_journalStop || m_journalWake; });
            if (m_journalStop) break;
            m_journalWake=false;
        }
        lock.unlock();
        syncPlaybackJournal();
        lock.lock();
    }
}

void App::syncPlaybackJournal()
{
    const Session session=m_session;
    if (!session.valid()) return;
    const std::string journal=OfflinePlaybackJournal::path("cache", LibraryCache::scopeKey(session.serverUrl,session.userId));
    std::lock_guard<std::mutex> lock(m_journalMutex);
    std::vector<OfflinePlaybackEntry> entries;
    if (!OfflinePlaybackJournal::load(journal, entries, nullptr)) return;
    auto map=[](PlaybackSyncStatus value){ switch(value){case PlaybackSyncStatus::Success:return OfflineJournalRequestStatus::Success;case PlaybackSyncStatus::Unauthorized:return OfflineJournalRequestStatus::Unauthorized;case PlaybackSyncStatus::Missing:return OfflineJournalRequestStatus::Missing;default:return OfflineJournalRequestStatus::Transient;} };
    OfflineJournalSyncStats stats=syncOfflinePlaybackEntries(entries,
        [&](const OfflinePlaybackEntry &entry,std::int64_t &ticks){std::string error; PlaybackSyncStatus status=PlaybackSyncStatus::Transient; RouteRequest(session).run([&](const std::string &base){status=JellyfinApi::getPlaybackPositionTicks(base,session.accessToken,session.userId,session.deviceId,entry.itemId,ticks,error);return status==PlaybackSyncStatus::Success;},error);return map(status);},
        [&](const OfflinePlaybackEntry &entry){std::string error; PlaybackSyncStatus status=PlaybackSyncStatus::Transient; RouteRequest(session).run([&](const std::string &base){status=JellyfinApi::reportPlaybackStopped(base,session.accessToken,session.deviceId,entry.itemId,entry.finalTicks,error);return status==PlaybackSyncStatus::Success;},error);return map(status);});
    if (stats.changed) OfflinePlaybackJournal::save(journal, entries, nullptr);
    m_journalFailures=stats.retry ? m_journalFailures+1 : 0;
}
void App::handleExternalPlayback()
{
    printf("[App] Starting external playback handoff\n");

    // 1. Suspend SDL/video/input
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
    PerformanceTelemetry &telemetry = performanceTelemetry();
    TelemetryTimer suspendTimer;
#endif
    if (!suspendPlatform()) {
        fprintf(stderr, "[App] Failed to suspend platform — aborting playback\n");
        return;
    }
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
    emitPlaybackEvent(telemetry, m_playbackSequence,
                      PlaybackStage::SuspendPlatform,
                      telemetryPlaybackSource(m_playbackSource), 0, 0,
                      suspendTimer.elapsedUs());
#endif

    // 2. Locate the playback runner script relative to this binary
    std::string runnerPath;
    {
        char exePath[4096];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0) {
            exePath[len] = '\0';
            std::string path(exePath);
            size_t lastSlash = path.rfind('/');
            if (lastSlash != std::string::npos) {
                runnerPath = path.substr(0, lastSlash + 1) + "playback_runner.sh";
            }
        }
    }
    if (runnerPath.empty()) {
        runnerPath = "./playback_runner.sh";
    }

    printf("[App] Playback runner: %s\n", runnerPath.c_str());

    // 3. Fork and exec the playback runner
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[App] fork() failed: %s\n", strerror(errno));
        resumePlatform();
        return;
    }

    if (pid == 0) {
        // Child process — exec the playback runner script
        execl("/bin/sh", "sh", runnerPath.c_str(), (char *)nullptr);
        // exec failed
        _exit(127);
    }

    // 4. Parent waits for child to finish.  A supported Onion exit can
    // interrupt this wait while the runner is still waiting for FFplay.  Do
    // not let that leave the runner (and its external children) orphaned.
    printf("[App] Waiting for playback child (PID=%d)\n", pid);
    int status = 0;
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
    telemetry.suspendSampling(true, SamplingReason::ExternalPlayback);
    telemetry.emitSessionEvent(SessionEventKind::SamplingSuspended,
                               Outcome::Success,
                               static_cast<uint32_t>(SamplingReason::ExternalPlayback), 0);
    TelemetryTimer childWaitTimer;
#endif
    pid_t waitedPid = -1;
    bool exitRequested = false;
    for (;;) {
        if (consumeRemoteExitRequest()) {
            exitRequested = true;
            break;
        }
        waitedPid = waitpid(pid, &status, WNOHANG);
        if (waitedPid == pid)
            break;
        if (waitedPid < 0 && errno != EINTR) {
            fprintf(stderr, "[App] Playback child wait failed: %s\n", strerror(errno));
            exitRequested = true;
            break;
        }
        // Keep the wait interruptible: a remote exit request can arrive
        // immediately after the check above, so never enter a blocking
        // waitpid call here.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (exitRequested) {
        printf("[App] Terminating playback child after exit request (PID=%d)\n", pid);
        if (kill(pid, SIGTERM) != 0 && errno != ESRCH)
            fprintf(stderr, "[App] Failed to terminate playback child: %s\n", strerror(errno));
        bool childReaped = reapPlaybackChildWithin(pid, status, std::chrono::seconds(8));
        if (!childReaped) {
            printf("[App] Playback child did not exit after TERM; sending KILL (PID=%d)\n", pid);
            if (kill(pid, SIGKILL) != 0 && errno != ESRCH)
                fprintf(stderr, "[App] Failed to kill playback child: %s\n", strerror(errno));
            childReaped = reapPlaybackChildWithin(pid, status, std::chrono::seconds(2));
            if (!childReaped)
                fprintf(stderr, "[App] Playback child was not reaped after KILL (PID=%d)\n", pid);
        }
        waitedPid = childReaped ? pid : -1;
        m_running = false;
    }

    if (waitedPid >= 0 && WIFEXITED(status)) {
        printf("[App] Playback child exited with status %d\n", WEXITSTATUS(status));
    } else if (waitedPid >= 0 && WIFSIGNALED(status)) {
        printf("[App] Playback child killed by signal %d\n", WTERMSIG(status));
    }
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
    const uint8_t childExitKind = waitedPid < 0 ? 0
        : (WIFEXITED(status) ? 1 : (WIFSIGNALED(status) ? 2 : 0));
    const int32_t childExitCode = waitedPid >= 0 && WIFEXITED(status)
        ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    emitPlaybackEvent(telemetry, m_playbackSequence,
                      PlaybackStage::ChildWait,
                      telemetryPlaybackSource(m_playbackSource), childExitKind, childExitCode,
                      childWaitTimer.elapsedUs());
#endif

    // 5. Resume SDL/video/input
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
    TelemetryTimer resumeTimer;
#endif
    if (!resumePlatform()) {
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
        telemetry.suspendSampling(false, SamplingReason::ExternalPlayback);
        telemetry.emitSessionEvent(SessionEventKind::SamplingResumed,
                                   Outcome::Success,
                                   static_cast<uint32_t>(SamplingReason::ExternalPlayback), 0);
#endif
        fprintf(stderr, "[App] Failed to resume platform — exiting\n");
        m_running = false;
        return;
    }
#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
    emitPlaybackEvent(telemetry, m_playbackSequence,
                      PlaybackStage::ResumePlatform,
                      telemetryPlaybackSource(m_playbackSource), 0, 0,
                      resumeTimer.elapsedUs());
    telemetry.suspendSampling(false, SamplingReason::ExternalPlayback);
    telemetry.emitSessionEvent(SessionEventKind::SamplingResumed,
                               Outcome::Success,
                               static_cast<uint32_t>(SamplingReason::ExternalPlayback), 0);
    if (telemetry.enabledFast())
        m_playbackResumeUs = TelemetryClock::monotonicUs();
#endif

    // Read (but leave for the active screen to consume) before its next update.
    if (ingestPlaybackResult(false)) scheduleJournalSync();

    printf("[App] External playback handoff complete, resuming UI\n");
}

void App::drawPlaybackStartingOverlay(Uint32 elapsedMs)
{
    const char *label = playbackStartingLabel(elapsedMs);
    const int textW = static_cast<int>(std::strlen(label)) * BitmapFont::GLYPH_W;
    const int paddingX = 12;
    const int paddingY = 8;
    const int boxW = textW + paddingX * 2;
    const int boxH = BitmapFont::GLYPH_H + paddingY * 2;
    const int boxX = (m_fb->w - boxW) / 2;
    const int boxY = (m_fb->h - boxH) / 2;

    BitmapFont::fillRect(m_fb, boxX, boxY, boxW, boxH,
                         Theme::BG_R, Theme::BG_G, Theme::BG_B);
    BitmapFont::drawRect(m_fb, boxX, boxY, boxW, boxH,
                         Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B);
    BitmapFont::drawString(m_fb, boxX + paddingX, boxY + paddingY, label,
                           Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);
}
}
