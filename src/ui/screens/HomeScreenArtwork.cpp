#include "HomeScreen.hpp"
#include "../ArtworkLayout.hpp"
#include "../ShowsBrowser.hpp"
#include "../../cache/ImageCache.hpp"

namespace miyoofin {

static constexpr int CARD_GAP = 6;
static constexpr int VISIBLE_ROWS = 3;
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;

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
    RowArtworkEntry &entry = m_rowArtwork[key];
    entry.status = RowArtworkStatus::Loaded;
    entry.image = std::make_shared<DecodedImage>(std::move(image));
    touchRowArtwork(key);
    evictRowArtworkIfNeeded();
}

void HomeScreen::submitDecode(const MediaItem &item, bool highPriority, bool shows)
{
    std::string key=rowArtworkKey(item); DisplayArtwork a=displayArtworkForItem(item);
    if(key.empty() || !a.valid()) return;
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    if(!m_decodeOutstanding.insert(key).second) return;
    if(m_decodeJobs.size() >= 32) { m_decodeOutstanding.erase(key); return; }
    DecodeJob job{key,{item.id,a.imageType,a.tag,a.width,a.height},shows};
    if(highPriority) m_decodeJobs.push_front(std::move(job)); else m_decodeJobs.push_back(std::move(job));
    m_decodeWake.notify_one();
}

void HomeScreen::decodeWorker()
{
    for (;;) { DecodeJob job; { std::unique_lock<std::mutex> lock(m_decodeMutex); m_decodeWake.wait(lock,[&]{return m_stopDecodeWorker||!m_decodeJobs.empty();}); if(m_stopDecodeWorker) return; job=std::move(m_decodeJobs.front());m_decodeJobs.pop_front(); }
        auto bytes=ImageCache::readCached(job.artwork.itemId,job.artwork.imageType,job.artwork.imageTag,job.artwork.width,job.artwork.height);
        DecodedImage image=bytes.empty()?DecodedImage{}:ImageDecoder::decodeJpeg(bytes.data(),bytes.size());
        std::lock_guard<std::mutex> lock(m_decodeMutex); m_decodeResults.push_back({std::move(job.key),std::move(image),job.shows,!bytes.empty()});
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
            && protectedKeys.find(result.key) == protectedKeys.end()) continue;
        if (!result.cachePresent) {
            // Poster sync may populate this key later; do not make a cache miss
            // a permanent failure.
            continue;
        }
        if (result.image.empty()) {
            m_rowArtwork[result.key].status = RowArtworkStatus::Failed;
        } else {
            if(result.key==m_selectedArtworkId) {
                m_selectedArtwork=result.image;
                m_selectedArtworkAttempted=true;
            }
            storeDecodedRowArtwork(result.key, std::move(result.image));
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
            const int screenX = cardX - m_cardScroll;
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
            int screenX = cardAccumX - m_cardScroll;
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
