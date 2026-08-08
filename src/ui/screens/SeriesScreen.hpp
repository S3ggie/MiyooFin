#ifndef MIYOOFIN_SERIES_SCREEN_HPP
#define MIYOOFIN_SERIES_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/Session.hpp"
#include <map>
#include <string>
#include <vector>

namespace miyoofin {

/// Screen displaying a series' seasons fetched from the Jellyfin server.
/// This is a minimal milestone screen: no episode fetching, no artwork.
class SeriesScreen : public Screen {
public:
    SeriesScreen(const Session &session, const MediaItem &series);
    ~SeriesScreen() override = default;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

private:
    enum class LoadState { Loading, Ready, Error };

    void fetchSeasons();
    void clampGridScroll();
    void tryLoadSeriesArtwork();
    void tryLoadOneVisibleSeasonArtwork();

    static std::string seasonArtworkKey(const MediaItem &season);

    // Data
    Session       m_session;
    MediaItem     m_series;
    std::vector<MediaItem> m_seasons;
    int           m_selectedSeason = 0;
    LoadState     m_loadState = LoadState::Loading;
    std::string   m_error;

    // Grid scroll state (row offset, not item index)
    int           m_seasonScroll = 0;  // kept for reset compatibility
    int           m_gridScroll = 0;    // visible top-row index into the grid
    int           m_overviewScroll = 0; // first visible wrapped overview line

    // Series poster artwork (B5e1c2a)
    DecodedImage  m_seriesArtwork;
    bool          m_seriesArtworkAttempted = false;

    // Season poster artwork (B5e1c2b)
    std::map<std::string, DecodedImage> m_seasonArtwork;
};

} // namespace miyoofin

#endif // MIYOOFIN_SERIES_SCREEN_HPP
