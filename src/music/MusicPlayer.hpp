#ifndef MIYOOFIN_MUSIC_PLAYER_HPP
#define MIYOOFIN_MUSIC_PLAYER_HPP

#include "MusicApi.hpp"
#include "MusicQueue.hpp"
#include "MusicTypes.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace miyoofin {
namespace music {

/// One line from the audio engine (see player/audio.c for the protocol).
struct EngineEvent
{
    enum class Type
    {
        None,
        Ready,
        Started,
        Pos,
        Ended,
        Idle,
        Error
    } type = Type::None;
    std::string path;
    double position = 0, duration = 0;
    bool paused = false;
};
EngineEvent parseEngineEvent(const std::string& line);

/// A track's playable file: a completed download, a cached stream, or a fresh fetch.
struct ResolvedTrack
{
    bool ok = false;
    std::string path, error;
    bool local = false; // a downloaded copy (reported as direct play)
};

/// How MusicPlayer reaches the outside world. Everything is injectable so the player is
/// tested on the host with a fake engine script and no network.
struct PlayerHooks
{
    /// Produces a playable file for the track; runs on the fetch thread and may block.
    std::function<ResolvedTrack(const Track&, const std::atomic<bool>& cancelled)> resolve;
    /// Delivers a playback report; runs on the report thread and may block.
    std::function<void(ReportKind, const Track&, std::int64_t positionTicks, bool paused,
                       const std::string& playSessionId)>
        report;
    /// Called on the UI thread when playing state changes (for the stay-awake file).
    std::function<void(bool awake)> setAwake;
    /// Persists the queue (serialized text; "" = nothing to resume). Runs on the report thread.
    std::function<void(const std::string& text)> saveQueue;
};

struct PlayerOptions
{
    std::string enginePath;             // the miyoofin-audio binary
    std::string engineLog;              // its stderr goes here ("" = discard)
    std::string runDir;                 // where queue.txt lives ("" = no persistence)
    std::vector<std::string> engineEnv; // extra NAME=value entries for the child
    std::vector<std::string> engineUnsetEnv;
};

enum class PlayState
{
    Idle,    // nothing loaded
    Loading, // the current track is being fetched / opened
    Playing,
    Paused
};

struct PlayerView
{
    PlayState state = PlayState::Idle;
    Track track; // the current track (id empty when idle)
    double position = 0, duration = 0;
    bool shuffle = false;
    Repeat repeat = Repeat::Off;
    int queuePosition = -1, queueSize = 0;
    std::string message;    // last problem worth showing ("" = none)
    bool resumable = false; // idle, but a saved queue can be picked up where it stopped
};

/// Owns the play queue, the audio-engine child process, and the background work (fetching
/// tracks, reporting plays). The UI thread calls the control methods and poll() each frame;
/// nothing here blocks it: engine I/O is non-blocking and fetches/reports run on two worker
/// threads. One instance lives for the whole app run.
class MusicPlayer
{
  public:
    MusicPlayer(PlayerOptions options, PlayerHooks hooks);
    ~MusicPlayer();
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // ---- controls (UI thread) -----------------------------------------------------------
    void playTracks(std::vector<Track> tracks, int startIndex, bool shuffle);
    void togglePause();
    void pause();
    void resume();
    void seekBy(int seconds);
    void next();
    void previous();
    void jumpTo(int position);
    void setShuffle(bool on);
    void cycleRepeat();
    void playNext(Track track);
    void append(Track track);
    bool removeAt(int position);
    void stop();
    /// Loads a queue saved by an earlier run; it waits (idle, resumable) until resumeSaved().
    void restoreQueue(const SavedQueue& saved);
    void resumeSaved();
    /// Blocks up to `timeoutMs` for the engine to exit (app exit / before video playback).
    void shutdown(int timeoutMs);

    /// Per-frame housekeeping: engine events, fetch results, supervision, reporting.
    void poll();

    /// Files the engine has open or has been told to open next (a cache clean-up must keep them).
    std::vector<std::string> inUsePaths() const;

    PlayerView view() const;
    const MusicQueue& queue() const
    {
        return m_queue;
    }
    pid_t enginePid() const
    {
        return m_pid;
    }
    bool active() const
    {
        return m_state != PlayState::Idle;
    }

    /// Test seam: the monotonic clock in milliseconds.
    std::function<std::int64_t()> now;

  private:
    struct FetchJob
    {
        Track track;
        std::uint64_t serial;
        bool forNext; // a preload for the following track
        std::shared_ptr<std::atomic<bool>> cancel;
    };
    struct FetchResult
    {
        std::uint64_t serial;
        bool forNext;
        Track track;
        ResolvedTrack resolved;
    };
    struct ReportJob
    {
        ReportKind kind;
        Track track;
        std::int64_t ticks;
        bool paused;
        std::string playSessionId;
    };

    void startCurrent(double startSec);
    void seekTo(double seconds);
    void requestFetch(const Track& track, bool forNext);
    void maybePreloadNext();
    void sendLine(const std::string& line);
    bool ensureEngine();
    void killEngine();
    void pumpEngine();
    void handleEvent(const EngineEvent& e);
    void applyFetchResult(const FetchResult& r);
    void superviseEngine();
    void report(ReportKind kind, bool paused);
    void finishPlayback(bool report, bool naturalEnd = false);
    void markQueueDirty()
    {
        m_queueDirty = true;
    }
    void saveQueueNow();
    void setState(PlayState state);
    void fetchLoop();
    void reportLoop();
    void invalidatePreload();
    void failCurrent(const std::string& message);

    PlayerOptions m_options;
    PlayerHooks m_hooks;
    MusicQueue m_queue;

    // engine process
    pid_t m_pid = -1;
    int m_toEngine = -1, m_fromEngine = -1;
    std::string m_outBuf, m_inBuf;
    std::int64_t m_lastEventMs = 0, m_spawnedMs = 0;
    std::deque<std::int64_t> m_crashes;
    bool m_engineReady = false;

    // playback state
    PlayState m_state = PlayState::Idle;
    std::uint64_t m_serial = 0; // identifies the current load; stale fetches are ignored
    double m_position = 0, m_duration = 0;
    std::string m_message;
    std::string m_loadedPath;  // what the engine was told to play
    std::string m_preloadPath; // and what it was told to play next
    std::string m_preloadTrackId;
    bool m_preloadRequested = false;
    std::string m_playSessionId;
    bool m_started = false; // the engine reported `started` for the current track
    std::int64_t m_lastProgressMs = 0;
    bool m_awake = false;
    double m_pendingStart = 0;
    bool m_queueDirty = false, m_queueFinished = false, m_pauseAfterStart = false;
    std::int64_t m_loadSentMs = 0;
    std::int64_t m_lastSaveMs = 0;
    bool m_hasPendingSave = false;
    std::string m_pendingSave;
    int m_failures = 0; // consecutive tracks that could not be played

    // workers
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<FetchJob> m_fetchJobs;
    std::deque<FetchResult> m_fetchResults;
    std::deque<ReportJob> m_reportJobs;
    std::shared_ptr<std::atomic<bool>> m_cancelFetch;
    bool m_stopWorkers = false;
    std::thread m_fetchThread, m_reportThread;
};

} // namespace music
} // namespace miyoofin

#endif
