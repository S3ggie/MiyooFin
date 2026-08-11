#ifndef MIYOOFIN_EPISODE_BROWSER_SCREEN_HPP
#define MIYOOFIN_EPISODE_BROWSER_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/Session.hpp"
#include "../../download/DownloadManager.hpp"
#include <memory>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

namespace miyoofin {

/// Screen displaying episodes for a selected season, fetched from Jellyfin.
/// Two-panel layout: left = scrollable text list, right = placeholder
/// thumbnail + metadata + overview + Play/Download buttons.
class EpisodeBrowserScreen : public Screen {
public:
    EpisodeBrowserScreen(const Session &session,
                         const MediaItem &series,
                         const MediaItem &season,
                         const std::string &initialEpisodeId = "", std::shared_ptr<DownloadManager> downloads={}, bool offline=false);
    ~EpisodeBrowserScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;
    const char *diagnosticName() const override { return "EpisodeBrowserScreen"; }
    bool deferDestruction() const override { return true; }

    /// Find the index of an episode by its ID in a list.
    /// Returns -1 if not found.  (Public for testing.)
    static int findEpisodeIndex(const std::vector<MediaItem> &episodes,
                                const std::string &episodeId);

    static constexpr int PREFETCH_AHEAD = 8;
    static constexpr int PREFETCH_BEHIND = 3;

    /// Return the next index in selected/ahead/behind priority order,
    /// excluding unavailable indices.  Examines at most 12 indices.
    static int nextPrefetchIndex(int selected, int total,
                                 const std::set<int> &unavailable);

    /// Advance the deterministic post-playback resume countdown.
    /// Returns true exactly when the caller should resume prefetching.
    static bool advancePrefetchResume(bool &pending, int &delayUpdates);
    static constexpr bool hasSeparateDownloadActions() { return true; }

private:
    enum class LoadState { Loading, Ready, Error };

    /// Which UI area currently holds input focus.
    enum class FocusArea { EpisodeList, ActionButtons };

    /// Which action button is focused (only meaningful when
    /// focus == ActionButtons).
    enum class ActionButton { Play, DownloadEpisode, DownloadSeason };

    /// Fetch episodes from the server.  Called on first enter() and
    /// on retry from Error state.
    void fetchEpisodes(bool loadCachedEpisodes = false);
    void publishEpisodes(std::vector<MediaItem> episodes);

    /// Ensure the selected episode is visible in the scrolled list.
    void clampListScroll();

    /// Load the Primary artwork for the currently selected episode.
    /// Never performs HTTP on the main thread (B5g1a).
    void tryLoadSelectedEpisodeArtwork();

    // ----- Artwork worker types -----

    /// Immutable job description copied to the background worker.
    struct ArtworkJob {
        std::string itemId;
        std::string imageTag;
        std::string artworkKey;   ///< Stable identity key for completion matching
        int         width  = 288;
        int         height = 162;
    };

    /// Completion signal published by the artwork worker.
    struct ArtworkCompletion {
        std::string artworkKey;
        DecodedImage image;
        bool        success = false;
    };

    /// Background artwork download worker loop (runs on worker thread).
    void artworkWorkerLoop();

    /// Publish current selection to the worker and wake it to recompute.
    void wakeArtworkWorker();

    // ----- Data -----
    Session       m_session;
    MediaItem     m_series;
    MediaItem     m_season;
    std::string   m_initialEpisodeId;
    std::vector<MediaItem> m_episodes;
    std::shared_ptr<DownloadManager> m_downloads; bool m_offline=false; std::uint64_t m_planId=0; bool m_confirmDownload=false, m_planIsSeason=false;
    LoadState     m_loadState = LoadState::Loading;
    std::string   m_error;
    std::thread m_fetchThread; std::mutex m_fetchMutex; bool m_fetchDone=false, m_fetchOk=false, m_cachedEpisodesDone=false; std::vector<MediaItem> m_fetchEpisodes, m_cachedEpisodes; std::string m_fetchError; std::atomic<bool> m_fetchCancelled{false};

    // ----- Navigation state -----
    int           m_selectedEpisode = 0;
    int           m_listScroll = 0;       // index of first visible row
    int           m_overviewScroll = 0;   // first visible wrapped line

    // ----- Focus state -----
    FocusArea     m_focus = FocusArea::EpisodeList;
    ActionButton  m_actionBtn = ActionButton::Play;

    // ----- Selected-episode artwork (B5g1a: non-blocking) -----
    DecodedImage  m_episodeArtwork;
    std::string   m_episodeArtworkKey;

    // ----- Artwork worker state (B5g1b) -----
    std::thread              m_workerThread;
    std::mutex               m_workerMutex;
    std::condition_variable  m_workerCv;
    bool                     m_workerStop = false;
    std::atomic<bool>        m_workerCancelled{false};
    std::atomic<bool>        m_shutdownSignalled{false};

    /// Immutable per-episode job data, rebuilt after fetchEpisodes().
    std::vector<ArtworkJob>  m_artworkJobs;
    int                      m_workerSelected = 0;
    std::uint64_t            m_workerGeneration = 0;
    bool                     m_workerPaused = false;

    // Main-thread-only playback return bookkeeping.  App performs exactly
    // one update between requesting playback and entering external playback.
    bool                     m_prefetchResumePending = false;
    int                      m_prefetchResumeDelayUpdates = 0;
    std::string              m_playbackEpisodeId;

    /// Key of the job currently being executed by the worker.
    /// Prevents duplicate submissions for the same artwork key.
    std::string              m_workerInProgressKey;

    /// Latest completion signal (consumed once by main thread).
    ArtworkCompletion        m_workerCompletion;
    bool                     m_workerHasCompletion = false;
    std::string              m_workerDecodedKey;

    /// Artwork keys that failed download; guarded by m_workerMutex.
    std::set<std::string>    m_failedKeys;

    // ----- Initial episode focus (B5e3b) -----
    bool          m_initialSelectionApplied = false;
};

} // namespace miyoofin

#endif // MIYOOFIN_EPISODE_BROWSER_SCREEN_HPP
