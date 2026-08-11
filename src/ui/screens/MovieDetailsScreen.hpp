#ifndef MIYOOFIN_MOVIE_DETAILS_SCREEN_HPP
#define MIYOOFIN_MOVIE_DETAILS_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/Session.hpp"
#include "../../download/DownloadManager.hpp"
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace miyoofin {

/// Screen displaying full details for a selected movie.
/// Two-panel layout: left = large poster, right = title, metadata,
/// genres, scrollable overview, and Play/Download action buttons.
class MovieDetailsScreen : public Screen {
public:
    MovieDetailsScreen(const Session &session, const MediaItem &movie, std::shared_ptr<DownloadManager> downloads={},
                       std::shared_ptr<const DecodedImage> gridArtwork={});
    ~MovieDetailsScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;
    const char *diagnosticName() const override { return "MovieDetailsScreen"; }
    bool deferDestruction() const override { return true; }

private:
    /// Which action button is focused.
    enum class ActionButton { Play, Download };

    /// Prepare download state and artwork without touching the SDL thread.
    void prepareWorker();
    DecodedImage loadMovieArtwork();
    void renderContent(SDL_Surface *fb);

    // ----- Data -----
    Session   m_session;
    MediaItem m_movie;
    std::shared_ptr<DownloadManager> m_downloads; std::uint64_t m_planId=0; bool m_confirmDownload=false;

    // ----- Movie poster artwork -----
    DecodedImage m_movieArtwork;
    SDL_Surface *m_movieArtworkSurface = nullptr;
    std::shared_ptr<const DecodedImage> m_gridArtwork;
    SDL_Surface *m_gridArtworkSurface = nullptr;
    std::thread m_prepareThread;
    std::mutex m_prepareMutex;
    DecodedImage m_preparedArtwork;
    std::vector<std::string> m_preparedOverviewLines;
    std::uint64_t m_preparedPlanId = 0;
    bool m_preparedPlanReady = false;
    bool m_preparedArtworkReady = false;
    bool m_preparedOverviewReady = false;
    bool m_prepareStarted = false;
    std::atomic<bool> m_prepareCancelled{false};
    std::atomic<bool> m_shutdownSignalled{false};

    // ----- Overview scroll -----
    int m_overviewScroll = 0;
    std::vector<std::string> m_overviewLines;
    bool m_firstRender = true;
    DownloadPlanSnapshot m_planSnapshot;

    // ----- Focus state -----
    ActionButton m_actionBtn = ActionButton::Play;

    // Main-thread-only playback return bookkeeping.
    bool m_playbackResultPending = false;
    int  m_playbackResultDelayUpdates = 0;
};

} // namespace miyoofin

#endif // MIYOOFIN_MOVIE_DETAILS_SCREEN_HPP
