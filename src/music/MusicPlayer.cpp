#include "MusicPlayer.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace miyoofin {
namespace music {

namespace {

constexpr std::int64_t kHangMs = 6000;
constexpr std::int64_t kProgressMs = 10000;
constexpr std::int64_t kCrashWindowMs = 60000;
constexpr int kMaxCrashes = 3;
constexpr std::int64_t kSaveMs = 5000;

std::int64_t steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string randomHex(int bytes)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    FILE* f = std::fopen("/dev/urandom", "rb");
    for (int i = 0; i < bytes; ++i) {
        unsigned char b = static_cast<unsigned char>(std::rand());
        if (f)
            (void)std::fread(&b, 1, 1, f);
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    if (f)
        std::fclose(f);
    return out;
}

void setNonBlocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

} // namespace

EngineEvent parseEngineEvent(const std::string& line)
{
    EngineEvent e;
    const std::size_t space = line.find(' ');
    const std::string word = line.substr(0, space);
    const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
    if (word == "ready") {
        e.type = EngineEvent::Type::Ready;
    } else if (word == "idle") {
        e.type = EngineEvent::Type::Idle;
    } else if (word == "started" || word == "ended" || word == "error") {
        e.type = word == "started" ? EngineEvent::Type::Started
                 : word == "ended" ? EngineEvent::Type::Ended
                                   : EngineEvent::Type::Error;
        e.path = rest;
    } else if (word == "pos") {
        double pos = 0, dur = 0;
        int paused = 0;
        if (std::sscanf(rest.c_str(), "%lf %lf %d", &pos, &dur, &paused) == 3) {
            e.type = EngineEvent::Type::Pos;
            e.position = pos;
            e.duration = dur;
            e.paused = paused != 0;
        }
    }
    return e;
}

MusicPlayer::MusicPlayer(PlayerOptions options, PlayerHooks hooks)
    : now(steadyMs), m_options(std::move(options)), m_hooks(std::move(hooks)),
      m_cancelFetch(std::make_shared<std::atomic<bool>>(false))
{
    std::signal(SIGPIPE, SIG_IGN); // a dead engine must never kill the UI
    m_fetchThread = std::thread([this] { fetchLoop(); });
    m_reportThread = std::thread([this] { reportLoop(); });
}

MusicPlayer::~MusicPlayer()
{
    shutdown(2000);
    m_cancelFetch->store(true);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopWorkers = true;
    }
    m_wake.notify_all();
    m_fetchThread.join();
    m_reportThread.join();
}

// ---- engine process ---------------------------------------------------------------------------

bool MusicPlayer::ensureEngine()
{
    if (m_pid > 0)
        return true;
    if (m_options.enginePath.empty() || access(m_options.enginePath.c_str(), X_OK) != 0) {
        m_message = "Audio engine missing";
        return false;
    }
    int toEngine[2], fromEngine[2];
    if (pipe2(toEngine, O_CLOEXEC) != 0)
        return false;
    if (pipe2(fromEngine, O_CLOEXEC) != 0) {
        close(toEngine[0]);
        close(toEngine[1]);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, toEngine[0], 0);
    posix_spawn_file_actions_adddup2(&actions, fromEngine[1], 1);
    if (!m_options.engineLog.empty())
        posix_spawn_file_actions_addopen(&actions, 2, m_options.engineLog.c_str(),
                                         O_WRONLY | O_CREAT | O_APPEND, 0644);
    else
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);

    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        bool drop = false;
        for (const std::string& name : m_options.engineUnsetEnv)
            if (std::strncmp(*e, name.c_str(), name.size()) == 0 && (*e)[name.size()] == '=')
                drop = true;
        if (!drop)
            env.push_back(*e);
    }
    for (const std::string& extra : m_options.engineEnv)
        env.push_back(extra);
    std::vector<char*> envp;
    for (std::string& e : env)
        envp.push_back(&e[0]);
    envp.push_back(nullptr);

    std::string path = m_options.enginePath;
    char* argv[] = {&path[0], nullptr};
    pid_t pid = -1;
    const int rc =
        posix_spawn(&pid, m_options.enginePath.c_str(), &actions, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(toEngine[0]);
    close(fromEngine[1]);
    if (rc != 0) {
        close(toEngine[1]);
        close(fromEngine[0]);
        m_message = "Audio engine failed to start";
        return false;
    }
    m_pid = pid;
    m_toEngine = toEngine[1];
    m_fromEngine = fromEngine[0];
    setNonBlocking(m_toEngine);
    setNonBlocking(m_fromEngine);
    m_inBuf.clear();
    m_outBuf.clear();
    m_engineReady = false;
    m_spawnedMs = m_lastEventMs = now();
    return true;
}

void MusicPlayer::killEngine()
{
    if (m_pid > 0) {
        kill(m_pid, SIGKILL);
        waitpid(m_pid, nullptr, 0);
    }
    if (m_toEngine >= 0)
        close(m_toEngine);
    if (m_fromEngine >= 0)
        close(m_fromEngine);
    m_pid = -1;
    m_toEngine = m_fromEngine = -1;
    m_engineReady = false;
}

void MusicPlayer::sendLine(const std::string& line)
{
    if (!ensureEngine())
        return;
    m_outBuf += line;
    m_outBuf += '\n';
    while (!m_outBuf.empty()) {
        const ssize_t n = write(m_toEngine, m_outBuf.data(), m_outBuf.size());
        if (n > 0) {
            m_outBuf.erase(0, static_cast<std::size_t>(n));
        } else {
            break; // EAGAIN: poll() flushes the rest
        }
    }
}

void MusicPlayer::shutdown(int timeoutMs)
{
    finishPlayback(true);
    if (m_pid <= 0)
        return;
    sendLine("quit");
    const std::int64_t deadline = now() + timeoutMs;
    int status = 0;
    while (now() < deadline) {
        if (waitpid(m_pid, &status, WNOHANG) == m_pid) {
            m_pid = -1;
            break;
        }
        usleep(20000);
    }
    if (m_pid > 0) {
        killEngine();
    } else {
        if (m_toEngine >= 0)
            close(m_toEngine);
        if (m_fromEngine >= 0)
            close(m_fromEngine);
        m_toEngine = m_fromEngine = -1;
        m_engineReady = false;
    }
}

void MusicPlayer::pumpEngine()
{
    if (m_pid <= 0)
        return;
    if (!m_outBuf.empty()) {
        const ssize_t n = write(m_toEngine, m_outBuf.data(), m_outBuf.size());
        if (n > 0)
            m_outBuf.erase(0, static_cast<std::size_t>(n));
    }
    char buf[2048];
    for (;;) {
        const ssize_t n = read(m_fromEngine, buf, sizeof(buf));
        if (n <= 0)
            break;
        m_inBuf.append(buf, static_cast<std::size_t>(n));
        if (m_inBuf.size() > 65536)
            m_inBuf.clear();
    }
    std::size_t nl;
    while ((nl = m_inBuf.find('\n')) != std::string::npos) {
        std::string line = m_inBuf.substr(0, nl);
        m_inBuf.erase(0, nl + 1);
        const EngineEvent e = parseEngineEvent(line);
        if (e.type != EngineEvent::Type::None) {
            m_lastEventMs = now();
            handleEvent(e);
        }
    }
}

void MusicPlayer::superviseEngine()
{
    if (m_pid <= 0)
        return;
    int status = 0;
    bool dead = waitpid(m_pid, &status, WNOHANG) == m_pid;
    if (!dead && m_started && m_state != PlayState::Idle && now() - m_lastEventMs > kHangMs) {
        kill(m_pid, SIGKILL);
        waitpid(m_pid, &status, 0);
        dead = true;
    }
    if (!dead)
        return;
    m_pid = -1;
    killEngine(); // closes the pipes
    if (m_state == PlayState::Idle)
        return;
    const std::int64_t t = now();
    m_crashes.push_back(t);
    while (!m_crashes.empty() && t - m_crashes.front() > kCrashWindowMs)
        m_crashes.pop_front();
    if (static_cast<int>(m_crashes.size()) > kMaxCrashes) {
        m_message = "Audio engine keeps stopping";
        finishPlayback(false);
        return;
    }
    startCurrent(m_position); // respawns lazily and resumes where it was
}

// ---- playback state machine ---------------------------------------------------------------------

void MusicPlayer::setState(PlayState state)
{
    m_state = state;
    const bool awake = state == PlayState::Playing || state == PlayState::Loading;
    if (awake != m_awake) {
        m_awake = awake;
        if (m_hooks.setAwake)
            m_hooks.setAwake(awake);
    }
}

void MusicPlayer::requestFetch(const Track& track, bool forNext)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fetchJobs.push_back({track, m_serial, forNext, m_cancelFetch});
    }
    m_wake.notify_all();
}

void MusicPlayer::startCurrent(double startSec)
{
    const Track* t = m_queue.current();
    if (!t) {
        finishPlayback(false);
        return;
    }
    // A new load supersedes whatever was being fetched.
    m_cancelFetch->store(true);
    m_cancelFetch = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fetchJobs.clear();
    }
    ++m_serial;
    m_pendingStart = startSec;
    m_started = false;
    m_loadedPath.clear();
    m_preloadPath.clear();
    m_preloadRequested = false;
    m_position = startSec;
    m_duration = t->durationSeconds();
    m_message.clear();
    m_playSessionId = randomHex(8);
    setState(PlayState::Loading);
    requestFetch(*t, false);
}

void MusicPlayer::invalidatePreload()
{
    if (m_state == PlayState::Idle)
        return;
    if (m_preloadRequested || !m_preloadPath.empty())
        sendLine("nonext");
    m_preloadPath.clear();
    m_preloadRequested = false;
    maybePreloadNext();
}

void MusicPlayer::maybePreloadNext()
{
    if (m_state == PlayState::Idle || m_loadedPath.empty() || m_preloadRequested)
        return;
    const Track* next = m_queue.peekNext();
    if (!next)
        return;
    m_preloadRequested = true;
    m_preloadTrackId = next->id;
    if (m_queue.repeat() == Repeat::One && next->id == m_queue.current()->id) {
        m_preloadPath = m_loadedPath; // the same file again
        sendLine("next " + m_preloadPath);
        return;
    }
    requestFetch(*next, true);
}

void MusicPlayer::applyFetchResult(const FetchResult& r)
{
    if (r.serial != m_serial || m_state == PlayState::Idle)
        return;
    if (r.forNext) {
        const Track* next = m_queue.peekNext();
        if (r.resolved.ok && next && next->id == r.track.id) {
            m_preloadPath = r.resolved.path;
            sendLine("next " + m_preloadPath);
        }
        return;
    }
    if (!r.resolved.ok) {
        failCurrent(r.resolved.error.empty() ? "Can't play " + r.track.title : r.resolved.error);
        return;
    }
    m_loadedPath = r.resolved.path;
    char start[32];
    std::snprintf(start, sizeof(start), "%.1f", m_pendingStart);
    sendLine(std::string("load ") + start + " " + m_loadedPath);
    maybePreloadNext();
}

void MusicPlayer::failCurrent(const std::string& message)
{
    m_message = message;
    ++m_failures;
    if (m_failures >= std::max(1, m_queue.size()) || !m_queue.skipNext()) {
        finishPlayback(false);
        return;
    }
    startCurrent(0);
}

void MusicPlayer::report(ReportKind kind, bool paused)
{
    const Track* t = m_queue.current();
    if (!t || !m_hooks.report)
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_reportJobs.push_back({kind, *t, static_cast<std::int64_t>(m_position * 10000000.0),
                                paused, m_playSessionId});
    }
    m_wake.notify_all();
}

void MusicPlayer::saveQueueNow()
{
    m_queueDirty = false;
    m_lastSaveMs = now();
    if (!m_hooks.saveQueue)
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pendingSave = m_queue.empty() || m_queueFinished
                            ? std::string()
                            : serializeQueue(m_queue.snapshot(m_position));
        m_hasPendingSave = true;
    }
    m_wake.notify_all();
}

void MusicPlayer::finishPlayback(bool reportStop, bool naturalEnd)
{
    if (naturalEnd)
        m_queueFinished = true;
    if (m_state == PlayState::Idle) {
        if (naturalEnd)
            saveQueueNow();
        return;
    }
    if (reportStop && m_started)
        report(ReportKind::Stopped, false);
    if (m_pid > 0)
        sendLine("stop");
    m_cancelFetch->store(true);
    m_cancelFetch = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fetchJobs.clear();
    }
    ++m_serial;
    m_started = false;
    m_loadedPath.clear();
    m_preloadPath.clear();
    m_preloadRequested = false;
    m_failures = 0;
    setState(PlayState::Idle);
    saveQueueNow(); // keep the queue and position, or clear them after a natural end
}

void MusicPlayer::handleEvent(const EngineEvent& e)
{
    using T = EngineEvent::Type;
    switch (e.type) {
    case T::Ready:
        m_engineReady = true;
        break;
    case T::Started:
        if (m_state == PlayState::Idle || e.path != m_loadedPath)
            break;
        m_started = true;
        m_failures = 0;
        m_lastProgressMs = now();
        if (m_state == PlayState::Loading)
            setState(PlayState::Playing);
        report(ReportKind::Start, false);
        maybePreloadNext();
        markQueueDirty();
        break;
    case T::Pos:
        if (m_state == PlayState::Idle || !m_started)
            break;
        m_position = e.position;
        if (e.duration > 0)
            m_duration = e.duration;
        break;
    case T::Ended: {
        if (m_state == PlayState::Idle || e.path != m_loadedPath)
            break;
        m_position = m_duration;
        report(ReportKind::Stopped, false);
        m_started = false;
        const bool gapless = !m_preloadPath.empty();
        if (!m_queue.advance()) {
            finishPlayback(false, true);
            break;
        }
        markQueueDirty();
        if (gapless) {
            m_loadedPath = m_preloadPath;
            m_preloadPath.clear();
            m_preloadRequested = false;
            m_position = 0;
            if (const Track* t = m_queue.current())
                m_duration = t->durationSeconds();
            m_playSessionId = randomHex(8);
            setState(PlayState::Loading); // becomes Playing on `started`
        } else {
            startCurrent(0);
        }
        break;
    }
    case T::Idle:
        // The engine ran dry before we managed to queue the next file: load it now.
        if (m_state != PlayState::Idle && !m_started && !m_loadedPath.empty() &&
            m_state != PlayState::Paused)
            startCurrent(0);
        break;
    case T::Error:
        if (m_state != PlayState::Idle && e.path == m_loadedPath)
            failCurrent("Can't play " + (m_queue.current() ? m_queue.current()->title : e.path));
        else if (e.path == m_preloadPath) {
            m_preloadPath.clear();
            m_preloadRequested = true; // do not retry a bad file; it will be fetched on demand
        }
        break;
    case T::None:
        break;
    }
}

void MusicPlayer::poll()
{
    pumpEngine();
    superviseEngine();
    std::deque<FetchResult> results;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        results.swap(m_fetchResults);
    }
    for (const FetchResult& r : results)
        applyFetchResult(r);
    if (m_state == PlayState::Playing && m_started && now() - m_lastProgressMs >= kProgressMs) {
        m_lastProgressMs = now();
        report(ReportKind::Progress, false);
    }
    if (m_queueDirty && m_state != PlayState::Idle && now() - m_lastSaveMs >= kSaveMs)
        saveQueueNow();
}

// ---- controls -----------------------------------------------------------------------------------

void MusicPlayer::playTracks(std::vector<Track> tracks, int startIndex, bool shuffle)
{
    if (tracks.empty())
        return;
    finishPlayback(true);
    if (shuffle)
        m_queue.setShuffle(true);
    m_queue.set(std::move(tracks), startIndex);
    m_queueFinished = false;
    m_failures = 0;
    markQueueDirty();
    startCurrent(0);
}

void MusicPlayer::pause()
{
    if (m_state != PlayState::Playing)
        return;
    sendLine("pause");
    markQueueDirty();
    setState(PlayState::Paused);
    report(ReportKind::Progress, true);
}

void MusicPlayer::resume()
{
    if (m_state != PlayState::Paused)
        return;
    sendLine("resume");
    setState(PlayState::Playing);
    report(ReportKind::Progress, false);
}

void MusicPlayer::togglePause()
{
    if (m_state == PlayState::Playing)
        pause();
    else if (m_state == PlayState::Paused)
        resume();
}

void MusicPlayer::seekBy(int seconds)
{
    if (!m_started || m_state == PlayState::Idle)
        return;
    double target = m_position + seconds;
    if (target < 0)
        target = 0;
    if (m_duration > 0 && target > m_duration - 1)
        target = std::max(0.0, m_duration - 1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", target);
    sendLine(std::string("seek ") + buf);
    m_position = target;
    report(ReportKind::Progress, m_state == PlayState::Paused);
}

void MusicPlayer::next()
{
    if (m_queue.empty())
        return;
    if (m_started)
        report(ReportKind::Stopped, false);
    if (!m_queue.skipNext()) {
        finishPlayback(false);
        return;
    }
    startCurrent(0);
}

void MusicPlayer::previous()
{
    if (m_queue.empty())
        return;
    if (m_started && m_position > 3.0) { // well into the track: restart it
        sendLine("seek 0");
        m_position = 0;
        return;
    }
    if (m_started)
        report(ReportKind::Stopped, false);
    m_queue.skipPrevious(); // at the very start this keeps the track and restarts it
    startCurrent(0);
}

void MusicPlayer::jumpTo(int position)
{
    if (!m_queue.jumpTo(position))
        return;
    if (m_started)
        report(ReportKind::Stopped, false);
    startCurrent(0);
}

void MusicPlayer::setShuffle(bool on)
{
    m_queue.setShuffle(on);
    markQueueDirty();
    invalidatePreload();
}

void MusicPlayer::cycleRepeat()
{
    markQueueDirty();
    const Repeat r = m_queue.repeat();
    m_queue.setRepeat(r == Repeat::Off   ? Repeat::All
                      : r == Repeat::All ? Repeat::One
                                         : Repeat::Off);
    invalidatePreload();
}

void MusicPlayer::playNext(Track track)
{
    markQueueDirty();
    m_queue.playNext(std::move(track));
    invalidatePreload();
}

void MusicPlayer::append(Track track)
{
    markQueueDirty();
    m_queue.append(std::move(track));
    invalidatePreload();
}

bool MusicPlayer::removeAt(int position)
{
    const bool removed = m_queue.removeAt(position);
    if (removed) {
        markQueueDirty();
        invalidatePreload();
    }
    return removed;
}

void MusicPlayer::stop()
{
    finishPlayback(true);
}

void MusicPlayer::restoreQueue(const SavedQueue& saved)
{
    if (m_state != PlayState::Idle || saved.tracks.empty())
        return;
    m_queue.restore(saved);
    m_queueFinished = false;
    m_position = saved.seconds;
    if (const Track* t = m_queue.current())
        m_duration = t->durationSeconds();
}

void MusicPlayer::resumeSaved()
{
    if (m_state != PlayState::Idle || m_queue.empty())
        return;
    m_queueFinished = false;
    m_failures = 0;
    startCurrent(m_position);
}

PlayerView MusicPlayer::view() const
{
    PlayerView v;
    v.state = m_state;
    if (const Track* t = m_queue.current())
        v.track = *t;
    v.position = m_position;
    v.duration = m_duration;
    v.shuffle = m_queue.shuffle();
    v.repeat = m_queue.repeat();
    v.queuePosition = m_queue.position();
    v.queueSize = m_queue.size();
    v.message = m_message;
    v.resumable = m_state == PlayState::Idle && !m_queue.empty() && !m_queueFinished;
    return v;
}

// ---- worker threads -----------------------------------------------------------------------------

void MusicPlayer::fetchLoop()
{
    for (;;) {
        FetchJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopWorkers || !m_fetchJobs.empty(); });
            if (m_stopWorkers)
                return;
            // The current track beats a preload that was queued before it.
            auto it = std::find_if(m_fetchJobs.begin(), m_fetchJobs.end(),
                                   [](const FetchJob& j) { return !j.forNext; });
            if (it == m_fetchJobs.end())
                it = m_fetchJobs.begin();
            job = *it;
            m_fetchJobs.erase(it);
        }
        FetchResult result{job.serial, job.forNext, job.track, {}};
        if (job.cancel->load()) {
            continue;
        } else if (m_hooks.resolve) {
            result.resolved = m_hooks.resolve(job.track, *job.cancel);
        } else {
            result.resolved.error = "No source";
        }
        if (job.cancel->load())
            continue;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_fetchResults.push_back(std::move(result));
    }
}

void MusicPlayer::reportLoop()
{
    for (;;) {
        ReportJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] {
                return m_stopWorkers || !m_reportJobs.empty() || m_hasPendingSave;
            });
            if (m_hasPendingSave) {
                std::string text;
                text.swap(m_pendingSave);
                m_hasPendingSave = false;
                lock.unlock();
                if (m_hooks.saveQueue)
                    m_hooks.saveQueue(text);
                continue;
            }
            if (m_reportJobs.empty())
                return; // stopping and drained
            job = std::move(m_reportJobs.front());
            m_reportJobs.pop_front();
            // Progress reports pile up while the network is slow; only the newest matters.
            while (job.kind == ReportKind::Progress && !m_reportJobs.empty() &&
                   m_reportJobs.front().kind == ReportKind::Progress &&
                   m_reportJobs.front().track.id == job.track.id) {
                job = std::move(m_reportJobs.front());
                m_reportJobs.pop_front();
            }
        }
        if (m_hooks.report)
            m_hooks.report(job.kind, job.track, job.ticks, job.paused, job.playSessionId);
    }
}

} // namespace music
} // namespace miyoofin
