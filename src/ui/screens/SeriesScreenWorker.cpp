#include "SeriesScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/ArtworkUrl.hpp"
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

}
