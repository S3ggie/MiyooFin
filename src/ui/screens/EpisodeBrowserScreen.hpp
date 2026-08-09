#ifndef MIYOOFIN_EPISODE_BROWSER_SCREEN_HPP
#define MIYOOFIN_EPISODE_BROWSER_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/Session.hpp"
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {

/// Screen displaying episodes for a selected season, fetched from Jellyfin.
/// Two-panel layout: left = scrollable text list, right = placeholder
/// thumbnail + metadata + overview + Play/Download buttons.
class EpisodeBrowserScreen : public Screen {
public:
    EpisodeBrowserScreen(const Session &session,
                         const MediaItem &series,
                         const MediaItem &season,
                         const std::string &initialEpisodeId = "");
    ~EpisodeBrowserScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

    /// Find the index of an episode by its ID in a list.
    /// Returns -1 if not found.  (Public for testing.)
    static int findEpisodeIndex(const std::vector<MediaItem> &episodes,
                                const std::string &episodeId);

private:
    enum class LoadState { Loading, Ready, Error };

    /// Which UI area currently holds input focus.
    enum class FocusArea { EpisodeList, ActionButtons };

    /// Which action button is focused (only meaningful when
    /// focus == ActionButtons).
    enum class ActionButton { Play, Download };

    /// Fetch episodes from the server.  Called on first enter() and
    /// on retry from Error state.
    void fetchEpisodes();

    /// Ensure the selected episode is visible in the scrolled list.
    void clampListScroll();

    /// Load the Primary artwork for the currently selected episode.
    /// Never performs HTTP on the main thread (B5g1a).
    void tryLoadSelectedEpisodeArtwork();

    // ----- Artwork worker types (B5g1a) -----

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
        bool        success = false;
    };

    /// Background artwork download worker loop (runs on worker thread).
    void artworkWorkerLoop();

    // ----- Data -----
    Session       m_session;
    MediaItem     m_series;
    MediaItem     m_season;
    std::string   m_initialEpisodeId;
    std::vector<MediaItem> m_episodes;
    LoadState     m_loadState = LoadState::Loading;
    std::string   m_error;

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

    // ----- Artwork worker state (B5g1a) -----
    std::thread              m_workerThread;
    std::mutex               m_workerMutex;
    std::condition_variable  m_workerCv;
    bool                     m_workerStop = false;

    /// Latest pending job (latest-selection-wins, no queue).
    ArtworkJob               m_workerPending;
    bool                     m_workerHasPending = false;

    /// Key of the job currently being executed by the worker.
    /// Prevents duplicate submissions for the same artwork key.
    std::string              m_workerInProgressKey;

    /// Latest completion signal (consumed once by main thread).
    ArtworkCompletion        m_workerCompletion;
    bool                     m_workerHasCompletion = false;

    /// Artwork keys that failed download; not retried for this screen.
    std::set<std::string>    m_failedKeys;

    // ----- Initial episode focus (B5e3b) -----
    bool          m_initialSelectionApplied = false;
};

} // namespace miyoofin

#endif // MIYOOFIN_EPISODE_BROWSER_SCREEN_HPP
