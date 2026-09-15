#include "HomeScreen.hpp"
#include "../ArtworkLayout.hpp"
#include "../ShowsBrowser.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
#include "../../cache/ImageCache.hpp"

namespace miyoofin {

static constexpr int CARD_GAP = 6;
static constexpr int VISIBLE_ROWS = 3;
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;
// Upper bound on cached card surfaces.  Each row artwork entry can produce
// surfaces at several box sizes; this cap prevents unbounded growth when
// distinct keys accumulate faster than row-eviction cleans them up.
static constexpr int MAX_CARD_SURFACES = 128;

/// Parse a row-artwork key ("itemId:imageType:imageTag:WxH") back into a
/// PosterJob suitable for ImageCache::removeCached().  Returns a zeroed
/// job when the key format is unrecognised (width == 0).
static HomePosterJob buildPosterJobFromKey(const std::string &key)
{
    // Format: "itemId:Primary:imageTag:WxH"
    auto p1 = key.find(':');
    if (p1 == std::string::npos) return {};
    auto p2 = key.find(':', p1 + 1);
    if (p2 == std::string::npos) return {};
    auto p3 = key.find(':', p2 + 1);
    if (p3 == std::string::npos) return {};
    auto px = key.find('x', p3 + 1);
    if (px == std::string::npos) return {};
    HomePosterJob job;
    job.itemId   = key.substr(0, p1);
    const std::string typeName = key.substr(p1 + 1, p2 - p1 - 1);
    job.imageType = (typeName == "Thumb") ? ImageType::Thumb : ImageType::Primary;
    job.imageTag  = key.substr(p2 + 1, p3 - p2 - 1);
    job.width     = std::atoi(key.substr(p3 + 1, px - p3 - 1).c_str());
    job.height    = std::atoi(key.substr(px + 1).c_str());
    return job;
}

void HomeScreen::tryLoadSelectedArtwork()
{
    const MediaItem *item = activeTabNamed("Shows") ? showsSelectedItem() : currentItem();
    if (!item) {
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    DisplayArtwork artwork = displayArtworkForItem(*item);
    if (!artwork.valid()) {
        // No Primary tag — clear artwork, keep placeholder
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    std::string key = rowArtworkKey(*item);

    // Shows artwork is always decoded by the background worker.  The selected
    // key is first in its working set, and the preview reads the same RAM
    // image as the grid card once it arrives.
    if (activeTabNamed("Shows")) {
        if (m_selectedArtworkId != key) m_selectedArtwork = {};
        m_selectedArtworkId = key;
        m_selectedArtworkAttempted = true;
        return;
    }

    // Already attempted this exact selection?  Do not retry.
    if (m_selectedArtworkAttempted && m_selectedArtworkId == key)
        return;

    // New selection — reset and attempt once
    m_selectedArtwork = {};
    m_selectedArtworkId = key;
    // Local library artwork remains retryable until the poster worker writes
    // it; Home retains its historical one-shot network behavior below.
    m_selectedArtworkAttempted = false;

    // Cache probing, reads and JPEG decode run on the existing bounded decode
    // worker.  A missing poster remains retryable after poster sync completes.
    submitDecode(*item,true,false);
}

// -------------------------------------------------------------------
// B5d2a: Row card artwork — loading state only (no rendering)
// -------------------------------------------------------------------

std::string HomeScreen::rowArtworkKey(const MediaItem &item)
{
    return homeArtworkKey(item);
}

void HomeScreen::evictRowArtworkIfNeeded()
{
    const std::set<std::string> protectedKeys = protectedRowArtworkKeys();
    while ((int)m_rowArtworkOrder.size() > ROW_ARTWORK_RAM_LIMIT) {
        auto victim = std::find_if(m_rowArtworkOrder.begin(),
                                   m_rowArtworkOrder.end(),
            [&](const std::string &key) {
                return protectedKeys.find(key) == protectedKeys.end();
            });
        // A temporary overflow is preferable to evicting an image being
        // rendered.  This is only possible when every cached key is visible.
        if (victim == m_rowArtworkOrder.end()) break;
        // Also evict any cached card surfaces for this key
        for (auto it = m_cardSurfaceCache.begin(); it != m_cardSurfaceCache.end(); ) {
            if (it->first.compare(0, victim->size(), *victim) == 0
                && (it->first.size() == victim->size()
                    || it->first[victim->size()] == ':')) {
                if (it->second) SDL_FreeSurface(it->second);
                it = m_cardSurfaceCache.erase(it);
            } else ++it;
        }
        m_rowArtwork.erase(*victim);
        m_rowArtworkOrder.erase(victim);
    }
}

void HomeScreen::touchRowArtwork(const std::string &key)
{
    auto it = std::find(m_rowArtworkOrder.begin(), m_rowArtworkOrder.end(), key);
    if (it != m_rowArtworkOrder.end()) m_rowArtworkOrder.erase(it);
    m_rowArtworkOrder.push_back(key);
}

void HomeScreen::storeDecodedRowArtwork(const std::string &key, DecodedImage image)
{
    // Evict any card surfaces whose cache key starts with this row artwork
    // key.  Surfaces created by prepareCardSurface now own their pixels, but
    // evicting here ensures stale artwork is never served after an artwork
    // refresh for the same item.
    for (auto it = m_cardSurfaceCache.begin(); it != m_cardSurfaceCache.end(); ) {
        if (it->first.compare(0, key.size(), key) == 0
            && (it->first.size() == key.size()
                || it->first[key.size()] == ':')) {
            if (it->second) SDL_FreeSurface(it->second);
            it = m_cardSurfaceCache.erase(it);
        } else ++it;
    }
    RowArtworkEntry &entry = m_rowArtwork[key];
    entry.status = RowArtworkStatus::Loaded;
    entry.image = std::make_shared<DecodedImage>(std::move(image));
    touchRowArtwork(key);
    evictRowArtworkIfNeeded();
}

void HomeScreen::prepareCardSurface(const std::string &cacheKey,
                                     const DecodedImage &img, int boxW, int boxH)
{
    if (img.empty() || boxW <= 0 || boxH <= 0) return;
    // Prevent unbounded card-surface cache growth.  When the cache exceeds
    // the limit, flush it entirely — it will be rebuilt on the next frame
    // for whatever is currently visible.
    if ((int)m_cardSurfaceCache.size() > MAX_CARD_SURFACES)
        freeAllCardSurfaces();
    // Free old cached surface for this key
    auto it = m_cardSurfaceCache.find(cacheKey);
    if (it != m_cardSurfaceCache.end()) {
        if (it->second) SDL_FreeSurface(it->second);
        m_cardSurfaceCache.erase(it);
    }
    // Compute aspect-fit destination dimensions
    const float imgAspect = (float)img.width / (float)img.height;
    const float boxAspect = (float)boxW / (float)boxH;
    int dw, dh;
    if (imgAspect > boxAspect) {
        dw = boxW;
        dh = (int)(boxW / imgAspect + 0.5f);
        if (dh > boxH) dh = boxH;
    } else {
        dh = boxH;
        dw = (int)(boxH * imgAspect + 0.5f);
        if (dw > boxW) dw = boxW;
    }
    // Create a temporary surface wrapping the decoded pixels for blitting.
    SDL_Surface *src = SDL_CreateRGBSurfaceFrom(
        (void *)img.pixels.data(), img.width, img.height, 32,
        img.width * 4, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!src) return;
    // Always blit into an owned surface.  The source pixels belong to a
    // DecodedImage behind a shared_ptr that storeDecodedRowArtwork may
    // replace at any time; wrapping the source directly (the old 1:1 path)
    // created a use-after-free when the old entry was destroyed, producing
    // TV-static garbage and duplicate artwork on neighboring cards.
    SDL_Surface *owned = SDL_CreateRGBSurface(0, dw, dh, 32,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (owned) {
        SDL_Rect srcR = {0, 0, img.width, img.height};
        SDL_Rect dstR = {0, 0, dw, dh};
        SDL_BlitScaled(src, &srcR, owned, &dstR);
        m_cardSurfaceCache[cacheKey] = owned;
    }
    SDL_FreeSurface(src);
}

void HomeScreen::freeAllCardSurfaces()
{
    for (auto &kv : m_cardSurfaceCache)
        if (kv.second) SDL_FreeSurface(kv.second);
    m_cardSurfaceCache.clear();
}

void HomeScreen::submitDecode(const MediaItem &item, bool highPriority, bool shows)
{
    std::string key=rowArtworkKey(item); DisplayArtwork a=displayArtworkForItem(item);
    if(key.empty() || !a.valid()) return;
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    if(!m_decodeOutstanding.insert(key).second) return;
    if(m_decodeJobs.size() >= 32) { m_decodeOutstanding.erase(key); performanceTelemetry().addWorkerFailed(WorkerId::HomeDecode); return; }
    const ArtworkContext context=shows ? ArtworkContext::HomeShows : (highPriority ? ArtworkContext::HomeSelected : ArtworkContext::HomeGrid);
    DecodeJob job{key,{item.id,a.imageType,a.tag,a.width,a.height},shows,context};
    if(highPriority) m_decodeJobs.push_front(std::move(job)); else m_decodeJobs.push_back(std::move(job));
    performanceTelemetry().setWorkerQueueDepth(
        WorkerId::HomeDecode, static_cast<uint32_t>(m_decodeJobs.size()));
    m_decodeWake.notify_one();
}

void HomeScreen::decodeWorker()
{
    for (;;) { DecodeJob job; { std::unique_lock<std::mutex> lock(m_decodeMutex); m_decodeWake.wait(lock,[&]{return m_stopDecodeWorker||!m_decodeJobs.empty();}); if(m_stopDecodeWorker){ performanceTelemetry().setWorkerActive(WorkerId::HomeDecode, false); performanceTelemetry().setWorkerQueueDepth(WorkerId::HomeDecode, static_cast<uint32_t>(m_decodeJobs.size())); return; } job=std::move(m_decodeJobs.front());m_decodeJobs.pop_front(); performanceTelemetry().setWorkerQueueDepth(WorkerId::HomeDecode, static_cast<uint32_t>(m_decodeJobs.size())); performanceTelemetry().setWorkerActive(WorkerId::HomeDecode, true); }
        TelemetryArtworkScope artwork(job.context);
        auto bytes=ImageCache::readCached(job.artwork.itemId,job.artwork.imageType,job.artwork.imageTag,job.artwork.width,job.artwork.height);
        DecodedImage image=bytes.empty()?DecodedImage{}:ImageDecoder::decodeJpeg(bytes.data(),bytes.size());
        std::lock_guard<std::mutex> lock(m_decodeMutex); m_decodeResults.push_back({std::move(job.key),std::move(image),job.shows,!bytes.empty()}); performanceTelemetry().setWorkerActive(WorkerId::HomeDecode, false);
    }
}

void HomeScreen::drainDecodedArtwork()
{
    std::deque<DecodeResult> results;
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        results.swap(m_decodeResults);
        for (const auto &result : results) m_decodeOutstanding.erase(result.key);
    }
    const std::set<std::string> protectedKeys = protectedRowArtworkKeys();
    for (auto &result : results) {
        // A job already being decoded cannot be cancelled.  Its result is not
        // allowed to displace current Shows artwork after a scroll, though.
        if (result.shows
            && m_activeShowsDecodeKeys.find(result.key) == m_activeShowsDecodeKeys.end()
            && protectedKeys.find(result.key) == protectedKeys.end()) { performanceTelemetry().addWorkerCancelled(WorkerId::HomeDecode); continue; }
        if (!result.cachePresent) {
            // Poster sync may populate this key later; do not make a cache miss
            // a permanent failure.
            continue;
        }
        if (result.image.empty()) {
            // Corrupt/undecodable cached file.  Delete it so poster sync can
            // re-download, and only set a permanent Failed tombstone after
            // kMaxDecodeAttempts retries to bound infinite retry loops.
            // Parse the key to extract parameters for removeCached.
            // Key format: "itemId:imageType:imageTag:WxH"
            const auto artwork = buildPosterJobFromKey(result.key);
            if (artwork.width > 0)
                ImageCache::removeCached(artwork.itemId, artwork.imageType,
                                         artwork.imageTag, artwork.width,
                                         artwork.height);
            int &attempts = m_rowArtworkAttempts[result.key];
            ++attempts;
            if (attempts >= kMaxDecodeAttempts)
                m_rowArtwork[result.key].status = RowArtworkStatus::Failed;
            // else: entry is NOT added/kept in m_rowArtwork, so
            // tryLoadOneRowArtwork will resubmit the decode on the next cycle.
        } else {
            if(result.key==m_selectedArtworkId) {
                m_selectedArtwork=result.image;
                m_selectedArtworkAttempted=true;
            }
            storeDecodedRowArtwork(result.key, std::move(result.image));
            m_rowArtworkAttempts.erase(result.key);
        }
    }
}

std::set<std::string> HomeScreen::protectedRowArtworkKeys() const
{
    std::set<std::string> keys;
    auto add = [&](const MediaItem &item) {
        const std::string key = rowArtworkKey(item);
        if (!key.empty()) keys.insert(key);
    };
    auto addGrid = [&](const std::vector<MediaItem> &items, int scroll,
                       int columns, int rows) {
        const int first = std::max(0, scroll) * columns;
        const int last = std::min((int)items.size(), first + columns * rows);
        for (int i = first; i < last; ++i) add(items[i]);
    };

    if (activeTabNamed("Shows")) {
        addGrid(m_filteredShows, m_showScroll, SHOWS_GRID_COLUMNS, SHOWS_GRID_ROWS);
        addGrid(m_filteredAnime, m_animeScroll, SHOWS_GRID_COLUMNS, SHOWS_GRID_ROWS);
        if (const MediaItem *item = showsSelectedItem()) add(*item);
        return keys;
    }
    if (activeTabNamed("Movies")) {
        const int movies=tabIndex("Movies");
        if (movies >= 0 && !m_tabs[movies].rows.empty()) {
            const auto &items = m_tabs[movies].rows[0].items;
            addGrid(items, m_rowScroll, MOVIE_GRID_COLUMNS, MOVIE_GRID_ROWS);
            if (const MediaItem *item = currentItem()) add(*item);
        }
        return keys;
    }

    // Home's viewport is horizontal and each row has variable card widths.
    const auto &rows = currentTab().rows;
    for (int ri = 0; ri < VISIBLE_ROWS; ++ri) {
        const int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        int cardX = 4;
        for (const auto &item : rows[rowIdx].items) {
            const ArtworkBox box = artworkBoxSize(item);
            const int screenX = cardX - rowCardScrollOffset(rowIdx, m_activeRow, m_cardScroll);
            if (screenX + box.w >= 4 && screenX <= 636) add(item);
            if (screenX > 636) break;
            cardX += box.w + CARD_GAP;
        }
    }
    if (const MediaItem *item = currentItem()) add(*item);
    return keys;
}

void HomeScreen::updateShowsDecodeWorkingSet()
{
    std::vector<const MediaItem *> desired;
    std::set<std::string> keys;
    if (!activeTabNamed("Shows")) {
        m_activeShowsDecodeKeys.clear();
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        for (auto it = m_decodeJobs.begin(); it != m_decodeJobs.end();) {
            if (it->shows) {
                m_decodeOutstanding.erase(it->key);
                it = m_decodeJobs.erase(it);
            } else ++it;
        }
        performanceTelemetry().setWorkerQueueDepth(
            WorkerId::HomeDecode, static_cast<uint32_t>(m_decodeJobs.size()));
        return;
    }
    auto add = [&](const MediaItem &item) {
        const std::string key = rowArtworkKey(item);
        if (!key.empty() && keys.insert(key).second) desired.push_back(&item);
    };
    if (const MediaItem *selected = showsSelectedItem()) add(*selected);
    auto addGrid = [&](const std::vector<MediaItem> &items, int scroll) {
        const int first = std::max(0, scroll) * SHOWS_GRID_COLUMNS;
        const int last = std::min((int)items.size(), first + SHOWS_GRID_COLUMNS * SHOWS_GRID_ROWS);
        for (int i = first; i < last; ++i) add(items[i]);
    };
    if (m_showsFocus == ShowsFocus::AnimeGrid) {
        addGrid(m_filteredAnime, m_animeScroll); addGrid(m_filteredShows, m_showScroll);
    } else {
        addGrid(m_filteredShows, m_showScroll); addGrid(m_filteredAnime, m_animeScroll);
    }
    m_activeShowsDecodeKeys.swap(keys);
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        for (auto it = m_decodeJobs.begin(); it != m_decodeJobs.end();) {
            if (it->shows && m_activeShowsDecodeKeys.find(it->key) == m_activeShowsDecodeKeys.end()) {
                m_decodeOutstanding.erase(it->key);
                it = m_decodeJobs.erase(it);
            } else ++it;
        }
        performanceTelemetry().setWorkerQueueDepth(
            WorkerId::HomeDecode, static_cast<uint32_t>(m_decodeJobs.size()));
    }
    for (size_t i = 0; i < desired.size(); ++i) {
        const std::string key = rowArtworkKey(*desired[i]);
        if (m_rowArtwork.find(key) == m_rowArtwork.end())
            submitDecode(*desired[i], i == 0, true);
    }
}

void HomeScreen::tryLoadOneRowArtwork()
{
    if (activeTabNamed("Shows")) {
        updateShowsDecodeWorkingSet();
        return;
    }
    const auto &rows = currentTab().rows;
    if (rows.empty()) return;

    // Movies is a flattened 9x4 grid: m_rowScroll is a grid row, not a
    // TabData row.  Decode only its actual visible cached posters.
    if (activeTabNamed("Movies")) {
        const MediaRow &row = rows[0];
        int first = m_rowScroll * MOVIE_GRID_COLUMNS;
        int last = std::min((int)row.items.size(), first + MOVIE_GRID_COLUMNS * MOVIE_GRID_ROWS);
        for (int i=first; i<last; ++i) {
            const MediaItem &item=row.items[i]; std::string key=rowArtworkKey(item);
            if (key.empty() || m_rowArtwork.find(key)!=m_rowArtwork.end()) continue;
            submitDecode(item,i==first,false);
        }
        return;
    }

    // Submit every horizontally visible card that is not already in the RAM
    // cache. submitDecode preserves outstanding-job de-duplication and the
    // bounded queue, while selected artwork was already queued at priority.
    static constexpr int HMARGIN = 4;
    for (int ri = 0; ri < VISIBLE_ROWS; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        const MediaRow &row = rows[rowIdx];
        int cardAccumX = HMARGIN;
        for (int ci = 0; ci < (int)row.items.size(); ++ci) {
            ArtworkBox sz = artworkBoxSize(row.items[ci]);
            int screenX = cardAccumX - rowCardScrollOffset(rowIdx, m_activeRow, m_cardScroll);
            if (screenX + sz.w < HMARGIN) {
                cardAccumX += sz.w + CARD_GAP;
                continue;
            }
            if (screenX > 640 - HMARGIN) break;
            std::string key = rowArtworkKey(row.items[ci]);
            if (!key.empty() && m_rowArtwork.find(key) == m_rowArtwork.end())
                submitDecode(row.items[ci], false, false);
            cardAccumX += sz.w + CARD_GAP;
        }
    }
}

} // namespace miyoofin
