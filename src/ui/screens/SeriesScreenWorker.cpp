#include "SeriesScreen.hpp"
#include "SeriesScreenInternal.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace miyoofin {

void SeriesScreen::fetchSeasons(bool loadCachedSeasons)
{
    if(m_fetchThread.joinable()) { std::lock_guard<std::mutex>g(m_fetchMutex); if(!m_fetchDone)return; m_fetchThread.join(); }
    if(m_seasons.empty()) m_loadState = LoadState::Loading;
    m_error.clear();
    {std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchDone=false;m_cachedSeasonsDone=false;}
    m_fetchCancelled.store(false, std::memory_order_release);
    m_catalogCancellation->store(false, std::memory_order_release);
    std::string id=m_series.id;
    const bool networkOffline=m_networkOffline, downloadedOnly=m_downloadedOnly, loadCached=loadCachedSeasons;
    std::shared_ptr<DownloadManager> downloads=m_downloads;
    const std::shared_ptr<library::LibraryQuery> libraryQuery=m_libraryQuery;
    const std::shared_ptr<library::LibrarySync> librarySync=m_librarySync;
    const std::shared_ptr<std::atomic_bool> cancellation=m_catalogCancellation;
    const MediaItem series=m_series;
    m_fetchThread=std::thread([this,id,networkOffline,downloadedOnly,loadCached,downloads,
                               libraryQuery,librarySync,cancellation,series](){
        if(loadCached) {
            if(m_fetchCancelled.load(std::memory_order_acquire)) return;
            std::vector<MediaItem> cached;
            if (libraryQuery) {
                const library::HierarchyPage result =
                    libraryQuery->seasons(id, cancellation).get();
                if (result.success) cached = result.items;
                if (result.cancelled || result.superseded) {
                    if (networkOffline) return;
                    cached.clear();
                }
            }
            if (downloadedOnly) {
                const DownloadSnapshot snapshot =
                    downloads ? downloads->snapshot() : DownloadSnapshot{};
                std::vector<MediaItem> filtered;
                for (const auto &season : cached) {
                    for (const auto &download : snapshot.items) {
                        const bool complete =
                            download.state == DownloadState::Complete
                            || download.state == DownloadState::LocalOnly
                            || download.state == DownloadState::UpdateAvailable;
                        if (complete && download.itemType == "episode"
                            && download.seriesId == id
                            && download.seasonId == season.id) {
                            filtered.push_back(season);
                            break;
                        }
                    }
                }
                if (filtered.empty()) {
                    for (const auto &download : snapshot.items) {
                        const bool complete =
                            download.state == DownloadState::Complete
                            || download.state == DownloadState::LocalOnly
                            || download.state == DownloadState::UpdateAvailable;
                        if (!complete || download.itemType != "episode"
                            || download.seriesId != id
                            || download.seasonId.empty()) continue;
                        MediaItem season;
                        season.id = download.seasonId;
                        season.type = "season";
                        season.seriesId = id;
                        season.title = download.seasonName.empty()
                            ? "Season " + std::to_string(download.seasonNumber)
                            : download.seasonName;
                        season.indexNumber = download.seasonNumber;
                        bool duplicate = false;
                        for (const auto &existing : filtered)
                            duplicate = duplicate || existing.id == season.id;
                        if (!duplicate) filtered.push_back(std::move(season));
                    }
                }
                cached = std::move(filtered);
            }
            {std::lock_guard<std::mutex>g(m_fetchMutex);m_cachedSeasons=std::move(cached);m_cachedSeasonsDone=true;}
            if(networkOffline) { std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchOk=true;m_fetchDone=true;return; }
        }
        if(m_fetchCancelled.load(std::memory_order_acquire)) return;
        std::vector<MediaItem> v;std::string e;bool ok=false;
        if (librarySync) {
            const library::HierarchyRefreshResult refreshed =
                librarySync->refreshSeasons(series, cancellation).get();
            ok = refreshed.success;
            v = refreshed.items;
            e = refreshed.message;
        } else {
            e = "LibrarySync service unavailable";
        }
        if(ok&&!m_fetchCancelled.load(std::memory_order_acquire)) {
            if (downloadedOnly) {
                const DownloadSnapshot snapshot =
                    downloads ? downloads->snapshot() : DownloadSnapshot{};
                v.erase(std::remove_if(v.begin(), v.end(), [&](const MediaItem &season) {
                    for (const auto &download : snapshot.items) {
                        const bool complete =
                            download.state == DownloadState::Complete
                            || download.state == DownloadState::LocalOnly
                            || download.state == DownloadState::UpdateAvailable;
                        if (complete && download.itemType == "episode"
                            && download.seriesId == id
                            && download.seasonId == season.id) return false;
                    }
                    return true;
                }), v.end());
            }
        }
        std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchOk=ok;m_fetchSeasons=std::move(v);m_fetchError=e;m_fetchDone=true;
    });
}

std::string SeriesScreen::seasonArtworkKey(const MediaItem &season)
{
    auto it = season.imageTags.find("Primary");
    if (it == season.imageTags.end() || it->second.empty())
        return {};
    return season.id + ":" + it->second + ":" + std::to_string(POSTER_W) + "x" + std::to_string(POSTER_H);
}

void SeriesScreen::tryLoadSeriesArtwork()
{
    if (m_seriesArtworkAttempted)
        return;
    m_seriesArtworkAttempted = true;

    // Look for Primary image tag using find(), not operator[]
    auto it = m_series.imageTags.find("Primary");
    if (it == m_series.imageTags.end() || it->second.empty()) {
        printf("[SeriesScreen] Artwork: no Primary tag\n");
        return;
    }

    const std::string &tag = it->second;
    constexpr int w = SHOW_W;   // 160
    constexpr int h = SHOW_H;   // 240

    printf("[SeriesScreen] Artwork: loading %s tag=%s (%dx%d)\n",
           m_series.id.c_str(), tag.c_str(), w, h);

    queueArtwork("series:" + m_series.id + ":" + tag, m_series, w, h, true);
}

void SeriesScreen::queueArtwork(const std::string &key, const MediaItem &item, int width, int height, bool series)
{
    {
        std::lock_guard<std::mutex> g(m_artworkMutex);
        m_artworkJobs.push_back({key, item, width, height, series});
        if (!m_artworkThread.joinable()) m_artworkThread=std::thread(&SeriesScreen::artworkWorkerLoop, this);
    }
    m_artworkCv.notify_one();
}

void SeriesScreen::artworkWorkerLoop()
{
    HttpClient artworkClient;  // persistent connection for series artwork fetches
    artworkClient.setTimeoutSec(8);
    for (;;) {
        ArtworkJob job;
        { std::unique_lock<std::mutex> l(m_artworkMutex); m_artworkCv.wait(l,[&]{return m_artworkStop || !m_artworkJobs.empty();}); if(m_artworkStop)return; job=std::move(m_artworkJobs.front());m_artworkJobs.erase(m_artworkJobs.begin()); }
        auto tag=job.item.imageTags.find("Primary"); if(tag==job.item.imageTags.end()) continue;
        std::vector<unsigned char> data;
        if(ImageCache::isCached(job.item.id,ImageType::Primary,tag->second,job.width,job.height)) data=ImageCache::readCached(job.item.id,ImageType::Primary,tag->second,job.width,job.height);
        if(data.empty() && !m_artworkCancelled.load(std::memory_order_acquire)) { BinaryHttpResponse r;std::string e;TelemetryRequestScope request(RequestKind::Artwork);if(RouteRequest(m_session).run([&](const std::string &base){return artworkClient.getBinary(buildImageUrl(base,job.item.id,ImageType::Primary,tag->second,job.width,job.height),JellyfinApi::buildAuthHeaders(m_session.accessToken,m_session.deviceId),r,e,512*1024,&m_artworkCancelled)&&r.ok();},e)){data=std::move(r.data);ImageCache::writeToCache(job.item.id,ImageType::Primary,tag->second,job.width,job.height,data.data(),data.size());} }
        DecodedImage image; if(!data.empty()&&!m_artworkCancelled.load(std::memory_order_acquire)) image=ImageDecoder::decodeJpeg(data.data(),data.size());
        {std::lock_guard<std::mutex>g(m_artworkMutex);if(!m_artworkStop)m_artworkCompleted[job.key]=std::move(image);}
}
}

// -------------------------------------------------------------------
// Season poster artwork — load at most one per update() cycle
// -------------------------------------------------------------------

void SeriesScreen::tryLoadOneVisibleSeasonArtwork()
{
    if (m_loadState != LoadState::Ready)
        return;

    int totalSeasons = (int)m_seasons.size();

    for (int vis = 0; vis < GRID_VISIBLE; ++vis) {
        int gridRow = vis / GRID_COLS;
        int gridCol = vis % GRID_COLS;
        int itemIdx = (m_gridScroll + gridRow) * GRID_COLS + gridCol;
        if (itemIdx >= totalSeasons) break;

        const MediaItem &season = m_seasons[itemIdx];

        std::string key = seasonArtworkKey(season);
        if (key.empty())
            continue;

        // Skip if already attempted (successful or not)
        if (m_seasonArtwork.count(key) > 0)
            continue;

        // Mark attempted immediately to prevent retry
        m_seasonArtwork[key] = DecodedImage{};  // empty = attempted, not yet decoded

        auto tagIt = season.imageTags.find("Primary");
        const std::string &tag = tagIt->second;

        printf("[SeriesScreen] SeasonArtwork: loading %s tag=%s (%dx%d)\n",
               season.id.c_str(), tag.c_str(), POSTER_W, POSTER_H);

        queueArtwork(key, season, POSTER_W, POSTER_H, false);
        return;
    }
}

} // namespace miyoofin
