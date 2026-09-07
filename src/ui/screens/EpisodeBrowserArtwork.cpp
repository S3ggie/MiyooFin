#include "EpisodeBrowserScreen.hpp"
#include "../ArtworkLayout.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../cache/OfflineCatalog.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../app/UiDiagnostics.hpp"
#include "../Theme.hpp"
#include <cstdio>

namespace miyoofin {

static constexpr int THUMB_X=326, THUMB_Y=38, THUMB_W=288, THUMB_H=162;

void EpisodeBrowserScreen::clearSelectedEpisodeArtwork()
{
    m_episodeArtwork = {};
    m_episodeArtworkKey.clear();
    freePreparedArtwork(m_episodeArtworkSurface);
}

void EpisodeBrowserScreen::tryLoadSelectedEpisodeArtwork()
{
    UiDiagnostics::Scope scope("EpisodeBrowserScreen::publishArtworkResult");
    int total = (int)m_episodes.size();
    if (total <= 0 || m_selectedEpisode < 0 || m_selectedEpisode >= total) {
        m_episodeArtwork = {};
        m_episodeArtworkKey.clear();
        freePreparedArtwork(m_episodeArtworkSurface);
        return;
    }

    const MediaItem &ep = m_episodes[m_selectedEpisode];

    // Look for Primary image tag (NOT Thumb)
    auto it = ep.imageTags.find("Primary");
    if (it == ep.imageTags.end() || it->second.empty()) {
        m_episodeArtwork = {};
        m_episodeArtworkKey = ep.id + ":none:288x162";
        freePreparedArtwork(m_episodeArtworkSurface);
        return;
    }

    const std::string &tag = it->second;

    // Build stable identity key: episodeId:Primary:imageTag:288x162
    std::string key = ep.id + ":Primary:" + tag + ":288x162";

    // 1. Already decoded artwork for this exact key?  Done.
    if (m_episodeArtworkKey == key && !m_episodeArtwork.empty())
        return;

    // 2. Publish the worker-decoded image (consumed once per frame max).
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (m_workerHasCompletion) {
            ArtworkCompletion comp = std::move(m_workerCompletion);
            m_workerHasCompletion = false;
            if (comp.artworkKey == key) {
                if (comp.success) {
                    m_episodeArtwork=std::move(comp.image);
                    freePreparedArtwork(m_episodeArtworkSurface);
                    m_episodeArtworkSurface=prepareArtworkSurface(m_episodeArtwork);
                    m_episodeArtworkKey = key;
                }
                return;
            }
            // Stale completion (old selection) — fall through
        }
    }

    // 3. Selection changed — clear previous decoded artwork
    if (m_episodeArtworkKey != key) {
        m_episodeArtwork = {};
        m_episodeArtworkKey = key;
        freePreparedArtwork(m_episodeArtworkSurface);
    }

    // 4. Failed or already in progress?  Wait.
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (m_failedKeys.count(key) || m_workerInProgressKey == key)
            return;
    }

    // Disk cache reads and JPEG decode also belong to the worker.
    wakeArtworkWorker();
}

void EpisodeBrowserScreen::wakeArtworkWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerSelected = m_selectedEpisode;
        m_workerListScroll = m_listScroll;
        const std::string selectedKey=(m_workerSelected>=0&&m_workerSelected<(int)m_artworkJobs.size())?m_artworkJobs[m_workerSelected].artworkKey:"";
        if(selectedKey!=m_workerDecodedKey)m_workerDecodedKey.clear();
        ++m_workerGeneration;
        // The one worker can be inside a prefetch transfer.  Make libcurl's
        // progress callback abort it so the latest selected cache hit is not
        // held behind that stale network timeout.
        if (!m_workerInProgressKey.empty())
            m_workerCancelled.store(true, std::memory_order_release);
        if (!m_workerThread.joinable())
            m_workerThread = std::thread(
                &EpisodeBrowserScreen::artworkWorkerLoop, this);
    }
    m_workerCv.notify_one();
}

EpisodeBrowserScreen::PreparedArtwork
EpisodeBrowserScreen::prepareArtworkSurface(const DecodedImage &image)
{
    PreparedArtwork prepared;
    if (image.empty()) return prepared;

    const float imgAspect = (float)image.width / (float)image.height;
    const float boxAspect = (float)THUMB_W / (float)THUMB_H;
    int drawW, drawH;
    if (imgAspect > boxAspect) {
        drawW = THUMB_W;
        drawH = (int)(THUMB_W / imgAspect + 0.5f);
        if (drawH > THUMB_H) drawH = THUMB_H;
    } else {
        drawH = THUMB_H;
        drawW = (int)(THUMB_H * imgAspect + 0.5f);
        if (drawW > THUMB_W) drawW = THUMB_W;
    }

    SDL_Surface *source = SDL_CreateRGBSurfaceFrom(
        (void *)image.pixels.data(), image.width, image.height, 32,
        image.width * 4, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    SDL_Surface *destination = SDL_CreateRGBSurface(
        0, drawW, drawH, 32,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (source && destination) {
        SDL_Rect sourceRect = {0, 0, image.width, image.height};
        SDL_Rect destinationRect = {0, 0, drawW, drawH};
        SDL_BlitScaled(source, &sourceRect, destination, &destinationRect);
        prepared.surface = destination;
        prepared.x = THUMB_X + (THUMB_W - drawW) / 2;
        prepared.y = THUMB_Y + (THUMB_H - drawH) / 2;
    } else if (destination) {
        SDL_FreeSurface(destination);
    }
    if (source) SDL_FreeSurface(source);
    return prepared;
}

void EpisodeBrowserScreen::freePreparedArtwork(PreparedArtwork &artwork)
{
    if (artwork.surface) SDL_FreeSurface(artwork.surface);
    artwork = {};
}

// -------------------------------------------------------------------
// artworkWorkerLoop — background thread (B5g1a)
//
// Chooses one artwork job at a time from the freshly computed visible window.
// Never accesses SDL, rendering, or m_episodeArtwork.
// -------------------------------------------------------------------
void EpisodeBrowserScreen::artworkWorkerLoop()
{
    std::uint64_t observedGeneration = 0;
    while (true) {
        ArtworkJob job;
        int candidate = -1;
        std::uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            m_workerCv.wait(lock, [&] {
                return m_workerStop || (!m_workerPaused &&
                    observedGeneration != m_workerGeneration);
            });
            if (m_workerStop) break;
            generation = m_workerGeneration;
            observedGeneration = generation;
            // wakeArtworkWorker() and this reset share m_workerMutex.  A
            // cancellation for a later generation therefore cannot be lost.
            m_workerCancelled.store(false, std::memory_order_release);
        }

        std::set<int> unavailable;
        while (true) {
            int selected;
            {
                std::lock_guard<std::mutex> lock(m_workerMutex);
                if (m_workerStop) return;
                if (m_workerPaused || generation != m_workerGeneration) break;
                selected = m_workerSelected;
                candidate = nextPrefetchIndex(selected, m_workerListScroll,
                    (int)m_artworkJobs.size(), unavailable);
                if (candidate < 0) break;
                job = m_artworkJobs[candidate];
                if (job.artworkKey.empty() ||
                    m_failedKeys.count(job.artworkKey) ||
                    m_workerInProgressKey == job.artworkKey) {
                    unavailable.insert(candidate);
                    continue;
                }
            }
            const bool cached=ImageCache::isCached(job.itemId, ImageType::Primary,
                                                   job.imageTag, job.width, job.height);
            if (cached && (candidate != selected || m_workerDecodedKey == job.artworkKey)) {
                unavailable.insert(candidate);
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(m_workerMutex);
                if (m_workerPaused || generation != m_workerGeneration) break;
                m_workerInProgressKey = job.artworkKey;
            }
            printf("[EpisodeBrowserScreen] Prefetch: selected=%d candidate=%d\n",
                   selected, candidate);
            break;
        }
        if (candidate < 0) {
            printf("[EpisodeBrowserScreen] Prefetch: window warm\n");
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            if (m_workerInProgressKey != job.artworkKey) continue;
        }

        // --- Execute job (no lock held) ---
        bool success = false;
        DecodedImage decoded;
        const bool cacheHit = ImageCache::isCached(job.itemId, ImageType::Primary,
                                                   job.imageTag, job.width, job.height);
        if (cacheHit)
        {
            success = true;
        } else {
            HttpClient client;
            client.setTimeoutSec(3);
            auto headers = JellyfinApi::buildAuthHeaders(
                m_session.accessToken, m_session.deviceId);

            BinaryHttpResponse response;
            std::string error;
            const bool fetched = RouteRequest(m_session).run([&](const std::string &base){return client.getBinary(buildImageUrl(base,job.itemId,ImageType::Primary,job.imageTag,job.width,job.height),headers,response,error,512*1024,&m_workerCancelled)&&response.ok();},error);
            if (fetched)
            {
                if (response.ok()) {
                    const bool cacheWriteSucceeded = ImageCache::writeToCache(
                        job.itemId, ImageType::Primary, job.imageTag,
                        job.width, job.height,
                        response.data.data(), response.data.size());
                    if (cacheWriteSucceeded) {
                        printf("[EpisodeBrowserScreen] ArtworkWorker:"
                               " downloaded episode=%s\n", job.itemId.c_str());
                        success = true;
                    } else {
                        printf("[EpisodeBrowserScreen] ArtworkWorker:"
                               " cache write failed\n");
                    }
                } else {
                    printf("[EpisodeBrowserScreen] ArtworkWorker:"
                           " HTTP %ld\n", response.status);
                }
            } else {
                printf("[EpisodeBrowserScreen] ArtworkWorker:"
                       " network error: %s\n", error.c_str());
            }
        }

        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            cancelled = artworkRequestCancelled(
                m_workerCancelled.load(std::memory_order_acquire), generation,
                m_workerGeneration);
        }

        // A newer selection won while this job was running.  Do not spend
        // additional time decoding stale bytes; immediately schedule from
        // that generation instead.
        if (cancelled)
            success = false;

        // Only the selected job needs a RAM image; prefetch candidates merely
        // warm the disk cache.  Both read/decode operations stay off SDL.
        int currentSelected=-1;
        {std::lock_guard<std::mutex> lock(m_workerMutex);currentSelected=m_workerSelected;}
        if(success && !cancelled && candidate==currentSelected) {
            auto bytes=ImageCache::readCached(job.itemId,ImageType::Primary,
                                              job.imageTag,job.width,job.height);
            if(!bytes.empty()) {
                decoded=ImageDecoder::decodeJpeg(bytes.data(),bytes.size());
            }
            success=!decoded.empty();
        }

        // --- Publish completion signal (under lock) ---
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            m_workerInProgressKey.clear();
            if (!m_workerStop) {
                const bool stale = artworkRequestCancelled(
                    m_workerCancelled.load(std::memory_order_acquire),
                    generation, m_workerGeneration);
                if (shouldMarkArtworkFailed(success, stale))
                    m_failedKeys.insert(job.artworkKey);
                if(!stale && candidate==m_workerSelected) {
                    if(success)m_workerDecodedKey=job.artworkKey;
                    m_workerCompletion.artworkKey=job.artworkKey;
                    m_workerCompletion.image=std::move(decoded);
                    m_workerCompletion.success=success;
                    m_workerHasCompletion=true;
                }
            }
        }
        // Recompute from the current selection after every request.
        {
            std::lock_guard<std::mutex> lock(m_workerMutex);
            ++m_workerGeneration;
            observedGeneration = m_workerGeneration - 1;
        }
    }
}


} // namespace miyoofin
