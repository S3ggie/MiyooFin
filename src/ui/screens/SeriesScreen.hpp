#ifndef MIYOOFIN_SERIES_SCREEN_HPP
#define MIYOOFIN_SERIES_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/Session.hpp"
#include "../../download/DownloadManager.hpp"
#include <memory>
#include <map>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace miyoofin {

/// Screen displaying a series' seasons fetched from the Jellyfin server.
/// This is a minimal milestone screen: no episode fetching, no artwork.
class SeriesScreen : public Screen {
public:
    SeriesScreen(const Session &session, const MediaItem &series, std::shared_ptr<DownloadManager> downloads={}, bool networkOffline=false,
                 std::vector<MediaItem> cachedSeasons={}, bool downloadedOnly=false);
    ~SeriesScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;
    const char *diagnosticName() const override { return "SeriesScreen"; }
    bool deferDestruction() const override { return true; }
    bool diagnosticSeasonsReady() const { return !m_seasons.empty(); }
    const std::vector<MediaItem> &diagnosticSeasons() const { return m_seasons; }
    // Shared by the async completion path and focused no-network tests.
    static std::vector<MediaItem> replaceSeasonsKeepingSelection(const std::vector<MediaItem> &current,
                                                                  std::vector<MediaItem> fresh, int &selected);

private:
    enum class LoadState { Loading, Ready, Error };

    // Performs catalog/projection work and network fetching on the existing
    // bounded fetch worker.  enter() must not read the catalog on the UI thread.
    void fetchSeasons(bool loadCachedSeasons = false);
    void clampGridScroll();
    void tryLoadSeriesArtwork();
    void tryLoadOneVisibleSeasonArtwork();
    void artworkWorkerLoop();
    void queueArtwork(const std::string &key, const MediaItem &item, int width, int height, bool series);

    struct PreparedArtwork {
        SDL_Surface *surface = nullptr;
        int x = 0;
        int y = 0;
    };
    static PreparedArtwork prepareArtworkSurface(const DecodedImage &image, int boxX, int boxY, int boxW, int boxH);
    static void freePreparedArtwork(PreparedArtwork &artwork);
    void drawSeasonPoster(SDL_Surface *fb, int x, int y, int w, int h,
                          const MediaItem &season, bool selected, const PreparedArtwork *artwork);

    static std::string seasonArtworkKey(const MediaItem &season);

    // Data
    Session       m_session;
    MediaItem     m_series;
    std::shared_ptr<DownloadManager> m_downloads;
    bool m_networkOffline=false, m_downloadedOnly=false;
    std::uint64_t m_planId = 0; bool m_confirmDownload = false; bool m_planWholeSeries = false;
    std::vector<MediaItem> m_seasons;
    int           m_selectedSeason = 0;
    LoadState     m_loadState = LoadState::Loading;
    std::string   m_error;
    std::thread m_fetchThread; std::mutex m_fetchMutex; bool m_fetchDone=false, m_fetchOk=false, m_cachedSeasonsDone=false; std::vector<MediaItem> m_fetchSeasons, m_cachedSeasons; std::string m_fetchError; std::atomic<bool> m_fetchCancelled{false};

    // Grid scroll state (row offset, not item index)
    int           m_seasonScroll = 0;  // kept for reset compatibility
    int           m_gridScroll = 0;    // visible top-row index into the grid
    int           m_overviewScroll = 0; // first visible wrapped overview line

    // Series poster artwork (B5e1c2a)
    DecodedImage  m_seriesArtwork;
    PreparedArtwork m_seriesArtworkSurface;
    bool          m_seriesArtworkAttempted = false;

    // Season poster artwork (B5e1c2b)
    std::map<std::string, DecodedImage> m_seasonArtwork;
    std::map<std::string, PreparedArtwork> m_seasonArtworkSurfaces;
    struct ArtworkJob { std::string key; MediaItem item; int width, height; bool series; };
    std::thread m_artworkThread; std::mutex m_artworkMutex; std::condition_variable m_artworkCv;
    std::vector<ArtworkJob> m_artworkJobs;
    std::map<std::string, DecodedImage> m_artworkCompleted;
    bool m_artworkStop=false; std::atomic<bool> m_artworkCancelled{false};
    std::atomic<bool> m_shutdownSignalled{false};
};

} // namespace miyoofin

#endif // MIYOOFIN_SERIES_SCREEN_HPP
