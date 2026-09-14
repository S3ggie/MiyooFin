#include "MovieDetailsScreen.hpp"
#include "MovieDetailsScreenInternal.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../net/JellyfinApi.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace miyoofin {

void MovieDetailsScreen::prepareWorker()
{
    // The selected MediaItem already contains the detail metadata. There is
    // deliberately no LibraryCache/OfflineCatalog read or offline projection
    // in this open path; downloaded-only Movies keep the item projected by
    // HomeScreen and local playback is still resolved when Play is pressed.
    std::vector<std::string> overviewLines;
    {
        UiDiagnostics::Scope scope("MovieDetailsScreen::metadata preparation",false);
        overviewLines=wrapText(m_movie.overview.c_str(),META_WRAP);
    }
    {
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        if(!m_prepareCancelled.load(std::memory_order_acquire)){
            m_preparedOverviewLines=std::move(overviewLines);
            m_preparedOverviewReady=true;
        }
    }

    uiDiagnostics().setWorker("artwork","movie download state");
    if(m_downloads&&!m_prepareCancelled.load(std::memory_order_acquire)){
        std::uint64_t planId=0;
        {
            // requestPlan includes DownloadManager mutex acquisition, its
            // snapshot/local-state estimate, and filesystem free-space lookup.
            UiDiagnostics::Scope scope("MovieDetailsScreen::DownloadManager snapshot/free-space preparation",false);
            planId=m_downloads->requestPlan({m_movie});
        }
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        if(!m_prepareCancelled.load(std::memory_order_acquire)){
            m_preparedPlanId=planId;
            m_preparedPlanReady=true;
        }
    }

    DecodedImage artwork;
    if(!m_prepareCancelled.load(std::memory_order_acquire))artwork=loadMovieArtwork();
    {
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        if(!m_prepareCancelled.load(std::memory_order_acquire)){
            m_preparedArtwork=std::move(artwork);
            m_preparedArtworkReady=true;
        }
    }
    uiDiagnostics().setWorker("artwork","idle");
}

// -------------------------------------------------------------------
// loadMovieArtwork — worker-only cache/network/decode path
// -------------------------------------------------------------------
DecodedImage MovieDetailsScreen::loadMovieArtwork()
{
    TelemetryArtworkScope artworkScope(ArtworkContext::MovieDetails);

    auto it = m_movie.imageTags.find("Primary");
    if (it == m_movie.imageTags.end() || it->second.empty()) {
        printf("[MovieDetailsScreen] No Primary artwork tag for %s\n",
               m_movie.title.c_str());
        return {};
    }

    const std::string &tag = it->second;
    printf("[MovieDetailsScreen] Artwork: loading %s tag=%s (%dx%d)\n",
           m_movie.id.c_str(), tag.c_str(), POSTER_W, POSTER_H);

    // 1. Check disk cache
    std::vector<unsigned char> jpegData;
    bool cached=false;
    {
        uiDiagnostics().setWorker("artwork","movie ImageCache lookup");
        UiDiagnostics::Scope scope("MovieDetailsScreen::ImageCache lookup/filesystem stat",false);
        cached=ImageCache::isCached(m_movie.id,ImageType::Primary,tag,POSTER_W,POSTER_H);
    }
    if(cached){
        uiDiagnostics().setWorker("artwork","movie ImageCache read");
        {
            UiDiagnostics::Scope scope("MovieDetailsScreen::ImageCache filesystem read",false);
            jpegData=ImageCache::readCached(m_movie.id,ImageType::Primary,tag,POSTER_W,POSTER_H);
        }
        printf("[MovieDetailsScreen] Artwork: cache hit (%zu bytes)\n",
               jpegData.size());
    }

    // 2. If not cached, synchronous HTTP request
    if (jpegData.empty()) {
        if(m_prepareCancelled.load(std::memory_order_acquire))return {};
        HttpClient client;
        client.setTimeoutSec(8);
        auto headers = JellyfinApi::buildAuthHeaders(
            m_session.accessToken, m_session.deviceId);

        BinaryHttpResponse response;
        std::string error;
        bool fetched=false;
        {
            // This curl transfer was the synchronous operation in enter()
            // responsible for the hardware-proven multi-second push stall.
            uiDiagnostics().setWorker("artwork","movie Jellyfin artwork HTTP");
            UiDiagnostics::Scope scope("MovieDetailsScreen::Jellyfin artwork HTTP/curl",false);
            TelemetryRequestScope request(RequestKind::Artwork);
            fetched=RouteRequest(m_session).run([&](const std::string &base){return client.getBinary(buildImageUrl(base,m_movie.id,ImageType::Primary,tag,POSTER_W,POSTER_H),headers,response,error,512*1024,&m_prepareCancelled)&&response.ok();},error);
        }
        if(!fetched){
            printf("[MovieDetailsScreen] Artwork fetch failed: %s\n",
                   error.c_str());
            return {};
        }
        if (!response.ok()) {
            printf("[MovieDetailsScreen] Artwork fetch failed: HTTP %ld\n",
                   response.status);
            return {};
        }

        jpegData = std::move(response.data);
        printf("[MovieDetailsScreen] Artwork: downloaded %zu bytes\n",
               jpegData.size());

        // Cache to disk (best-effort)
        if(!m_prepareCancelled.load(std::memory_order_acquire)){
            uiDiagnostics().setWorker("artwork","movie ImageCache write");
            UiDiagnostics::Scope scope("MovieDetailsScreen::ImageCache filesystem write",false);
            ImageCache::writeToCache(m_movie.id,ImageType::Primary,tag,
                                     POSTER_W,POSTER_H,jpegData.data(),jpegData.size());
        }
    }

    // 3. Decode JPEG
    if(m_prepareCancelled.load(std::memory_order_acquire))return {};
    uiDiagnostics().setWorker("artwork","movie JPEG decode");
    DecodedImage artwork;
    {
        UiDiagnostics::Scope scope("MovieDetailsScreen::JPEG/image decode",false);
        artwork=ImageDecoder::decodeJpeg(jpegData.data(),jpegData.size());
    }
    if (artwork.empty()) {
        printf("[MovieDetailsScreen] Artwork: decode failed\n");
    } else {
        printf("[MovieDetailsScreen] Artwork: decoded %dx%d\n",
               artwork.width, artwork.height);
    }
    return artwork;
}

// -------------------------------------------------------------------
// render
// -------------------------------------------------------------------
}
