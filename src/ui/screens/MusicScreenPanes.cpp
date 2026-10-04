#include "MusicScreen.hpp"
#include "MusicScreenInternal.hpp"
#include "../../music/MusicApi.hpp"
#include "../../music/MusicPaths.hpp"
#include "../../music/MusicParse.hpp"
#include "../UiKit.hpp"
#include "TextEntryScreen.hpp"
#include "../ClockSettings.hpp"
#include "../../app/ScreenStack.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace miyoofin {

using namespace music_screen_detail;

int MusicScreen::detailHeaderHeight(const MusicPane& pane) const
{
    switch (pane.frame.kind) {
    case MusicPaneKind::AlbumTracks:
    case MusicPaneKind::PlaylistTracks:
    case MusicPaneKind::DownloadedTracks:
    case MusicPaneKind::ArtistAlbums:
        return 108;
    default:
        return 0;
    }
}

bool MusicScreen::miniPlayerVisible() const
{
    return m_playerView.state != music::PlayState::Idle;
}

int MusicScreen::contentBottom() const
{
    return miniPlayerVisible() ? 404 : 456;
}

bool MusicScreen::isGrid(const MusicPane& pane) const
{
    return m_settings && m_settings->albumGrid.load() && pane.frame.kind == MusicPaneKind::Albums;
}

int MusicScreen::visibleRows(const MusicPane& pane) const
{
    const int top = 76 + detailHeaderHeight(pane);
    if (isGrid(pane))
        return kGridCols * std::max(1, (contentBottom() - top) / kGridCellH);
    if (pane.frame.kind == MusicPaneKind::Settings)
        return std::max(1, (contentBottom() - top) / 66); // tall cards, as in MiyooFin Settings
    return std::max(1, (contentBottom() - top) / kRowHeight);
}

void MusicScreen::requestPane(MusicPane& pane, bool nextPage)
{
    pane.requested = true;
    pane.failed = false;
    switch (pane.frame.kind) {
    case MusicPaneKind::Home: {
        music::ListingRequest recent;
        recent.kind = music::Listing::RecentAlbums;
        recent.limit = 12;
        pane.ticket = m_library->requestList(recent);
        music::ListingRequest played;
        played.kind = music::Listing::RecentlyPlayed;
        played.limit = 15;
        pane.ticket2 = m_library->requestList(played);
        break;
    }
    case MusicPaneKind::Downloads:
    case MusicPaneKind::DownloadedTracks:
    case MusicPaneKind::Settings:
        break; // built from local state, not requested
    default:
        pane.ticket = m_library->requestList(
            requestFor(pane.frame, nextPage ? static_cast<int>(pane.rows.size()) : 0));
        break;
    }
}

std::vector<MusicScreen::HomeSegment> MusicScreen::homeSegments(const MusicPane& pane) const
{
    std::vector<HomeSegment> segs;
    const int count = static_cast<int>(pane.rows.size());
    for (int i = 0; i < count;) {
        const MusicRow& row = pane.rows[i];
        if (row.kind == MusicRow::Kind::Heading) {
            HomeSegment s;
            s.first = i + 1;
            int j = i + 1;
            while (j < count && pane.rows[j].kind != MusicRow::Kind::Heading)
                ++j;
            s.count = j - (i + 1);
            if (s.count > 0)
                segs.push_back(s);
            i = j;
        } else { // "Continue listening"
            segs.push_back({i, 1, true});
            ++i;
        }
    }
    return segs;
}

int MusicScreen::homeSegmentHeight(const HomeSegment& s) const
{
    return s.card ? 72 : 24 + 84 + 44;
}

void MusicScreen::clampPane(MusicPane& pane)
{
    const int count = static_cast<int>(pane.rows.size());
    MusicFrame& f = pane.frame;
    if (count == 0) {
        f.selected = f.scroll = 0;
        return;
    }
    f.selected = std::max(0, std::min(f.selected, count - 1));
    if (f.kind == MusicPaneKind::Home) { // scroll counts rails here, not rows
        while (f.selected < count - 1 && !pane.rows[f.selected].selectable())
            ++f.selected;
        const auto segs = homeSegments(pane);
        int sel = 0;
        for (int i = 0; i < static_cast<int>(segs.size()); ++i)
            if (f.selected >= segs[i].first && f.selected < segs[i].first + segs[i].count)
                sel = i;
        f.scroll = std::max(0, std::min(f.scroll, sel));
        const int avail = contentBottom() - 76;
        for (;;) { // scroll down until the selected rail fits
            int used = 0;
            for (int i = f.scroll; i <= sel && i < static_cast<int>(segs.size()); ++i)
                used += homeSegmentHeight(segs[i]);
            if (used <= avail || f.scroll >= sel)
                break;
            ++f.scroll;
        }
        return;
    }
    // Land on a selectable row.
    while (f.selected < count - 1 && !pane.rows[f.selected].selectable())
        ++f.selected;
    while (f.selected > 0 && !pane.rows[f.selected].selectable())
        --f.selected;
    const int visible = visibleRows(pane);
    if (isGrid(pane)) { // scroll moves a whole row of covers at a time
        const int firstRow = f.scroll / kGridCols, row = f.selected / kGridCols,
                  rowsShown = visible / kGridCols;
        const int newFirst =
            row < firstRow ? row : (row >= firstRow + rowsShown ? row - rowsShown + 1 : firstRow);
        f.scroll = newFirst * kGridCols;
        return;
    }
    if (f.selected < f.scroll)
        f.scroll = f.selected;
    if (f.selected >= f.scroll + visible)
        f.scroll = f.selected - visible + 1;
    // Show a heading above the first row when the cursor is at the top.
    if (f.selected > 0 && f.scroll == f.selected &&
        pane.rows[f.selected - 1].kind == MusicRow::Kind::Heading)
        f.scroll = f.selected - 1;
    f.scroll = std::max(0, std::min(f.scroll, std::max(0, count - 1)));
}

void MusicScreen::restoreSelection(MusicPane& pane, const std::string& selectedId)
{
    if (!selectedId.empty()) {
        for (std::size_t i = 0; i < pane.rows.size(); ++i)
            if (pane.rows[i].selectable() && pane.rows[i].id == selectedId) {
                pane.frame.selected = static_cast<int>(i);
                break;
            }
    }
    clampPane(pane);
}

void MusicScreen::rebuildRows(MusicPane& pane)
{
    // Only Home is assembled from several listings.
    if (pane.frame.kind != MusicPaneKind::Home)
        return;
    const std::string selectedId = pane.frame.selectedId;
    pane.rows.clear();
    if (!pane.homeAlbums.empty()) {
        pane.rows.push_back(headingRow("Recently added"));
        for (const music::Album& a : pane.homeAlbums)
            pane.rows.push_back(albumRow(a));
    }
    if (!pane.homeTracks.empty()) {
        pane.rows.push_back(headingRow("Recently played"));
        for (const music::Track& t : pane.homeTracks)
            pane.rows.push_back(trackRow(t, false));
    }
    restoreSelection(pane, selectedId);
}

void MusicScreen::applyListResult(const music::ListResult& r)
{
    MusicPane* pane = nullptr;
    bool second = false;
    for (TabRuntime& tab : m_tabs) {
        auto check = [&](MusicPane& p) {
            if (p.ticket == r.ticket) {
                pane = &p;
                second = false;
            } else if (p.ticket2 == r.ticket && r.ticket != 0) {
                pane = &p;
                second = true;
            }
        };
        for (MusicPane& p : tab.roots)
            check(p);
        for (MusicPane& p : tab.drill)
            check(p);
    }
    if (!pane)
        return;
    if (r.final) {
        (second ? pane->ticket2 : pane->ticket) = 0;
    }
    pane->loadedOnce = pane->loadedOnce || r.ok || r.fromCache;
    if (r.final && !r.ok) {
        pane->failed = pane->rows.empty() && !pane->loadedOnce;
        pane->error = r.error;
        if (!pane->rows.empty() || pane->loadedOnce)
            return; // keep what we have on screen
        return;
    }
    if (pane->frame.kind == MusicPaneKind::Home) {
        if (second)
            pane->homeTracks = r.tracks.items;
        else
            pane->homeAlbums = r.albums.items;
        rebuildRows(*pane);
        return;
    }

    const std::string selectedId = pane->frame.selectedId;
    const bool first = r.request.start == 0;
    if (first)
        pane->rows.clear();
    const bool numbered = pane->frame.kind == MusicPaneKind::AlbumTracks;
    int pageTotal = 0;
    std::size_t loaded = 0;
    switch (r.request.kind) {
    case music::Listing::Artists:
        for (const music::Artist& a : r.artists.items)
            pane->rows.push_back(artistRow(a));
        pageTotal = r.artists.total;
        loaded = r.artists.items.size();
        break;
    case music::Listing::Albums:
    case music::Listing::RecentAlbums:
    case music::Listing::ArtistAlbums:
        for (const music::Album& a : r.albums.items)
            pane->rows.push_back(albumRow(a));
        pageTotal = r.albums.total;
        loaded = r.albums.items.size();
        break;
    case music::Listing::Playlists:
        for (const music::Playlist& p : r.playlists.items)
            pane->rows.push_back(playlistRow(p));
        pageTotal = r.playlists.total;
        loaded = r.playlists.items.size();
        break;
    default:
        for (const music::Track& t : r.tracks.items)
            pane->rows.push_back(trackRow(t, numbered));
        pageTotal = r.tracks.total;
        loaded = r.tracks.items.size();
        break;
    }
    pane->total = pageTotal;
    pane->hasMore = !r.fromCache && static_cast<int>(pane->rows.size()) < pageTotal && loaded > 0;
    if (r.fromCache)
        pane->hasMore = false; // the network page decides
    restoreSelection(*pane, first ? selectedId : std::string());
    if (first && pane->frame.selectedId.empty())
        pane->frame.selected = std::min(pane->frame.selected, static_cast<int>(pane->rows.size()));
    clampPane(*pane);
}

// "Continue listening": the first row of Home while a saved queue is waiting.
void MusicScreen::syncResumeRow(MusicPane& pane)
{
    const music::PlayerView& v = m_playerView;
    const bool want = v.resumable;
    const bool have = !pane.rows.empty() && pane.rows.front().id == kResumeId;
    if (!want) {
        if (have) {
            pane.rows.erase(pane.rows.begin());
            pane.frame.selected = std::max(0, pane.frame.selected - 1);
            clampPane(pane);
        }
        return;
    }
    MusicRow row;
    row.kind = MusicRow::Kind::Action;
    row.id = kResumeId;
    row.title = "Continue listening";
    row.subtitle = v.track.title + " - " + v.track.artist;
    row.right = music::formatDuration(static_cast<int>(v.position));
    row.artId = v.track.artId();
    row.artTag = v.track.artTag();
    if (have) {
        pane.rows.front() = row;
    } else {
        pane.rows.insert(pane.rows.begin(), row);
        // A cursor the user has not moved starts on "Continue listening"; one they have moved
        // stays on the same item (it shifted down by one).
        if (pane.frame.selectedId.empty())
            pane.frame.selected = 0;
        else
            pane.frame.selected += 1;
        clampPane(pane);
    }
}

// ---- covers
// ---------------------------------------------------------------------------------------

SDL_Surface* MusicScreen::cover(const std::string& id, const std::string& tag, int size,
                                bool request)
{
    if (id.empty())
        return nullptr;
    const std::string key = music::MusicLibrary::coverKey(id, tag, size);
    auto it = m_covers.find(key);
    if (it != m_covers.end()) {
        // Most recently used goes to the back.
        auto pos = std::find(m_coverOrder.begin(), m_coverOrder.end(), key);
        if (pos != m_coverOrder.end() && pos + 1 != m_coverOrder.end()) {
            m_coverOrder.erase(pos);
            m_coverOrder.push_back(key);
        }
        return it->second;
    }
    if (request && !m_coverRequested.count(key) && !m_coverMissing.count(key)) {
        m_coverRequested.insert(key);
        m_library->requestCover(id, tag, size);
    }
    return nullptr;
}

void MusicScreen::takeCovers()
{
    for (music::CoverResult& r : m_library->takeCovers()) {
        m_coverRequested.erase(r.key);
        if (r.dropped)
            continue; // never fetched: it is asked for again while it is on screen
        if (!r.ok) {
            m_coverMissing.insert(r.key);
            continue;
        }
        SDL_Surface* surface =
            SDL_CreateRGBSurfaceWithFormatFrom(r.image.pixels.data(), r.image.width, r.image.height,
                                               32, r.image.width * 4, SDL_PIXELFORMAT_RGBA32);
        if (!surface)
            continue;
        SDL_Surface* copy = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(surface);
        if (!copy)
            continue;
        auto old = m_covers.find(r.key);
        if (old != m_covers.end()) {
            SDL_FreeSurface(old->second);
            old->second = copy;
        } else {
            m_covers[r.key] = copy;
            m_coverOrder.push_back(r.key);
        }
    }
    trimCovers();
}

void MusicScreen::trimCovers()
{
    while (m_covers.size() > kMaxCovers && !m_coverOrder.empty()) {
        const std::string key = m_coverOrder.front();
        m_coverOrder.pop_front();
        auto it = m_covers.find(key);
        if (it != m_covers.end()) {
            SDL_FreeSurface(it->second);
            m_covers.erase(it);
        }
    }
}

void MusicScreen::requestVisibleCovers()
{
    if (m_view != View::Browse)
        return;
    const MusicPane& pane = activePane();
    if (pane.frame.kind == MusicPaneKind::Home) {
        for (const MusicRow& row : pane.rows) // a short list: all of its covers
            cover(row.artId, row.artTag, 128, true);
        return;
    }
    const int visible = visibleRows(pane);
    for (int i = pane.frame.scroll;
         i < pane.frame.scroll + visible + 1 && i < static_cast<int>(pane.rows.size()); ++i)
        cover(pane.rows[i].artId, pane.rows[i].artTag, 128, true);
    if (detailHeaderHeight(pane) > 0)
        cover(pane.frame.artId, pane.frame.artTag, 128, true);
}

} // namespace miyoofin
