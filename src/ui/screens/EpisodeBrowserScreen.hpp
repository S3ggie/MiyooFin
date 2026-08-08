#ifndef MIYOOFIN_EPISODE_BROWSER_SCREEN_HPP
#define MIYOOFIN_EPISODE_BROWSER_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/Session.hpp"
#include <string>
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
    ~EpisodeBrowserScreen() override = default;

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
    void tryLoadSelectedEpisodeArtwork();

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

    // ----- Selected-episode artwork (B5e2c) -----
    DecodedImage  m_episodeArtwork;
    std::string   m_episodeArtworkKey;
    bool          m_episodeArtworkAttempted = false;

    // ----- Initial episode focus (B5e3b) -----
    bool          m_initialSelectionApplied = false;
};

} // namespace miyoofin

#endif // MIYOOFIN_EPISODE_BROWSER_SCREEN_HPP
