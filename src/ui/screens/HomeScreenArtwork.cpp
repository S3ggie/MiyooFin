#include "HomeScreen.hpp"
#include "../ArtworkLayout.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"

namespace miyoofin {

static constexpr int VISIBLE_ROWS = 3;
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;

void HomeScreen::tryLoadSelectedArtwork()
{
    const MediaItem* item = activeTabNamed("Shows") ? showsSelectedItem() : currentItem();
    if (!item) {
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    const MediaRow* selectedRow = activeTabNamed("Home") ? currentRow() : nullptr;
    const bool landscape = selectedRow && homeRailIsLandscape(selectedRow->label);
    DisplayArtwork artwork = displayArtworkForItem(*item, landscape);
    if (!artwork.valid()) {
        // No Primary tag — clear artwork, keep placeholder
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    std::string key = rowArtworkKey(*item, landscape);

    // A terminal worker tombstone applies to the selected preview as well as
    // its row card; do not resubmit the same identity every UI frame.
    const auto rowState = m_rowArtworkCache.entries.find(key);
    if (rowState != m_rowArtworkCache.entries.end() &&
        rowState->second.status == RowArtworkStatus::Failed) {
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
    submitDecode(*item, true, false, landscape);
}

// -------------------------------------------------------------------
// B5d2a: Row card artwork — loading state only (no rendering)
// -------------------------------------------------------------------

std::string HomeScreen::rowArtworkKey(const MediaItem& item, bool landscape)
{
    return homeArtworkKey(item, landscape);
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
    m_rowArtworkCache.evictIfNeeded(protectedRowArtworkKeys());
}

void HomeScreen::reviveArrivedArtwork(unsigned dtMs)
{
    constexpr unsigned kPeriodMs = 1000;
    constexpr int kChecksPerPass = 64;
    m_reviveMs += dtMs;
    if (m_reviveMs < kPeriodMs || !m_artworkController)
        return;
    m_reviveMs = 0;
    auto it = m_rowArtworkCache.entries.upper_bound(m_reviveCursor);
    for (int checked = 0; checked < kChecksPerPass; ++checked) {
        if (it == m_rowArtworkCache.entries.end()) {
            m_reviveCursor.clear();
            return;
        }
        m_reviveCursor = it->first;
        if (it->second.status != RowArtworkStatus::Failed) {
            ++it;
            continue;
        }
        std::string itemId, tag;
        ImageType type = ImageType::Primary;
        int w = 0, h = 0;
        const bool arrived = parseArtworkIdentityKey(it->first, itemId, type, tag, w, h) &&
                             ImageCache::isCached(itemId, type, tag, w, h);
        if (arrived) {
            m_artworkController->resetDecodeAttempts(it->first);
            it = m_rowArtworkCache.entries.erase(it);
        } else {
            ++it;
        }
    }
}

void HomeScreen::touchRowArtwork(const std::string& key)
{
    m_rowArtworkCache.touch(key);
}

void HomeScreen::storeDecodedRowArtwork(const std::string& key, DecodedImage image)
{
    m_rowArtworkCache.store(key, std::move(image), protectedRowArtworkKeys());
}

void HomeScreen::prepareCardSurface(const std::string& cacheKey, const DecodedImage& img, int boxW,
                                    int boxH)
{
    m_rowArtworkCache.prepareCardSurface(cacheKey, img, boxW, boxH);
}

void HomeScreen::freeAllCardSurfaces()
{
    m_rowArtworkCache.clearCardSurfaces();
}

void HomeScreen::submitDecode(const MediaItem& item, bool highPriority, bool shows, bool landscape)
{
    std::string key = rowArtworkKey(item, landscape);
    DisplayArtwork a = displayArtworkForItem(item, landscape);
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
                m_rowArtworkCache.entries[key].status = RowArtworkStatus::Failed;
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
        const ArtworkBox box = homeRailCardSize(homeRailIsLandscape(rows[rowIdx].label));
        const bool landscape = homeRailIsLandscape(rows[rowIdx].label);
        for (const auto& item : rows[rowIdx].items) {
            const int screenX = cardX - rowCardScrollOffset(rowIdx, m_activeRow, m_cardScroll);
            if (screenX + box.w >= HOME_RAIL_MARGIN && screenX <= 640 - HOME_RAIL_MARGIN) {
                const std::string key = rowArtworkKey(item, landscape);
                if (!key.empty())
                    keys.insert(key);
            }
            if (screenX > 640 - HOME_RAIL_MARGIN)
                break;
            cardX += box.w + HOME_RAIL_GAP;
        }
    }
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
        if (m_rowArtworkCache.entries.find(key) == m_rowArtworkCache.entries.end())
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
            if (key.empty() ||
                m_rowArtworkCache.entries.find(key) != m_rowArtworkCache.entries.end())
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
        const ArtworkBox sz = homeRailCardSize(homeRailIsLandscape(row.label));
        for (int ci = 0; ci < (int)row.items.size(); ++ci) {
            int screenX = cardAccumX - rowCardScrollOffset(rowIdx, m_activeRow, m_cardScroll);
            if (screenX + sz.w < HOME_RAIL_MARGIN) {
                cardAccumX += sz.w + HOME_RAIL_GAP;
                continue;
            }
            if (screenX > 640 - HOME_RAIL_MARGIN)
                break;
            const bool landscape = homeRailIsLandscape(row.label);
            std::string key = rowArtworkKey(row.items[ci], landscape);
            if (!key.empty() &&
                m_rowArtworkCache.entries.find(key) == m_rowArtworkCache.entries.end())
                submitDecode(row.items[ci], false, false, landscape);
            cardAccumX += sz.w + HOME_RAIL_GAP;
        }
    }
}

} // namespace miyoofin
