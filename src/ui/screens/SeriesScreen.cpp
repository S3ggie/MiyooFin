#include "SeriesScreen.hpp"
#include "SeriesScreenInternal.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../cache/ImageCache.hpp"
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstring>

namespace miyoofin {

SeriesScreen::SeriesScreen(const Session& session, const MediaItem& series,
                           std::shared_ptr<DownloadManager> downloads, bool networkOffline,
                           std::vector<MediaItem> cachedSeasons, bool downloadedOnly,
                           std::shared_ptr<library::LibraryCoordinator> libraryCoordinator,
                           std::shared_ptr<library::LibraryQuery> libraryQuery)
    : m_session(session), m_series(series), m_downloads(std::move(downloads)),
      m_libraryCoordinator(std::move(libraryCoordinator)), m_libraryQuery(std::move(libraryQuery)),
      m_networkOffline(networkOffline), m_downloadedOnly(downloadedOnly),
      m_seasons(std::move(cachedSeasons))
{
    if (m_libraryCoordinator && !m_libraryQuery)
        m_libraryQuery = m_libraryCoordinator->query();
    if (!m_seasons.empty())
        m_loadState = LoadState::Ready;
}

void SeriesScreen::enter()
{
    UiDiagnostics::Scope scope("SeriesScreen::enter");
    printf("[SeriesScreen] enter series=%s\n", m_series.title.c_str());
    if (m_seasons.empty()) {
        // ScreenStack::push calls enter() synchronously on the SDL thread.
        // Catalog reads can parse a large file (and the offline projection also
        // snapshots DownloadManager), so leave this screen immediately visible
        // and let the existing fetch worker provide cached seasons afterwards.
        m_loadState = LoadState::Loading;
        fetchSeasons(true);
    } else {
        // The Home hierarchy worker already supplied a filtered in-memory
        // cache.  Keep it interactive while the normal network refresh runs.
        fetchSeasons();
    }
    tryLoadSeriesArtwork();
}
SeriesScreen::~SeriesScreen()
{
    leave();
    if (m_fetchThread.joinable())
        m_fetchThread.join();
    if (m_artworkThread.joinable())
        m_artworkThread.join();
    freePreparedArtwork(m_seriesArtworkSurface);
    for (auto& entry : m_seasonArtworkSurfaces)
        freePreparedArtwork(entry.second);
}

std::vector<MediaItem>
SeriesScreen::replaceSeasonsKeepingSelection(const std::vector<MediaItem>& current,
                                             std::vector<MediaItem> fresh, int& selected)
{
    std::string selectedId =
        current.empty() ? "" : current[std::min(std::max(selected, 0), (int)current.size() - 1)].id;
    int n = 0;
    for (; n < (int)fresh.size() && fresh[n].id != selectedId; n++)
        ;
    selected = n < (int)fresh.size() ? n : 0;
    return fresh;
}

void SeriesScreen::leave()
{
    if (m_shutdownSignalled.exchange(true, std::memory_order_acq_rel))
        return;
    UiDiagnostics::Scope scope("SeriesScreen::workerShutdown");
    printf("[SeriesScreen] leave series=%s\n", m_series.title.c_str());
    m_fetchCancelled.store(true, std::memory_order_release);
    m_catalogCancellation->store(true, std::memory_order_release);
    const auto hierarchyRequest = m_hierarchyRequest.load(std::memory_order_acquire);
    if (hierarchyRequest != 0 && m_libraryCoordinator)
        m_libraryCoordinator->cancelHierarchyRequest(hierarchyRequest);
    m_artworkCancelled.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> g(m_artworkMutex);
        m_artworkStop = true;
    }
    m_artworkCv.notify_one();
}

void SeriesScreen::update(Uint32 /*dt*/)
{
    std::vector<MediaItem> cached;
    bool cachedDone = false;
    {
        std::lock_guard<std::mutex> g(m_fetchMutex);
        if (m_cachedSeasonsDone) {
            cached = std::move(m_cachedSeasons);
            m_cachedSeasonsDone = false;
            cachedDone = true;
        }
    }
    if (cachedDone) {
        UiDiagnostics::Scope scope("SeriesScreen::publishCachedSeasons");
        m_seasons = std::move(cached);
        // Offline completion and a usable online cache are immediately
        // interactive; an empty online cache continues showing the loader
        // while the network request is in flight.
        if (m_networkOffline || !m_seasons.empty())
            m_loadState = LoadState::Ready;
    }
    bool fetchDone;
    {
        std::lock_guard<std::mutex> g(m_fetchMutex);
        fetchDone = m_fetchDone;
    }
    if (fetchDone) {
        UiDiagnostics::Scope scope("SeriesScreen::publishSeasonResult");
        std::vector<MediaItem> fresh;
        std::string err;
        bool ok;
        {
            std::lock_guard<std::mutex> g(m_fetchMutex);
            ok = m_fetchOk;
            fresh = std::move(m_fetchSeasons);
            err = m_fetchError;
            m_fetchDone = false;
        }
        if (m_fetchThread.joinable())
            m_fetchThread.join();
        if (ok) {
            if (!m_networkOffline)
                m_seasons =
                    replaceSeasonsKeepingSelection(m_seasons, std::move(fresh), m_selectedSeason);
            m_loadState = LoadState::Ready;
        } else if (m_seasons.empty()) {
            m_loadState = LoadState::Error;
            m_error = err;
        }
    }
    {
        UiDiagnostics::Scope scope("SeriesScreen::publishArtworkResult");
        std::lock_guard<std::mutex> g(m_artworkMutex);
        for (auto& entry : m_artworkCompleted) {
            if (entry.first.rfind("series:", 0) == 0) {
                m_seriesArtwork = std::move(entry.second);
                freePreparedArtwork(m_seriesArtworkSurface);
                m_seriesArtworkSurface =
                    prepareArtworkSurface(m_seriesArtwork, SHOW_X, SHOW_Y, SHOW_W, SHOW_H);
            } else {
                m_seasonArtwork[entry.first] = std::move(entry.second);
                PreparedArtwork& prepared = m_seasonArtworkSurfaces[entry.first];
                freePreparedArtwork(prepared);
                prepared =
                    prepareArtworkSurface(m_seasonArtwork[entry.first], 0, 0, POSTER_W, POSTER_H);
            }
        }
        m_artworkCompleted.clear();
    }
    {
        UiDiagnostics::Scope scope("SeriesScreen::queueVisibleArtwork");
        tryLoadOneVisibleSeasonArtwork();
    }
}

// -------------------------------------------------------------------
// Grid navigation helpers
// -------------------------------------------------------------------

/// Clamp gridScroll so the selected season's row is visible.
}
