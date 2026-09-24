#include "HomeScreen.hpp"
#include "../ArtworkLayout.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"

namespace miyoofin {

static constexpr int VISIBLE_ROWS = 3;
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;
// Upper bound on cached card surfaces.  Each row artwork entry can produce
// surfaces at several box sizes; this cap prevents unbounded growth when
// distinct keys accumulate faster than row-eviction cleans them up.
static constexpr int MAX_CARD_SURFACES = 128;

void HomeScreen::tryLoadSelectedArtwork()
{
    const MediaItem* item = activeTabNamed("Shows") ? showsSelectedItem() : currentItem();
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

    // A terminal worker tombstone applies to the selected preview as well as
    // its row card; do not resubmit the same identity every UI frame.
    const auto rowState = m_rowArtwork.find(key);
    if (rowState != m_rowArtwork.end() && rowState->second.status == RowArtworkStatus::Failed) {
        m_selectedArtworkAttempted = true;
        m_selectedArtworkId = key;
        return;
    }

    // Shows artwork is always decoded by the background worker.  The selected
    // key is first in its working set, and the preview reads the same RAM
    // image as the grid card once it arrives.
    if (activeTabNamed("Shows")) {
        if (m_selectedArtworkId != key)
            m_selectedArtwork = {};
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
    // it or the controller reaches the bounded failure tombstone.
    m_selectedArtworkAttempted = false;

    // Cache probing, reads and JPEG decode run on the existing bounded decode
    // worker.  A missing poster remains retryable on the existing update
    // cadence until the controller's bounded tombstone is reached.
    submitDecode(*item, true, false);
}

// -------------------------------------------------------------------
// B5d2a: Row card artwork — loading state only (no rendering)
// -------------------------------------------------------------------

std::string HomeScreen::rowArtworkKey(const MediaItem& item)
{
    return homeArtworkKey(item);
}

bool HomeScreen::acceptsShowsArtworkResult(const HomeArtworkController::DecodeResult& result,
                                           std::uint64_t currentGeneration,
                                           const std::set<std::string>& activeKeys,
                                           const std::set<std::string>& protectedKeys)
{
    if (result.context != ArtworkContext::HomeShows)
        return true;
    const std::string key = HomeArtworkController::identityKey(result.identity);
    return result.showsWorkingSetGeneration == currentGeneration &&
           (activeKeys.find(key) != activeKeys.end() ||
            protectedKeys.find(key) != protectedKeys.end());
}

void HomeScreen::evictRowArtworkIfNeeded()
{
    const std::set<std::string> protectedKeys = protectedRowArtworkKeys();
    while ((int)m_rowArtworkOrder.size() > ROW_ARTWORK_RAM_LIMIT) {
        auto victim = std::find_if(
            m_rowArtworkOrder.begin(), m_rowArtworkOrder.end(),
            [&](const std::string& key) { return protectedKeys.find(key) == protectedKeys.end(); });
        // A temporary overflow is preferable to evicting an image being
        // rendered.  This is only possible when every cached key is visible.
        if (victim == m_rowArtworkOrder.end())
            break;
        // Also evict any cached card surfaces for this key
        for (auto it = m_cardSurfaceCache.begin(); it != m_cardSurfaceCache.end();) {
            if (it->first.compare(0, victim->size(), *victim) == 0 &&
                (it->first.size() == victim->size() || it->first[victim->size()] == ':')) {
                if (it->second)
                    SDL_FreeSurface(it->second);
                it = m_cardSurfaceCache.erase(it);
            } else {
                ++it;
            }
        }
        m_rowArtwork.erase(*victim);
        m_rowArtworkOrder.erase(victim);
    }
}

void HomeScreen::touchRowArtwork(const std::string& key)
{
    auto it = std::find(m_rowArtworkOrder.begin(), m_rowArtworkOrder.end(), key);
    if (it != m_rowArtworkOrder.end())
        m_rowArtworkOrder.erase(it);
    m_rowArtworkOrder.push_back(key);
}

void HomeScreen::storeDecodedRowArtwork(const std::string& key, DecodedImage image)
{
    // Evict any card surfaces whose cache key starts with this row artwork
    // key.  Surfaces created by prepareCardSurface now own their pixels, but
    // evicting here ensures stale artwork is never served after an artwork
    // refresh for the same item.
    for (auto it = m_cardSurfaceCache.begin(); it != m_cardSurfaceCache.end();) {
        if (it->first.compare(0, key.size(), key) == 0 &&
            (it->first.size() == key.size() || it->first[key.size()] == ':')) {
            if (it->second)
                SDL_FreeSurface(it->second);
            it = m_cardSurfaceCache.erase(it);
        } else {
            ++it;
        }
    }
    RowArtworkEntry& entry = m_rowArtwork[key];
    entry.status = RowArtworkStatus::Loaded;
    entry.image = std::make_shared<DecodedImage>(std::move(image));
    touchRowArtwork(key);
    evictRowArtworkIfNeeded();
}

void HomeScreen::prepareCardSurface(const std::string& cacheKey, const DecodedImage& img, int boxW,
                                    int boxH)
{
    if (img.empty() || boxW <= 0 || boxH <= 0)
        return;
    // Prevent unbounded card-surface cache growth.  When the cache exceeds
    // the limit, flush it entirely — it will be rebuilt on the next frame
    // for whatever is currently visible.
    if ((int)m_cardSurfaceCache.size() > MAX_CARD_SURFACES)
        freeAllCardSurfaces();
    // Free old cached surface for this key
    auto it = m_cardSurfaceCache.find(cacheKey);
    if (it != m_cardSurfaceCache.end()) {
        if (it->second)
            SDL_FreeSurface(it->second);
        m_cardSurfaceCache.erase(it);
    }
    // Compute aspect-fit destination dimensions
    const float imgAspect = (float)img.width / (float)img.height;
    const float boxAspect = (float)boxW / (float)boxH;
    int dw, dh;
    if (imgAspect > boxAspect) {
        dw = boxW;
        dh = (int)(boxW / imgAspect + 0.5f);
        if (dh > boxH)
            dh = boxH;
    } else {
        dh = boxH;
        dw = (int)(boxH * imgAspect + 0.5f);
        if (dw > boxW)
            dw = boxW;
    }
    // Create a temporary surface wrapping the decoded pixels for blitting.
    SDL_Surface* src =
        SDL_CreateRGBSurfaceFrom((void*)img.pixels.data(), img.width, img.height, 32, img.width * 4,
                                 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (!src)
        return;
    // Always blit into an owned surface.  The source pixels belong to a
    // DecodedImage behind a shared_ptr that storeDecodedRowArtwork may
    // replace at any time; wrapping the source directly (the old 1:1 path)
    // created a use-after-free when the old entry was destroyed, producing
    // TV-static garbage and duplicate artwork on neighboring cards.
    SDL_Surface* owned =
        SDL_CreateRGBSurface(0, dw, dh, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
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
    for (auto& kv : m_cardSurfaceCache) {
        if (kv.second)
            SDL_FreeSurface(kv.second);
    }
    m_cardSurfaceCache.clear();
}

void HomeScreen::submitDecode(const MediaItem& item, bool highPriority, bool shows)
{
    std::string key = rowArtworkKey(item);
    DisplayArtwork a = displayArtworkForItem(item);
    if (key.empty() || !a.valid() || !m_artworkController)
        return;
    const ArtworkContext context =
        shows ? ArtworkContext::HomeShows
              : (highPriority ? ArtworkContext::HomeSelected : ArtworkContext::HomeGrid);
    HomeArtworkController::DecodeRequest request;
    request.identity = {item.id, a.imageType, a.tag, a.width, a.height};
    request.highPriority = highPriority;
    request.context = context;
    request.showsWorkingSetGeneration = shows ? m_showsArtworkGeneration : 0;
    m_artworkController->requestDecode(std::move(request));
}

void HomeScreen::drainDecodedArtwork()
{
    if (!m_artworkController)
        return;
    std::deque<HomeArtworkController::DecodeResult> results;
    m_artworkController->takeDecodeResults(results);
    const std::set<std::string> protectedKeys = protectedRowArtworkKeys();
    for (auto& result : results) {
        const std::string key = HomeArtworkController::identityKey(result.identity);
        // A job already being decoded cannot be cancelled.  Its result is not
        // allowed to displace current Shows artwork after a scroll.  Home owns
        // this freshness decision; the controller only returns value results.
        if (!acceptsShowsArtworkResult(result, m_showsArtworkGeneration, m_activeShowsDecodeKeys,
                                       protectedKeys)) {
            m_artworkController->completeDecodeResult(result, false);
            performanceTelemetry().addWorkerCancelled(WorkerId::HomeDecode);
            continue;
        }
        m_artworkController->completeDecodeResult(result, true);
        if (result.image.empty()) {
            // The controller owns miss/corrupt-cache retry counting and
            // corrupt-cache deletion; Home applies only its terminal UI
            // tombstone.
            if (result.terminalFailure)
                m_rowArtwork[key].status = RowArtworkStatus::Failed;
        } else {
            if (key == m_selectedArtworkId) {
                m_selectedArtwork = result.image;
                m_selectedArtworkAttempted = true;
            }
            storeDecodedRowArtwork(key, std::move(result.image));
        }
    }
}

std::set<std::string> HomeScreen::protectedRowArtworkKeys() const
{
    std::set<std::string> keys;
    auto add = [&](const MediaItem& item) {
        const std::string key = rowArtworkKey(item);
        if (!key.empty())
            keys.insert(key);
    };
    auto addGrid = [&](const std::vector<MediaItem>& items, int scroll, int columns, int rows) {
        const int first = std::max(0, scroll) * columns;
        const int last = std::min((int)items.size(), first + columns * rows);
        for (int i = first; i < last; ++i)
            add(items[i]);
    };

    if (activeTabNamed("Shows")) {
        addGrid(m_filteredShows, m_showScroll, SHOWS_GRID_COLUMNS, SHOWS_GRID_ROWS);
        addGrid(m_filteredAnime, m_animeScroll, SHOWS_GRID_COLUMNS, SHOWS_GRID_ROWS);
        if (const MediaItem* item = showsSelectedItem())
            add(*item);
        return keys;
    }
    if (activeTabNamed("Movies")) {
        const int movies = tabIndex("Movies");
        if (movies >= 0 && !m_tabs[movies].rows.empty()) {
            const auto& items = m_tabs[movies].rows[0].items;
            addGrid(items, m_rowScroll, MOVIE_GRID_COLUMNS, MOVIE_GRID_ROWS);
            if (const MediaItem* item = currentItem())
                add(*item);
        }
        return keys;
    }

    // Home's viewport is horizontal and each row has variable card widths.
    const auto& rows = currentTab().rows;
    for (int ri = 0; ri < VISIBLE_ROWS; ++ri) {
        const int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size())
            break;
        int cardX = HOME_RAIL_MARGIN;
        for (const auto& item : rows[rowIdx].items) {
            const ArtworkBox box = homeRailCardSize(item);
            const int screenX = cardX - rowCardScrollOffset(rowIdx, m_activeRow, m_cardScroll);
            if (screenX + box.w >= HOME_RAIL_MARGIN && screenX <= 640 - HOME_RAIL_MARGIN)
                add(item);
            if (screenX > 640 - HOME_RAIL_MARGIN)
                break;
            cardX += box.w + HOME_RAIL_GAP;
        }
    }
    if (const MediaItem* item = currentItem())
        add(*item);
    return keys;
}

void HomeScreen::updateShowsDecodeWorkingSet()
{
    std::vector<const MediaItem*> desired;
    std::set<std::string> keys;
    if (!activeTabNamed("Shows")) {
        if (!m_activeShowsDecodeKeys.empty())
            ++m_showsArtworkGeneration;
        m_activeShowsDecodeKeys.clear();
        if (m_artworkController)
            m_artworkController->setShowsWorkingSet(m_showsArtworkGeneration, {});
        return;
    }
    auto add = [&](const MediaItem& item) {
        const std::string key = rowArtworkKey(item);
        if (!key.empty() && keys.insert(key).second)
            desired.push_back(&item);
    };
    if (const MediaItem* selected = showsSelectedItem())
        add(*selected);
    auto addGrid = [&](const std::vector<MediaItem>& items, int scroll) {
        const int first = std::max(0, scroll) * SHOWS_GRID_COLUMNS;
        const int last = std::min((int)items.size(), first + SHOWS_GRID_COLUMNS * SHOWS_GRID_ROWS);
        for (int i = first; i < last; ++i)
            add(items[i]);
    };
    if (m_showsFocus == ShowsFocus::AnimeGrid) {
        addGrid(m_filteredAnime, m_animeScroll);
        addGrid(m_filteredShows, m_showScroll);
    } else {
        addGrid(m_filteredShows, m_showScroll);
        addGrid(m_filteredAnime, m_animeScroll);
    }
    if (keys != m_activeShowsDecodeKeys)
        ++m_showsArtworkGeneration;
    m_activeShowsDecodeKeys.swap(keys);
    if (m_artworkController)
        m_artworkController->setShowsWorkingSet(m_showsArtworkGeneration, m_activeShowsDecodeKeys);
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
    const auto& rows = currentTab().rows;
    if (rows.empty())
        return;

    // Movies is a flattened 9x4 grid: m_rowScroll is a grid row, not a
    // TabData row.  Decode only its actual visible cached posters.
    if (activeTabNamed("Movies")) {
        const MediaRow& row = rows[0];
        int first = m_rowScroll * MOVIE_GRID_COLUMNS;
        int last = std::min((int)row.items.size(), first + MOVIE_GRID_COLUMNS * MOVIE_GRID_ROWS);
        for (int i = first; i < last; ++i) {
            const MediaItem& item = row.items[i];
            std::string key = rowArtworkKey(item);
            if (key.empty() || m_rowArtwork.find(key) != m_rowArtwork.end())
                continue;
            submitDecode(item, i == first, false);
        }
        return;
    }

    // Submit every horizontally visible card that is not already in the RAM
    // cache. submitDecode preserves outstanding-job de-duplication and the
    // bounded queue, while selected artwork was already queued at priority.
    for (int ri = 0; ri < VISIBLE_ROWS; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size())
            break;
        const MediaRow& row = rows[rowIdx];
        int cardAccumX = HOME_RAIL_MARGIN;
        for (int ci = 0; ci < (int)row.items.size(); ++ci) {
            ArtworkBox sz = homeRailCardSize(row.items[ci]);
            int screenX = cardAccumX - rowCardScrollOffset(rowIdx, m_activeRow, m_cardScroll);
            if (screenX + sz.w < HOME_RAIL_MARGIN) {
                cardAccumX += sz.w + HOME_RAIL_GAP;
                continue;
            }
            if (screenX > 640 - HOME_RAIL_MARGIN)
                break;
            std::string key = rowArtworkKey(row.items[ci]);
            if (!key.empty() && m_rowArtwork.find(key) == m_rowArtwork.end())
                submitDecode(row.items[ci], false, false);
            cardAccumX += sz.w + HOME_RAIL_GAP;
        }
    }
}

} // namespace miyoofin
