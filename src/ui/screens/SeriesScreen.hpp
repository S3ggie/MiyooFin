#ifndef MIYOOFIN_SERIES_SCREEN_HPP
#define MIYOOFIN_SERIES_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../net/Session.hpp"
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

    // Data
    Session       m_session;
    MediaItem     m_series;
    std::vector<MediaItem> m_seasons;
    int           m_selectedSeason = 0;
    LoadState     m_loadState = LoadState::Loading;
    std::string   m_error;

    // Scroll state for season list
    int           m_seasonScroll = 0;
};

} // namespace miyoofin

#endif // MIYOOFIN_SERIES_SCREEN_HPP
