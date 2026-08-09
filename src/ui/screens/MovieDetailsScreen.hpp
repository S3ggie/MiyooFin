#ifndef MIYOOFIN_MOVIE_DETAILS_SCREEN_HPP
#define MIYOOFIN_MOVIE_DETAILS_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/Session.hpp"
#include <string>
#include <vector>

namespace miyoofin {

/// Screen displaying full details for a selected movie.
/// Two-panel layout: left = large poster, right = title, metadata,
/// genres, scrollable overview, and Play/Download action buttons.
class MovieDetailsScreen : public Screen {
public:
    MovieDetailsScreen(const Session &session, const MediaItem &movie);
    ~MovieDetailsScreen() override = default;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

private:
    /// Which action button is focused.
    enum class ActionButton { Play, Download };

    /// Load the Primary artwork for the movie.
    void tryLoadMovieArtwork();

    // ----- Data -----
    Session   m_session;
    MediaItem m_movie;

    // ----- Movie poster artwork -----
    DecodedImage m_movieArtwork;
    bool         m_movieArtworkAttempted = false;

    // ----- Overview scroll -----
    int m_overviewScroll = 0;

    // ----- Focus state -----
    ActionButton m_actionBtn = ActionButton::Play;

    // Main-thread-only playback return bookkeeping.
    bool m_playbackResultPending = false;
    int  m_playbackResultDelayUpdates = 0;
};

} // namespace miyoofin

#endif // MIYOOFIN_MOVIE_DETAILS_SCREEN_HPP
