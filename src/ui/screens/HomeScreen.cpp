#include "HomeScreen.hpp"
#include "SeriesScreen.hpp"
#include "MovieDetailsScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../ArtworkLayout.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../app/ScreenStack.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cstring>
#include <map>

namespace miyoofin {

// Layout constants
static constexpr int TAB_Y       = 0;
static constexpr int TAB_H       = 24;
static constexpr int INFO_Y      = 26;
static constexpr int INFO_H      = 110;
static constexpr int ROWS_Y      = INFO_Y + INFO_H + 2;
static constexpr int BOTTOM_H    = 18;
static constexpr int CARD_GAP    = 6;
static constexpr int ROW_STRIP_H = 96;  // max card height across all types
static constexpr int ROW_LABEL_H = 18;
static constexpr int VISIBLE_ROWS = 3;

// Selected artwork box origin (top-left of info panel)
static constexpr int ART_X = 8;
static constexpr int ART_Y = INFO_Y + 6;   // 32
// Width and height are now computed per-item via artworkBoxSize()

HomeScreen::HomeScreen(const Session &session)
    : m_activeTab(0), m_activeRow(0), m_activeCard(0)
    , m_rowScroll(0), m_cardScroll(0)
    , m_session(session)
    , m_userName(session.userName)
{
    // Placeholder tabs until fetch completes
    m_tabs.push_back({"Home", {{"", {}}}});
    m_tabs.push_back({"Movies", {{"", {}}}});
    m_tabs.push_back({"Shows", {{"", {}}}});
    m_tabs.push_back({"Search", {{"", {}}}});
    m_tabs.push_back({"Downloads", {{"", {}}}});
}

HomeScreen::~HomeScreen()
{
    if (m_fetchThread.joinable())
        m_fetchThread.join();
}

const TabData &HomeScreen::currentTab() const
{
    return m_tabs[m_activeTab];
}

const MediaRow *HomeScreen::currentRow() const
{
    const auto &rows = currentTab().rows;
    if (m_activeRow < (int)rows.size())
        return &rows[m_activeRow];
    return nullptr;
}

const MediaItem *HomeScreen::currentItem() const
{
    const MediaRow *row = currentRow();
    if (row && m_activeCard < (int)row->items.size())
        return &row->items[m_activeCard];
    return nullptr;
}

void HomeScreen::clampNavigation()
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) {
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        return;
    }
    if (m_activeRow < 0) m_activeRow = 0;
    if (m_activeRow >= (int)rows.size()) m_activeRow = (int)rows.size() - 1;
    const auto &items = rows[m_activeRow].items;
    if (items.empty()) {
        m_activeCard = 0; m_cardScroll = 0;
        return;
    }
    if (m_activeCard < 0) m_activeCard = 0;
    if (m_activeCard >= (int)items.size()) m_activeCard = (int)items.size() - 1;
    if (m_activeRow < m_rowScroll) m_rowScroll = m_activeRow;
    if (m_activeRow >= m_rowScroll + VISIBLE_ROWS)
        m_rowScroll = m_activeRow - VISIBLE_ROWS + 1;
    // Horizontal: m_cardScroll is a pixel offset, clamp so active card is visible
    static constexpr int HMARGIN = 4;
    m_cardScroll = clampCardScroll(items, m_activeCard, m_cardScroll,
                                    640, HMARGIN, CARD_GAP);
}

void HomeScreen::enter()
{
    printf("[HomeScreen] enter (tab=%d) user=%s\n", m_activeTab,
           m_userName.c_str());
    if (m_loadState == LoadState::Loading && !m_fetchDone)
        startFetch();
}

void HomeScreen::leave()
{
    printf("[HomeScreen] leave\n");
}

bool HomeScreen::handleAction(Action action)
{
    if (m_logoutArmed && action != Action::ActionsMenu) {
        m_logoutArmed = false;
        m_logoutTimer = 0;
    }

    // Loading: only allow logout
    if (m_loadState == LoadState::Loading) {
        if (action == Action::ActionsMenu && !m_logoutArmed) {
            m_logoutArmed = true; m_logoutTimer = 3000; return true;
        }
        return false;
    }

    // Error: allow retry (A) and logout (Y)
    if (m_loadState == LoadState::Error) {
        if (action == Action::Confirm) {
            m_loadState = LoadState::Loading;
            m_fetchDone = false; m_fetchError.clear(); m_fetchResult.clear();
            startFetch(); return true;
        }
        if (action == Action::ActionsMenu && !m_logoutArmed) {
            m_logoutArmed = true; m_logoutTimer = 3000; return true;
        }
        return false;
    }

    // Ready: normal navigation
    switch (action) {
    case Action::Up:     m_activeRow--; clampNavigation(); return true;
    case Action::Down:   m_activeRow++; clampNavigation(); return true;
    case Action::Left:   m_activeCard--; clampNavigation(); return true;
    case Action::Right:  m_activeCard++; clampNavigation(); return true;
    case Action::NextTab:
        m_activeTab = (m_activeTab + 1) % (int)m_tabs.size();
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); return true;
    case Action::PrevTab:
        m_activeTab--;
        if (m_activeTab < 0) m_activeTab = (int)m_tabs.size() - 1;
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); return true;
    case Action::Search:
        m_activeTab = 3; m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0; return true;
    case Action::ActionsMenu:
        if (m_logoutArmed) { m_logoutRequested = true; }
        else { m_logoutArmed = true; m_logoutTimer = 3000; }
        return true;
    case Action::Confirm: {
        const MediaItem *item = currentItem();
        if (item) {
            printf("[HomeScreen] Select: %s (%s)\n",
                   item->title.c_str(), item->type.c_str());
            if (item->type == "show") {
                m_stack->push(std::make_unique<SeriesScreen>(m_session, *item));
                return true;
            }
            if (item->type == "movie") {
                m_stack->push(std::make_unique<MovieDetailsScreen>(m_session, *item));
                return true;
            }
        }
        return true;
    }
    case Action::Back:
        if (m_logoutArmed) { m_logoutArmed = false; m_logoutTimer = 0; return true; }
        return false;
    default: return false;
    }
}

void HomeScreen::update(Uint32 dt)
{
    if (m_logoutArmed && !m_logoutRequested) {
        if (dt >= m_logoutTimer) { m_logoutTimer = 0; m_logoutArmed = false; }
        else m_logoutTimer -= dt;
    }
    if (m_loadState == LoadState::Loading && m_fetchDone)
        finishFetch();

    // Attempt selected artwork load (identity guard prevents repeats)
    if (m_loadState == LoadState::Ready)
        tryLoadSelectedArtwork();

    // B5d2a: load at most one new row artwork per update cycle
    if (m_loadState == LoadState::Ready)
        tryLoadOneRowArtwork();
}

void HomeScreen::render(SDL_Surface *fb)
{
    drawTabBar(fb);

    if (m_loadState == LoadState::Loading) {
        drawLoadingState(fb);
        drawBottomHints(fb);
        return;
    }
    if (m_loadState == LoadState::Error) {
        drawErrorState(fb);
        drawBottomHints(fb);
        return;
    }

    // Ready
    const TabData &tab = currentTab();
    if (m_activeTab == 3 || m_activeTab == 4) {
        drawPlaceholderTab(fb, m_activeTab == 3 ?
            "Search - not yet implemented" :
            "Downloads - not yet implemented");
    } else if (tab.rows.size() == 1 && tab.rows[0].items.empty()
               && tab.rows[0].label.empty()) {
        drawPlaceholderTab(fb, "No content");
    } else {
        bool hasItems = false;
        for (const auto &r : tab.rows)
            if (!r.items.empty()) { hasItems = true; break; }
        if (!hasItems) {
            drawPlaceholderTab(fb, tab.name == "Movies" ?
                "No movies on this server" :
                tab.name == "Shows" ? "No shows on this server" : "No content");
        } else {
            drawInfoPanel(fb);
            drawRowList(fb);
        }
    }
    drawBottomHints(fb);
}

void HomeScreen::startFetch()
{
    m_fetchDone = false;
    m_fetchError.clear();
    m_fetchResult.clear();

    std::string url   = m_session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid   = m_session.userId;
    std::string devId = m_session.deviceId;

    m_fetchThread = std::thread([this, url, token, uid, devId]() {
        std::string err;

        // 1. Get library views
        std::vector<LibraryView> views;
        if (!JellyfinApi::getViews(url, token, uid, devId, views, err)) {
            m_fetchError = err;
            m_fetchDone = true;
            return;
        }
        printf("[HomeScreen] Got %zu library views\n", views.size());

        // 2. Continue watching (non-fatal if fails)
        std::vector<MediaItem> cw;
        std::string cwErr;
        if (!JellyfinApi::getResumeItems(url, token, uid, devId, 12, cw, cwErr))
            printf("[HomeScreen] Continue watching: %s\n", cwErr.c_str());

        // 3. Recently added (non-fatal)
        std::vector<MediaItem> ra;
        std::string raErr;
        if (!JellyfinApi::getLatestItems(url, token, uid, devId, 16, ra, raErr))
            printf("[HomeScreen] Recently added: %s\n", raErr.c_str());

        // 4. Per-library items
        std::vector<std::pair<std::string, std::vector<MediaItem>>> moviesByView;
        std::vector<std::pair<std::string, std::vector<MediaItem>>> showsByView;
        for (const auto &v : views) {
            if (v.collectionType == "movies") {
                std::vector<MediaItem> items; std::string ie;
                if (JellyfinApi::getLibraryItems(url, token, uid, devId,
                        v.id, "Movie", 50, items, ie))
                    moviesByView.push_back({v.name, std::move(items)});
                else printf("[HomeScreen] Movies '%s': %s\n",
                            v.name.c_str(), ie.c_str());
            } else if (v.collectionType == "tvshows") {
                std::vector<MediaItem> items; std::string ie;
                if (JellyfinApi::getLibraryItems(url, token, uid, devId,
                        v.id, "Series", 50, items, ie))
                    showsByView.push_back({v.name, std::move(items)});
                else printf("[HomeScreen] Shows '%s': %s\n",
                            v.name.c_str(), ie.c_str());
            }
        }

        m_fetchResult = JellyfinApi::buildTabs(
            views, cw, ra, moviesByView, showsByView);
        m_fetchDone = true;
    });
    m_fetchThread.detach();
}

void HomeScreen::finishFetch()
{
    m_fetchDone = false;
    if (!m_fetchError.empty()) {
        m_loadState = LoadState::Error;
        printf("[HomeScreen] Fetch failed: %s\n", m_fetchError.c_str());
        return;
    }
    m_tabs = std::move(m_fetchResult);
    m_loadState = LoadState::Ready;
    clampNavigation();
    printf("[HomeScreen] Library loaded: %zu tabs\n", m_tabs.size());
}

void HomeScreen::tryLoadSelectedArtwork()
{
    const MediaItem *item = currentItem();
    if (!item) {
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    // Per-type artwork box dimensions
    ArtworkBox box = artworkBoxSize(*item);

    // Look for a Primary image tag
    auto it = item->imageTags.find("Primary");
    if (it == item->imageTags.end() || it->second.empty()) {
        // No Primary tag — clear artwork, keep placeholder
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    // Build identity key from item id + primary tag
    std::string key = item->id + ":" + it->second;

    // Already attempted this exact selection?  Do not retry.
    if (m_selectedArtworkAttempted && m_selectedArtworkId == key)
        return;

    // New selection — reset and attempt once
    m_selectedArtwork = {};
    m_selectedArtworkId = key;
    m_selectedArtworkAttempted = true;

    const std::string &tag = it->second;
    printf("[HomeScreen] Artwork: loading %s tag=%s (%dx%d)\n",
           item->id.c_str(), tag.c_str(), box.w, box.h);

    // 1. Check disk cache
    std::vector<unsigned char> jpegData;
    if (ImageCache::isCached(item->id, ImageType::Primary, tag, box.w, box.h)) {
        jpegData = ImageCache::readCached(item->id, ImageType::Primary, tag, box.w, box.h);
        printf("[HomeScreen] Artwork: cache hit (%zu bytes)\n", jpegData.size());
    }

    // 2. If not cached, synchronous HTTP request
    if (jpegData.empty()) {
        std::string url = buildImageUrl(
            m_session.serverUrl, item->id, ImageType::Primary, tag, box.w, box.h);

        HttpClient client;
        client.setTimeoutSec(8);
        auto headers = JellyfinApi::buildAuthHeaders(
            m_session.accessToken, m_session.deviceId);

        BinaryHttpResponse response;
        std::string error;
        if (!client.getBinary(url, headers, response, error, 512 * 1024)) {
            printf("[HomeScreen] Artwork fetch failed: %s\n", error.c_str());
            return;
        }
        if (!response.ok()) {
            printf("[HomeScreen] Artwork HTTP %ld\n", response.status);
            return;
        }

        jpegData = std::move(response.data);
        printf("[HomeScreen] Artwork: downloaded %zu bytes\n", jpegData.size());

        // Cache to disk (best-effort)
        ImageCache::writeToCache(item->id, ImageType::Primary, tag, box.w, box.h,
                                 jpegData.data(), jpegData.size());
    }

    // 3. Decode JPEG
    m_selectedArtwork = ImageDecoder::decodeJpeg(jpegData.data(), jpegData.size());
    if (m_selectedArtwork.empty()) {
        printf("[HomeScreen] Artwork: decode failed\n");
    } else {
        printf("[HomeScreen] Artwork: decoded %dx%d\n",
               m_selectedArtwork.width, m_selectedArtwork.height);
    }
}

// -------------------------------------------------------------------
// B5d2a: Row card artwork — loading state only (no rendering)
// -------------------------------------------------------------------

std::string HomeScreen::rowArtworkKey(const MediaItem &item)
{
    return buildRowArtworkKey(item);
}

void HomeScreen::evictRowArtworkIfNeeded()
{
    while ((int)m_rowArtworkOrder.size() > ROW_ARTWORK_RAM_LIMIT) {
        std::string oldKey = m_rowArtworkOrder.front();
        m_rowArtworkOrder.erase(m_rowArtworkOrder.begin());

        m_rowArtwork.erase(oldKey);
    }
}

void HomeScreen::tryLoadOneRowArtwork()
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) return;

    // Scan horizontally visible cards and find ONE not-yet-attempted candidate
    static constexpr int HMARGIN = 4;
    std::string candidate;
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
            if (!key.empty() && m_rowArtwork.find(key) == m_rowArtwork.end()) {
                if (candidate.empty())
                    candidate = key;
            }
            cardAccumX += sz.w + CARD_GAP;
        }
    }

    if (candidate.empty()) return;

    // Mark as attempted immediately (tentatively Failed until proven otherwise)
    m_rowArtwork[candidate].status = RowArtworkStatus::Failed;

    // Find the matching item to get dimensions and tag
    const MediaItem *matchItem = nullptr;
    for (int ri = 0; ri < VISIBLE_ROWS && !matchItem; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        const MediaRow &row = rows[rowIdx];
        for (int ci = 0; ci < (int)row.items.size(); ++ci) {
            if (rowArtworkKey(row.items[ci]) == candidate) {
                matchItem = &row.items[ci];
                break;
            }
        }
    }
    if (!matchItem) return;

    ArtworkBox box = artworkBoxSize(*matchItem);
    auto tagIt = matchItem->imageTags.find("Primary");
    if (tagIt == matchItem->imageTags.end()) return;
    const std::string &tag = tagIt->second;
    const std::string &itemId = matchItem->id;

    printf("[HomeScreen] RowArtwork: loading %s (%dx%d)\n",
           candidate.c_str(), box.w, box.h);

    // 1. Check disk cache
    std::vector<unsigned char> jpegData;
    if (ImageCache::isCached(itemId, ImageType::Primary, tag, box.w, box.h))
        jpegData = ImageCache::readCached(itemId, ImageType::Primary, tag, box.w, box.h);

    // 2. If not cached, synchronous HTTP request
    if (jpegData.empty()) {
        std::string url = buildImageUrl(
            m_session.serverUrl, itemId, ImageType::Primary, tag, box.w, box.h);

        HttpClient client;
        client.setTimeoutSec(5);
        auto headers = JellyfinApi::buildAuthHeaders(
            m_session.accessToken, m_session.deviceId);

        BinaryHttpResponse response;
        std::string error;
        if (!client.getBinary(url, headers, response, error, 256 * 1024)) {
            printf("[HomeScreen] RowArtwork fetch failed: %s\n", error.c_str());
            return;
        }
        if (!response.ok()) {
            printf("[HomeScreen] RowArtwork HTTP %ld\n", response.status);
            return;
        }
        jpegData = std::move(response.data);
        ImageCache::writeToCache(itemId, ImageType::Primary, tag, box.w, box.h,
                                 jpegData.data(), jpegData.size());
    }

    // 3. Decode JPEG
    DecodedImage img = ImageDecoder::decodeJpeg(jpegData.data(), jpegData.size());
    if (img.empty()) {
        printf("[HomeScreen] RowArtwork: decode failed\n");
        return;  // status already Failed
    }

    printf("[HomeScreen] RowArtwork: decoded %s (%dx%d)\n",
           candidate.c_str(), img.width, img.height);

    // Store decoded image
    m_rowArtwork[candidate].status = RowArtworkStatus::Loaded;
    m_rowArtwork[candidate].image = std::move(img);
    m_rowArtworkOrder.push_back(candidate);
    evictRowArtworkIfNeeded();
}

void HomeScreen::drawTabBar(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 0, TAB_Y, 640, TAB_H,
        Theme::BG_R*2/3, Theme::BG_G*2/3, Theme::BG_B*2/3, 255);
    BitmapFont::fillRect(fb, 0, TAB_Y+TAB_H-1, 640, 1,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 100);
    int x = 8, tabY = TAB_Y + (TAB_H - BitmapFont::GLYPH_H)/2;
    for (int i = 0; i < (int)m_tabs.size(); ++i) {
        const char *name = m_tabs[i].name.c_str();
        if (i == m_activeTab) {
            BitmapFont::drawString(fb,x,tabY,name,
                Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,
                Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
            int tw = (int)::strlen(name) * BitmapFont::GLYPH_W;
            BitmapFont::fillRect(fb, x, TAB_Y+TAB_H-3, tw, 2,
                Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,200);
        } else {
            BitmapFont::drawString(fb,x,tabY,name,
                Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
                Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
        }
        x += (int)::strlen(name) * BitmapFont::GLYPH_W + 16;
    }
}

void HomeScreen::drawInfoPanel(SDL_Surface *fb)
{
    const MediaItem *item = currentItem();
    if (!item) return;
    BitmapFont::fillRect(fb,0,INFO_Y,640,INFO_H,24,24,32,255);
    BitmapFont::fillRect(fb,0,INFO_Y,640,1,
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,60);
    ArtworkBox box = artworkBoxSize(*item);
    int px=ART_X, py=ART_Y, pw=box.w, ph=box.h;
    // Placeholder colour behind everything
    BitmapFont::fillRect(fb,px,py,pw,ph,item->artR,item->artG,item->artB,255);

    // Render decoded artwork if available, aspect-fit centred
    if (!m_selectedArtwork.empty()) {
        int imgW = m_selectedArtwork.width;
        int imgH = m_selectedArtwork.height;
        float imgAspect = (float)imgW / (float)imgH;
        float boxAspect = (float)pw / (float)ph;
        int drawW, drawH;
        if (imgAspect > boxAspect) {
            // Wider than box — fit to width
            drawW = pw;
            drawH = (int)(pw / imgAspect + 0.5f);
            if (drawH > ph) drawH = ph;
        } else {
            // Taller than box — fit to height
            drawH = ph;
            drawW = (int)(ph * imgAspect + 0.5f);
            if (drawW > pw) drawW = pw;
        }
        int drawX = px + (pw - drawW) / 2;
        int drawY = py + (ph - drawH) / 2;

        // Create an SDL surface wrapping the RGBA pixel data.
        // The DecodedImage (m_selectedArtwork) keeps the pixels alive.
        SDL_Surface *imgSurface = SDL_CreateRGBSurfaceFrom(
            (void *)m_selectedArtwork.pixels.data(),
            imgW, imgH,
            32,                    // bits per pixel
            imgW * 4,              // pitch (bytes per row)
            0x000000FF,            // R mask
            0x0000FF00,            // G mask
            0x00FF0000,            // B mask
            0xFF000000);           // A mask
        if (imgSurface) {
            SDL_Rect srcRect = {0, 0, imgW, imgH};
            SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
            SDL_BlitScaled(imgSurface, &srcRect, fb, &dstRect);
            SDL_FreeSurface(imgSurface);
        }
    }

    BitmapFont::drawRect(fb,px,py,pw,ph,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B);
    int mx=px+pw+10, my=py+2;
    BitmapFont::drawString(fb,mx,my,item->title.c_str(),
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32);
    my += BitmapFont::GLYPH_H + 2;
    char line1[128];
    std::snprintf(line1,sizeof(line1),"%d  |  %s",item->year,item->genre.c_str());
    BitmapFont::drawString(fb,mx,my,line1,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    my += BitmapFont::GLYPH_H + 2;
    char rs[32];
    int fs=(int)item->rating, idx=0;
    for (int s=0;s<fs;++s) rs[idx++]='*';
    if (item->rating-fs>=0.25f) rs[idx++]='.';
    rs[idx]='\0';
    BitmapFont::drawString(fb,mx,my,rs,
        Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,24,24,32);
    char rn[16];
    std::snprintf(rn,sizeof(rn),"  %.1f/5.0",item->rating);
    BitmapFont::drawString(fb,mx+(int)::strlen(rs)*BitmapFont::GLYPH_W,my,rn,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    my += BitmapFont::GLYPH_H + 2;
    char ov[256];
    std::snprintf(ov,sizeof(ov),"%s",item->overview.c_str());
    if ((int)::strlen(ov) > 55) { ov[55]='\0'; std::strcat(ov,"..."); }
    BitmapFont::drawString(fb,mx,my,ov,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    BitmapFont::fillRect(fb,0,INFO_Y+INFO_H,640,1,
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,60);
}

void HomeScreen::drawRowList(SDL_Surface *fb)
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) return;
    static constexpr int VGAP = 4;
    static constexpr int HMARGIN = 4;
    for (int ri=0; ri<VISIBLE_ROWS; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        const MediaRow &row = rows[rowIdx];
        int rowY = ROWS_Y + ri * (ROW_LABEL_H + ROW_STRIP_H + VGAP + 4);
        int caY = rowY + ROW_LABEL_H;
        char label[64];
        std::snprintf(label,sizeof(label),"  %s",row.label.c_str());
        BitmapFont::drawString(fb,4,rowY,label,
            rowIdx==m_activeRow?Theme::ACCENT_R:Theme::TEXT_R,
            rowIdx==m_activeRow?Theme::ACCENT_G:Theme::TEXT_G,
            rowIdx==m_activeRow?Theme::ACCENT_B:Theme::TEXT_B,
            Theme::BG_R,Theme::BG_G,Theme::BG_B);
        // Draw cards with per-item sizing and pixel-scroll offset
        int cardAccumX = HMARGIN;
        for (int ci=0; ci<(int)row.items.size(); ++ci) {
            ArtworkBox sz = artworkBoxSize(row.items[ci]);
            int screenX = cardAccumX - m_cardScroll;
            if (screenX + sz.w < HMARGIN) {
                // Fully off-screen left
                cardAccumX += sz.w + CARD_GAP;
                continue;
            }
            if (screenX > 640 - HMARGIN) break;
            bool sel = (rowIdx==m_activeRow && ci==m_activeCard);
            int cardScreenY = caY + (ROW_STRIP_H - sz.h) / 2;
            drawCard(fb,screenX,cardScreenY,sz.w,sz.h,row.items[ci],sel);
            cardAccumX += sz.w + CARD_GAP;
        }
    }
}

void HomeScreen::drawCard(SDL_Surface *fb,int x,int y,int w,int h,
                          const MediaItem &item,bool selected)
{
    BitmapFont::fillRect(fb,x,y,w,h,item.artR,item.artG,item.artB,255);

    // B5d2b: render loaded row artwork over the placeholder
    {
        std::string key = rowArtworkKey(item);
        if (!key.empty()) {
            auto it = m_rowArtwork.find(key);
            if (it != m_rowArtwork.end()
                && it->second.status == RowArtworkStatus::Loaded
                && !it->second.image.empty())
            {
                const DecodedImage &img = it->second.image;
                int imgW = img.width;
                int imgH = img.height;
                float imgAspect = (float)imgW / (float)imgH;
                float boxAspect = (float)w / (float)h;
                int drawW, drawH;
                if (imgAspect > boxAspect) {
                    drawW = w;
                    drawH = (int)(w / imgAspect + 0.5f);
                    if (drawH > h) drawH = h;
                } else {
                    drawH = h;
                    drawW = (int)(h * imgAspect + 0.5f);
                    if (drawW > w) drawW = w;
                }
                int drawX = x + (w - drawW) / 2;
                int drawY = y + (h - drawH) / 2;

                SDL_Surface *imgSurface = SDL_CreateRGBSurfaceFrom(
                    (void *)img.pixels.data(),
                    imgW, imgH,
                    32,
                    imgW * 4,
                    0x000000FF,
                    0x0000FF00,
                    0x00FF0000,
                    0xFF000000);
                if (imgSurface) {
                    SDL_Rect srcRect = {0, 0, imgW, imgH};
                    SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
                    SDL_BlitScaled(imgSurface, &srcRect, fb, &dstRect);
                    SDL_FreeSurface(imgSurface);
                }
            }
        }
    }

    int ty = y + h - BitmapFont::GLYPH_H - 2;
    BitmapFont::fillRect(fb,x,ty,w,BitmapFont::GLYPH_H+2,0,0,0,160);
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s",item.title.c_str());
    int mcc = (w-4)/BitmapFont::GLYPH_W;
    if ((int)::strlen(buf) > mcc) {
        buf[mcc-1]='.'; buf[mcc-2]='.'; buf[mcc]='\0';
    }
    BitmapFont::drawString(fb,x+2,ty+1,buf,255,255,255,0,0,0);
    if (selected) {
        BitmapFont::drawRect(fb,x-2,y-2,w+4,h+4,255,220,40);
        BitmapFont::drawRect(fb,x-1,y-1,w+2,h+2,255,255,120);
    } else {
        BitmapFont::drawRect(fb,x,y,w,h,
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B);
    }
}

void HomeScreen::drawPlaceholderTab(SDL_Surface *fb, const char *message)
{
    BitmapFont::drawString(fb,8,200,message,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
        Theme::BG_R,Theme::BG_G,Theme::BG_B);
}

void HomeScreen::drawBottomHints(SDL_Surface *fb)
{
    int y = 480 - BOTTOM_H;
    BitmapFont::fillRect(fb,0,y,640,BOTTOM_H,
        Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3,255);

    if (m_loadState == LoadState::Loading) {
        BitmapFont::drawString(fb,8,y+2,"Y=Logout",
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else if (m_loadState == LoadState::Error) {
        BitmapFont::drawString(fb,8,y+2,"A=Retry  Y=Logout",
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else if (m_logoutArmed && !m_logoutRequested) {
        int secs = (m_logoutTimer + 999) / 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Press Y again to confirm logout (%d)", secs);
        BitmapFont::drawString(fb,8,y+2,buf,
            Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else {
        BitmapFont::drawString(fb,8,y+2,
            "A=Select  B=Back  L/R=Tabs  X=Search  Y=Logout",
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    }
}

void HomeScreen::drawLoadingState(SDL_Surface *fb)
{
    char userBuf[64];
    std::snprintf(userBuf, sizeof(userBuf), "Logged in as %s",
                  m_userName.c_str());
    int ux = (fb->w - (int)::strlen(userBuf) * BitmapFont::GLYPH_W) / 2;
    int uy = fb->h / 3;
    BitmapFont::drawString(fb, ux, uy, userBuf,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    static int dotPhase = 0;
    dotPhase = (dotPhase + 1) % 60;
    int dots = dotPhase / 15;
    char buf[32] = "Loading library";
    for (int i = 0; i < dots; ++i) std::strcat(buf, ".");
    int mx = (fb->w - (int)::strlen(buf) * BitmapFont::GLYPH_W) / 2;
    int my = uy + BitmapFont::GLYPH_H + 16;
    BitmapFont::drawString(fb, mx, my, buf,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

void HomeScreen::drawErrorState(SDL_Surface *fb)
{
    const char *header = "Failed to load library";
    int hx = (fb->w - (int)::strlen(header) * BitmapFont::GLYPH_W) / 2;
    int hy = fb->h / 3;
    BitmapFont::drawString(fb, hx, hy, header,
        Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    char errBuf[128];
    std::snprintf(errBuf, sizeof(errBuf), "%s", m_fetchError.c_str());
    int maxChars = (640 - 16) / BitmapFont::GLYPH_W;
    if ((int)::strlen(errBuf) > maxChars) errBuf[maxChars] = '\0';
    int ex = (fb->w - (int)::strlen(errBuf) * BitmapFont::GLYPH_W) / 2;
    int ey = hy + BitmapFont::GLYPH_H + 8;
    BitmapFont::drawString(fb, ex, ey, errBuf,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    const char *hint = "Press A to retry";
    int hix = (fb->w - (int)::strlen(hint) * BitmapFont::GLYPH_W) / 2;
    int hiy = ey + BitmapFont::GLYPH_H + 16;
    BitmapFont::drawString(fb, hix, hiy, hint,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

} // namespace miyoofin
