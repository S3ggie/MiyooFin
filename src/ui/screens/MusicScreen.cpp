#include "MusicScreen.hpp"
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

namespace {

constexpr const char* kSettingsFile = "music-settings.txt";
constexpr int kRowHeight = 44;
constexpr int kGridCols = 4, kGridCellH = 156;
constexpr int kPrefetchRows = 12;
constexpr Uint32 kStateSaveMs = 2000;
constexpr std::size_t kMaxCovers = 80;

constexpr int kMenuPlay = 1, kMenuShuffle = 2, kMenuPlayNext = 3, kMenuAppend = 4, kMenuGoAlbum = 5,
              kMenuGoArtist = 6, kMenuDownload = 7, kMenuRemove = 8, kMenuRetry = 9,
              kMenuDownloadPage = 10, kMenuPlayDownloaded = 11, kMenuShuffleDownloaded = 12,
              kMenuAddToPlaylist = 13, kMenuRemoveFromPlaylist = 14, kMenuDeletePlaylist = 15,
              kMenuSync = 16, kMenuFavorite = 17;
constexpr const char* kDownloadPrefix = "dl:";
constexpr const char* kNewPlaylistId = "__newplaylist__";

std::string readText(const std::string& path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeText(const std::string& path, const std::string& text)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << text;
        if (!out.good())
            return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string minutes(std::int64_t ticks)
{
    const int m = static_cast<int>(ticks / 10000000 / 60);
    return std::to_string(m) + " min";
}

MusicRow headingRow(const std::string& text)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Heading;
    r.title = text;
    return r;
}

MusicRow trackRow(const music::Track& t, bool numbered)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Track;
    r.id = t.id;
    r.track = t;
    r.title = (numbered && t.trackNumber > 0 ? std::to_string(t.trackNumber) + ". " : "") + t.title;
    // On an album's page the artist is only worth a line when it differs from the album's.
    r.subtitle = numbered && t.artist == t.albumArtist ? std::string() : t.artist;
    if (!t.album.empty() && !numbered)
        r.subtitle += (r.subtitle.empty() ? "" : " - ") + t.album;
    r.right = music::formatDuration(t.durationSeconds());
    r.artId = t.artId();
    r.artTag = t.artTag();
    return r;
}

MusicRow albumRow(const music::Album& a)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Album;
    r.id = a.id;
    r.album = a;
    r.title = a.title;
    r.subtitle = a.artist;
    if (a.year > 0)
        r.right = std::to_string(a.year);
    r.artId = a.id;
    r.artTag = a.imageTag;
    return r;
}

MusicRow artistRow(const music::Artist& a)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Artist;
    r.id = a.id;
    r.artist = a;
    r.title = a.name;
    r.artId = a.id;
    r.artTag = a.imageTag;
    return r;
}

MusicRow playlistRow(const music::Playlist& p)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Playlist;
    r.id = p.id;
    r.playlist = p;
    r.title = p.title;
    r.subtitle = std::to_string(p.trackCount) + (p.trackCount == 1 ? " track" : " tracks");
    r.right = p.runTimeTicks > 0 ? minutes(p.runTimeTicks) : "";
    r.artId = p.id;
    r.artTag = p.imageTag;
    return r;
}

bool isAlphabetical(MusicPaneKind k)
{
    return k == MusicPaneKind::Artists || k == MusicPaneKind::Albums || k == MusicPaneKind::Songs;
}

} // namespace

std::vector<std::string> MusicScreen::tabNames()
{
    return {"Home", "Library", "Playlists", "Downloads", "Settings"};
}

music::ListingRequest MusicScreen::requestFor(const MusicFrame& frame, int start)
{
    music::ListingRequest r;
    r.start = start;
    r.letter = frame.letter;
    switch (frame.kind) {
    case MusicPaneKind::Artists:
        r.kind = music::Listing::Artists;
        r.limit = 60;
        break;
    case MusicPaneKind::Albums:
        r.kind = music::Listing::Albums;
        r.limit = 60;
        break;
    case MusicPaneKind::Songs:
        r.kind = music::Listing::Songs;
        r.limit = 60;
        break;
    case MusicPaneKind::Playlists:
        r.kind = music::Listing::Playlists;
        r.limit = 100;
        break;
    case MusicPaneKind::ArtistAlbums:
        r.kind = music::Listing::ArtistAlbums;
        r.parentId = frame.id;
        r.limit = 100;
        break;
    case MusicPaneKind::AlbumTracks:
        r.kind = music::Listing::AlbumTracks;
        r.parentId = frame.id;
        r.limit = 300;
        break;
    case MusicPaneKind::PlaylistTracks:
        r.kind = music::Listing::PlaylistTracks;
        r.parentId = frame.id;
        r.limit = 100;
        break;
    default:
        break;
    }
    return r;
}

// ---- construction
// ---------------------------------------------------------------------------------

MusicScreen::MusicScreen(const Session& session, music::MusicPlayer* player,
                         music::MusicSettings* settings, music::MusicDownloads* downloads)
    : m_session(session), m_paths(music::MusicPaths::forSession(session)), m_player(player),
      m_settings(settings), m_downloads(downloads),
      m_library(std::make_unique<music::MusicLibrary>(session, m_paths.cache, m_paths.stream))
{
    m_paths.ensureDirs();
    MusicUiState state = MusicUiState::defaults();
    MusicUiState saved;
    if (MusicUiState::parse(readText(m_paths.uiState), saved))
        state = saved;
    buildFromState(state);
}

MusicScreen::~MusicScreen()
{
    for (auto& entry : m_covers)
        SDL_FreeSurface(entry.second);
    for (auto& entry : m_scaledCovers)
        SDL_FreeSurface(entry.second.scaled);
}

void MusicScreen::enter() {}

void MusicScreen::leave()
{
    saveState();
    m_library->cancelLists();
}

MusicPane MusicScreen::makePane(const MusicFrame& frame) const
{
    MusicPane p;
    p.frame = frame;
    return p;
}

void MusicScreen::buildFromState(const MusicUiState& state)
{
    m_tabs.assign(kMusicTabCount, TabRuntime{});
    for (int t = 0; t < kMusicTabCount; ++t) {
        for (const MusicFrame& f : state.tabs[t].roots)
            m_tabs[t].roots.push_back(makePane(f));
        m_tabs[t].section = state.tabs[t].section;
        for (const MusicFrame& f : state.tabs[t].drill)
            m_tabs[t].drill.push_back(makePane(f));
    }
    m_activeTab = state.activeTab;
}

MusicUiState MusicScreen::snapshotState() const
{
    MusicUiState s = MusicUiState::defaults();
    s.activeTab = m_activeTab;
    for (int t = 0; t < kMusicTabCount; ++t) {
        s.tabs[t].roots.clear();
        for (const MusicPane& p : m_tabs[t].roots)
            s.tabs[t].roots.push_back(p.frame);
        s.tabs[t].section = m_tabs[t].section;
        for (const MusicPane& p : m_tabs[t].drill)
            s.tabs[t].drill.push_back(p.frame);
    }
    return s;
}

void MusicScreen::saveState()
{
    writeText(m_paths.uiState, snapshotState().serialize());
    m_stateDirty = false;
    m_stateSavedAt = m_clock;
}

// ---- panes
// ----------------------------------------------------------------------------------------

std::vector<MusicPane>& MusicScreen::rootsOf(int tab)
{
    return m_tabs[tab].roots;
}

MusicPane& MusicScreen::activePane()
{
    TabRuntime& tab = m_tabs[m_activeTab];
    if (!tab.drill.empty())
        return tab.drill.back();
    return tab.roots[std::min<int>(tab.section, static_cast<int>(tab.roots.size()) - 1)];
}

const MusicPane& MusicScreen::activePane() const
{
    return const_cast<MusicScreen*>(this)->activePane();
}

void MusicScreen::setTab(int tab)
{
    m_activeTab = (tab + kMusicTabCount) % kMusicTabCount;
    m_menu.open = false;
    markDirty();
}

void MusicScreen::openFrame(const MusicFrame& frame)
{
    m_tabs[m_activeTab].drill.push_back(makePane(frame));
    markDirty();
}

void MusicScreen::popFrame()
{
    TabRuntime& tab = m_tabs[m_activeTab];
    if (tab.drill.empty())
        return;
    tab.drill.pop_back();
    markDirty();
}

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

// ---- frame update
// ---------------------------------------------------------------------------------

void MusicScreen::update(Uint32 dt)
{
    m_clock += dt;
    ++m_frame;
    if (m_player)
        m_playerView = m_player->view();
    m_battery.update(dt);

    if (!m_syncStarted) { // once per visit: playlists marked "keep in sync" catch up
        m_syncStarted = true;
        syncDownloadedPlaylists();
    }
    for (const music::ListResult& r : m_library->takeLists()) {
        auto pending = m_pending.find(r.ticket);
        if (pending != m_pending.end()) {
            applyPending(r, Pending(pending->second)); // a copy: the entry is erased inside
            continue;
        }
        applyListResult(r);
    }
    takeCovers();
    applyJobResults();

    MusicPane& pane = activePane();
    if (pane.frame.kind == MusicPaneKind::Playlists)
        syncNewPlaylistRow(pane);
    if (pane.frame.kind == MusicPaneKind::Settings)
        refreshSettingsRows(pane);
    if (pane.frame.kind == MusicPaneKind::Home)
        syncResumeRow(pane);
    if (pane.frame.kind == MusicPaneKind::Downloads ||
        pane.frame.kind == MusicPaneKind::DownloadedTracks)
        refreshDownloadRows(pane);
    if (!pane.requested)
        requestPane(pane, false);
    // The next page loads before the cursor reaches the end of what we have.
    if (pane.hasMore && pane.ticket == 0 && !pane.failed &&
        pane.frame.selected + kPrefetchRows >= static_cast<int>(pane.rows.size()))
        requestPane(pane, true);
    requestVisibleCovers();
    if (m_lyricsView && m_view == View::NowPlaying)
        requestLyrics(); // a new track while the lyrics are open

    if (m_stateDirty && m_clock - m_stateSavedAt >= kStateSaveMs)
        saveState();
    if (!m_toast.empty() && m_clock > m_toastUntil)
        m_toast.clear();
}

constexpr const char* kResumeId = "__resume__";

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

std::string formatMegabytes(std::uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024 * 1024));
    else
        std::snprintf(buf, sizeof(buf), "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024));
    return buf;
}

// The Downloads tab (one row per downloaded album/playlist/track) and the page of a downloaded
// collection. Both come straight from the local index, so they work with no network at all.
void MusicScreen::refreshDownloadRows(MusicPane& pane)
{
    pane.requested = true;
    pane.loadedOnce = true;
    if (!m_downloads)
        return;
    // Progress moves quickly while downloading, so rebuild on every change (cheap: no I/O).
    const std::uint64_t revision = m_downloads->revision();
    if (revision == m_downloadsRevision && pane.builtRevision == revision)
        return;
    m_downloadsRevision = revision;
    pane.builtRevision = revision;
    const std::string selectedId = pane.frame.selectedId;
    pane.rows.clear();
    if (pane.frame.kind == MusicPaneKind::DownloadedTracks) {
        for (const music::Track& t : m_downloads->tracksOf(pane.frame.id))
            pane.rows.push_back(trackRow(t, false));
        pane.total = static_cast<int>(pane.rows.size());
    } else {
        for (const music::DownloadStatus& s : m_downloads->snapshot()) {
            MusicRow r;
            r.kind = MusicRow::Kind::Action;
            r.id = kDownloadPrefix + s.collection.id;
            r.title = s.collection.name;
            std::string info = s.collection.artist.empty() ? "" : s.collection.artist + " - ";
            info += std::to_string(s.done) + "/" + std::to_string(s.total) +
                    (s.total == 1 ? " track" : " tracks");
            if (s.failed)
                info = "Failed: " + s.error;
            else if (s.done < s.total)
                info = (s.active ? "Downloading " : "Waiting ") + info;
            if (s.collection.sync && !s.failed)
                info += " - keeps in sync";
            r.subtitle = info;
            r.right = formatMegabytes(s.bytes);
            r.artId = s.collection.artId;
            r.artTag = s.collection.artTag;
            if (s.done < s.total && !s.failed)
                r.progress = s.total ? s.done * 100 / s.total : 0;
            pane.rows.push_back(std::move(r));
        }
        pane.total = static_cast<int>(pane.rows.size());
    }
    restoreSelection(pane, selectedId);
}

bool MusicScreen::isDownloaded(const MusicRow& row) const
{
    if (!m_downloads)
        return false;
    switch (row.kind) {
    case MusicRow::Kind::Track:
        return m_downloads->hasTrack(row.track.id);
    case MusicRow::Kind::Album:
        return m_downloads->hasCollection(row.album.id);
    case MusicRow::Kind::Playlist:
        return m_downloads->hasCollection(row.playlist.id);
    default:
        return false;
    }
}

music::DownloadCollection MusicScreen::collectionFor(const MusicRow& row) const
{
    music::DownloadCollection c;
    c.id = row.id;
    c.name = row.title;
    c.artist = row.kind == MusicRow::Kind::Album ? row.album.artist : "";
    c.artId = row.artId;
    c.artTag = row.artTag;
    c.kind = row.kind == MusicRow::Kind::Album      ? "album"
             : row.kind == MusicRow::Kind::Playlist ? "playlist"
                                                    : "track";
    if (row.kind == MusicRow::Kind::Track) {
        c.id = "track-" + row.track.id;
        c.name = row.track.title;
        c.artist = row.track.artist;
        c.artId = row.track.artId();
        c.artTag = row.track.artTag();
    }
    return c;
}

music::DownloadCollection MusicScreen::collectionForPane(const MusicPane& pane) const
{
    music::DownloadCollection c;
    c.id = pane.frame.id;
    c.name = pane.frame.title;
    c.artist = pane.frame.subtitle;
    c.artId = pane.frame.artId;
    c.artTag = pane.frame.artTag;
    c.kind = pane.frame.kind == MusicPaneKind::PlaylistTracks ? "playlist" : "album";
    return c;
}

void MusicScreen::downloadTracks(const music::DownloadCollection& collection,
                                 const std::vector<music::Track>& tracks)
{
    if (!m_downloads || tracks.empty())
        return;
    m_downloads->enqueue(collection, tracks);
    m_toast = "Downloading " + collection.name;
    m_toastUntil = m_clock + 2200;
}

// "+ New playlist" is always the first row of the Playlists tab.
void MusicScreen::syncNewPlaylistRow(MusicPane& pane)
{
    const bool have = !pane.rows.empty() && pane.rows.front().id == kNewPlaylistId;
    // "Favorite songs" is always the second row, right after "+ New playlist".
    const bool haveFavorites = pane.rows.size() > 1 && pane.rows[1].id == music::kFavoritesId;
    if (have && !haveFavorites) {
        MusicRow fav;
        fav.kind = MusicRow::Kind::Playlist;
        fav.id = music::kFavoritesId;
        fav.title = "Favorite songs";
        fav.subtitle = "Songs you have hearted";
        fav.playlist.id = music::kFavoritesId;
        fav.playlist.title = fav.title;
        pane.rows.insert(pane.rows.begin() + 1, fav);
        if (pane.frame.selected >= 1)
            ++pane.frame.selected;
        clampPane(pane);
    }
    if (have)
        return;
    MusicRow row;
    row.kind = MusicRow::Kind::Action;
    row.id = kNewPlaylistId;
    row.title = "+ New playlist";
    row.subtitle = "Create an empty playlist";
    const bool hadRows = !pane.rows.empty();
    pane.rows.insert(pane.rows.begin(), row);
    pane.frame.selected = hadRows ? pane.frame.selected + 1 : 0;
    clampPane(pane);
}

void MusicScreen::syncDownloadedPlaylists()
{
    if (!m_downloads || m_session.manualOfflineMode)
        return;
    for (const music::DownloadStatus& s : m_downloads->snapshot()) {
        if (!s.collection.sync || s.collection.kind != "playlist")
            continue;
        music::ListingRequest r;
        r.kind = music::Listing::PlaylistTracks;
        r.parentId = s.collection.id;
        r.limit = 500;
        m_pending[m_library->requestList(r)] = Pending{PendingKind::SyncDownload, 0, s.collection};
    }
}

void MusicScreen::startPlaylistPicker(std::vector<music::Track> tracks)
{
    if (tracks.empty()) {
        m_toast = "Nothing to add";
        m_toastUntil = m_clock + 2000;
        return;
    }
    m_pickTracks = std::move(tracks);
    music::ListingRequest r;
    r.kind = music::Listing::Playlists;
    r.limit = 200;
    m_pending[m_library->requestList(r)] = Pending{PendingKind::PickPlaylist, 0, {}};
    m_toast = "Loading playlists...";
    m_toastUntil = m_clock + 1500;
}

void MusicScreen::openPickerFor(const std::vector<music::Playlist>& playlists)
{
    m_pickPlaylists = playlists;
    std::vector<std::string> items = {"+ New playlist..."};
    for (const music::Playlist& p : playlists)
        items.push_back(p.title);
    const std::size_t n = m_pickTracks.size();
    m_picker.open(n == 1 ? "Add \"" + m_pickTracks[0].title + "\" to..."
                         : "Add " + std::to_string(n) + " tracks to...",
                  std::move(items));
}

void MusicScreen::chosePlaylist(int index)
{
    std::vector<music::Track> tracks = std::move(m_pickTracks);
    m_pickTracks.clear();
    if (index <= 0) {
        askPlaylistName("New playlist", "", std::move(tracks));
        return;
    }
    if (index - 1 >= static_cast<int>(m_pickPlaylists.size()))
        return;
    const music::Playlist playlist = m_pickPlaylists[index - 1];
    std::vector<std::string> ids;
    for (const music::Track& t : tracks)
        ids.push_back(t.id);
    m_library->runJob(
        "add|" + playlist.id + "|" + playlist.title,
        [ids, id = playlist.id](const music::Connection& c, std::string&, std::string& error) {
            return music::addToPlaylist(c, id, ids, error);
        });
    m_toast = "Adding...";
    m_toastUntil = m_clock + 1500;
}

void MusicScreen::askPlaylistName(const std::string& title, const std::string& initial,
                                  std::vector<music::Track> tracks)
{
    if (!m_stack)
        return;
    m_stack->push(
        std::make_unique<TextEntryScreen>(title, initial, [this, tracks](const std::string& name) {
            createPlaylistNamed(name, tracks);
        }));
}

void MusicScreen::createPlaylistNamed(const std::string& name, std::vector<music::Track> tracks)
{
    std::vector<std::string> ids;
    for (const music::Track& t : tracks)
        ids.push_back(t.id);
    m_library->runJob("create||" + name,
                      [name, ids](const music::Connection& c, std::string& id, std::string& error) {
                          return music::createPlaylist(c, name, ids, id, error);
                      });
    m_toast = "Creating \"" + name + "\"...";
    m_toastUntil = m_clock + 1500;
}

void MusicScreen::deletePlaylistById(const std::string& id, const std::string& name)
{
    if (m_removeArmed != id) { // deleting cannot be undone: ask twice
        m_removeArmed = id;
        m_toast = "Choose Delete playlist again to delete " + name;
        m_toastUntil = m_clock + 3000;
        return;
    }
    m_removeArmed.clear();
    m_library->runJob("delete|" + id + "|" + name,
                      [id](const music::Connection& c, std::string&, std::string& error) {
                          return music::deletePlaylist(c, id, error);
                      });
    m_toast = "Deleting...";
    m_toastUntil = m_clock + 1500;
}

void MusicScreen::removeFromPlaylist(const music::Track& track)
{
    const MusicPane& pane = activePane();
    if (pane.frame.kind != MusicPaneKind::PlaylistTracks || track.entryId.empty())
        return;
    const std::string playlistId = pane.frame.id, entry = track.entryId;
    m_library->runJob(
        "remove|" + playlistId + "|" + track.title,
        [playlistId, entry](const music::Connection& c, std::string&, std::string& error) {
            return music::removeFromPlaylist(c, playlistId, {entry}, error);
        });
    m_toast = "Removing...";
    m_toastUntil = m_clock + 1500;
}

void MusicScreen::refreshPlaylists()
{
    music::ListingRequest r;
    r.kind = music::Listing::Playlists;
    r.limit = 100;
    m_library->eraseListCache(r);
    for (MusicPane& p : m_tabs[static_cast<int>(MusicTab::Playlists)].roots) {
        p.requested = false; // loaded again the next time it is shown
        p.ticket = 0;
    }
}

bool MusicScreen::isFavorite(const music::Track& track) const
{
    const auto it = m_favOverride.find(track.id);
    return it != m_favOverride.end() ? it->second : track.favorite;
}

void MusicScreen::toggleFavorite(const music::Track& track)
{
    const bool on = !isFavorite(track);
    const std::string id = track.id;
    m_library->runJob("fav|" + id + "|" + (on ? "1" : "0"),
                      [id, on](const music::Connection& c, std::string&, std::string& error) {
                          return music::setFavorite(c, id, on, error);
                      });
    m_toast = on ? "Adding to favorites..." : "Removing from favorites...";
    m_toastUntil = m_clock + 1500;
}

void MusicScreen::requestLyrics()
{
    const std::string id = m_playerView.track.id;
    if (id.empty() || id == m_lyricsFor)
        return;
    m_lyricsFor = id;
    m_lyrics.clear();
    m_lyricsScroll = 0;
    m_lyricsLoading = true;
    m_library->runJob("lyrics|" + id + "|",
                      [id](const music::Connection& c, std::string& packed, std::string& error) {
                          return music::fetchLyrics(c, id, packed, error);
                      });
}

void MusicScreen::applyJobResults()
{
    for (const music::JobResult& r : m_library->takeJobs()) {
        if (r.label.compare(0, 4, "fav|") == 0) {
            const std::size_t bar = r.label.find('|', 4);
            const std::string id = r.label.substr(4, bar - 4);
            const bool on = r.label.compare(bar + 1, 1, "1") == 0;
            if (r.ok) {
                m_favOverride[id] = on;
                music::ListingRequest favorites;
                favorites.kind = music::Listing::PlaylistTracks;
                favorites.parentId = music::kFavoritesId;
                m_library->eraseListCache(favorites);
                MusicPane& pane = activePane();
                if (pane.frame.kind == MusicPaneKind::PlaylistTracks &&
                    pane.frame.id == music::kFavoritesId) {
                    pane.requested = false; // show the changed list
                    pane.ticket = 0;
                }
                m_toast = on ? "Added to favorites" : "Removed from favorites";
            } else {
                m_toast = "Couldn't change the favorite: " + r.error;
            }
            m_toastUntil = m_clock + 2500;
            continue;
        }
        if (r.label.compare(0, 7, "lyrics|") == 0) {
            if (r.label.substr(7, r.label.find('|', 7) - 7) == m_lyricsFor) {
                m_lyricsLoading = false;
                m_lyrics = music::unpackLyrics(r.id);
            }
            continue;
        }
        const std::size_t first = r.label.find('|');
        const std::size_t second = r.label.find('|', first + 1);
        const std::string kind = r.label.substr(0, first);
        const std::string id = r.label.substr(first + 1, second - first - 1);
        const std::string name = r.label.substr(second + 1);
        if (!r.ok) {
            m_toast = "Couldn't " +
                      std::string(kind == "add"      ? "add"
                                  : kind == "create" ? "create the playlist"
                                  : kind == "remove" ? "remove"
                                                     : "delete the playlist") +
                      ": " + r.error;
            m_toastUntil = m_clock + 3500;
            continue;
        }
        music::ListingRequest tracksOfPlaylist;
        tracksOfPlaylist.kind = music::Listing::PlaylistTracks;
        tracksOfPlaylist.parentId = id;
        if (kind == "create") {
            m_toast = "Created \"" + name + "\"";
            refreshPlaylists();
        } else if (kind == "delete") {
            m_toast = "Deleted \"" + name + "\"";
            refreshPlaylists();
        } else {
            m_toast = kind == "add" ? "Added to \"" + name + "\"" : "Removed \"" + name + "\"";
            m_library->eraseListCache(tracksOfPlaylist);
            MusicPane& pane = activePane();
            if (pane.frame.kind == MusicPaneKind::PlaylistTracks && pane.frame.id == id) {
                pane.requested = false; // show the changed playlist
                pane.ticket = 0;
            }
        }
        m_toastUntil = m_clock + 2500;
    }
}

void MusicScreen::refreshSettingsRows(MusicPane& pane)
{
    auto action = [](const std::string& title, const std::string& subtitle,
                     const std::string& right) {
        MusicRow r;
        r.kind = MusicRow::Kind::Action;
        r.id = title;
        r.title = title;
        r.subtitle = subtitle;
        r.right = right;
        return r;
    };
    const int stream = m_settings ? m_settings->streamKbps.load() : 192;
    const int download = m_settings ? m_settings->downloadKbps.load() : 192;
    pane.rows = {
        action("Enter MiyooFin", "Press A to enter MiyooFin", ""),
        action("Streaming quality", "Used when playing from the server",
               std::to_string(stream) + " kbps"),
        action("Download quality", "Used for offline music", std::to_string(download) + " kbps"),
        action("Album view", "Albums as a list or a grid of covers",
               m_settings && m_settings->albumGrid.load() ? "Grid" : "List"),
        action("Clock format", "12 or 24 hour clock in the header",
               ClockSettings::instance().hour24() ? "24-hour" : "12-hour"),
        action("Time zone", "Where the header clock gets its time zone",
               ClockSettings::instance().zoneSummary()),
        action("Clear music cache", "Streamed tracks and covers (downloads stay)",
               m_cacheClearArmed ? "Press A again" : ""),
        action("Server",
               m_session.routes().lan.empty() ? m_session.routes().pub : m_session.routes().lan,
               ""),
        action("Account",
               m_session.userName + (m_session.manualOfflineMode ? " (offline mode)" : ""), "")};
    pane.requested = true;
    pane.loadedOnce = true;
    clampPane(pane);
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

// ---- input
// ----------------------------------------------------------------------------------------

bool MusicScreen::handleAction(Action action)
{
    if (m_picker.active()) {
        const ChoiceMenu::Result result = m_picker.handle(action);
        if (m_zonePick) {
            if (result != ChoiceMenu::Result::None)
                m_zonePick = false;
            if (result == ChoiceMenu::Result::Chosen) {
                ClockSettings& clock = ClockSettings::instance();
                const int index = m_picker.chosen();
                if (index == 0) {
                    clock.setZoneMode("auto");
                    clock.requestDetect();
                } else if (index == 1) {
                    clock.setZoneMode("device");
                } else if (index - 2 < static_cast<int>(clockZones().size())) {
                    clock.setZoneMode(clockZones()[index - 2].key);
                }
                clock.applyZone();
                clock.save("clock-settings.txt");
            }
            return true;
        }
        if (result == ChoiceMenu::Result::Chosen)
            chosePlaylist(m_picker.chosen());
        return true;
    }
    if (m_menu.open)
        return handleMenu(action);
    if (m_view == View::NowPlaying)
        return handleNowPlaying(action);
    return handleBrowse(action);
}

void MusicScreen::cycleLetter(int delta)
{
    MusicPane& pane = activePane();
    if (!isAlphabetical(pane.frame.kind))
        return;
    // Order: everything, A..Z, #.
    int index =
        pane.frame.letter == 0 ? 0 : (pane.frame.letter == '#' ? 27 : pane.frame.letter - 'A' + 1);
    index = (index + delta + 28) % 28;
    pane.frame.letter = index == 0 ? 0 : (index == 27 ? '#' : static_cast<char>('A' + index - 1));
    pane.frame.selected = pane.frame.scroll = 0;
    pane.frame.selectedId.clear();
    pane.rows.clear();
    pane.hasMore = false;
    pane.loadedOnce = false;
    // Answers for the previous letter find no matching ticket and are simply ignored.
    requestPane(pane, false);
    markDirty();
}

bool MusicScreen::handleHomeNav(Action action)
{
    MusicPane& pane = activePane();
    const auto segs = homeSegments(pane);
    int seg = -1;
    for (int i = 0; i < static_cast<int>(segs.size()); ++i)
        if (pane.frame.selected >= segs[i].first &&
            pane.frame.selected < segs[i].first + segs[i].count)
            seg = i;
    if (seg < 0)
        return true;
    int target = pane.frame.selected;
    const int offset = pane.frame.selected - segs[seg].first;
    if (action == Action::Left || action == Action::Right) {
        const int o = offset + (action == Action::Left ? -1 : 1);
        if (o >= 0 && o < segs[seg].count)
            target = segs[seg].first + o;
    } else {
        const int next = seg + (action == Action::Up ? -1 : 1);
        if (next >= 0 && next < static_cast<int>(segs.size()))
            target = segs[next].first + std::min(offset, segs[next].count - 1);
    }
    if (target != pane.frame.selected) {
        pane.frame.selected = target;
        pane.frame.selectedId = pane.rows[target].id;
        clampPane(pane);
        markDirty();
    }
    return true;
}

bool MusicScreen::handleBrowse(Action action)
{
    MusicPane& pane = activePane();
    const int count = static_cast<int>(pane.rows.size());
    switch (action) {
    case Action::PrevTab:
        setTab(m_activeTab - 1);
        return true;
    case Action::NextTab:
        setTab(m_activeTab + 1);
        return true;
    case Action::Settings: // START
        if (miniPlayerVisible()) {
            m_view = View::NowPlaying;
            m_queueView = false;
        }
        return true;
    case Action::Menu: // SELECT
        if (miniPlayerVisible()) {
            m_view = View::NowPlaying;
            m_queueView = true;
            m_queueSelected = std::max(0, m_player->queue().position());
        }
        return true;
    case Action::Search: // X
        if (m_player)
            m_player->togglePause();
        return true;
    case Action::Up:
    case Action::Down: {
        if (count == 0)
            return true;
        if (pane.frame.kind == MusicPaneKind::Home)
            return handleHomeNav(action);
        int i = pane.frame.selected + (action == Action::Up ? -1 : 1);
        if (isGrid(pane)) { // a row of covers up or down; the last row stops at the last cover
            i = pane.frame.selected + (action == Action::Up ? -kGridCols : kGridCols);
            if (i >= count && pane.frame.selected / kGridCols < (count - 1) / kGridCols)
                i = count - 1;
        }
        while (i >= 0 && i < count && !pane.rows[i].selectable())
            i += action == Action::Up ? -1 : 1;
        if (i >= 0 && i < count) {
            pane.frame.selected = i;
            pane.frame.selectedId = pane.rows[i].id;
            clampPane(pane);
            markDirty();
        }
        return true;
    }
    case Action::Left:
    case Action::Right:
        if (pane.frame.kind == MusicPaneKind::Home)
            return handleHomeNav(action);
        if (isGrid(pane)) {
            const int i = pane.frame.selected + (action == Action::Left ? -1 : 1);
            if (i >= 0 && i < count) {
                pane.frame.selected = i;
                pane.frame.selectedId = pane.rows[i].id;
                clampPane(pane);
                markDirty();
            }
            return true;
        }
        if (action == Action::Right) {
            if (isAlphabetical(pane.frame.kind))
                cycleLetter(1);
            return true;
        }
        if (isAlphabetical(pane.frame.kind))
            cycleLetter(-1);
        return true;
    case Action::PrevPage:
    case Action::NextPage: {
        TabRuntime& tab = m_tabs[m_activeTab];
        const int delta = action == Action::PrevPage ? -1 : 1;
        if (tab.drill.empty() && tab.roots.size() > 1) {
            tab.section = (tab.section + delta + static_cast<int>(tab.roots.size())) %
                          static_cast<int>(tab.roots.size());
            markDirty();
            return true;
        }
        if (count > 0) {
            const int page = visibleRows(pane);
            pane.frame.selected =
                std::max(0, std::min(count - 1, pane.frame.selected + delta * page));
            while (pane.frame.selected < count - 1 && !pane.rows[pane.frame.selected].selectable())
                ++pane.frame.selected;
            pane.frame.selectedId = pane.rows[pane.frame.selected].id;
            clampPane(pane);
            markDirty();
        }
        return true;
    }
    case Action::Confirm:
        if (pane.frame.selected >= 0 && pane.frame.selected < count &&
            pane.rows[pane.frame.selected].id == kResumeId) {
            if (m_player)
                m_player->resumeSaved();
        } else if (pane.frame.kind == MusicPaneKind::Settings) {
            settingsAction(pane.frame.selected);
        } else if (pane.frame.selected >= 0 && pane.frame.selected < count) {
            activateRow(pane.rows[pane.frame.selected]);
        }
        return true;
    case Action::ActionsMenu: // Y
        if (pane.frame.selected >= 0 && pane.frame.selected < count)
            openMenu(pane.rows[pane.frame.selected]);
        return true;
    case Action::Back:
        popFrame();
        return true;
    default:
        return false;
    }
}

void MusicScreen::playFromPane(int rowIndex)
{
    MusicPane& pane = activePane();
    std::vector<music::Track> tracks;
    int start = 0;
    for (int i = 0; i < static_cast<int>(pane.rows.size()); ++i) {
        if (pane.rows[i].kind != MusicRow::Kind::Track)
            continue;
        if (i == rowIndex)
            start = static_cast<int>(tracks.size());
        tracks.push_back(pane.rows[i].track);
    }
    if (tracks.empty() || !m_player)
        return;
    m_player->playTracks(std::move(tracks), start, false);
}

void MusicScreen::playCollection(const MusicRow& row, PendingKind kind)
{
    music::ListingRequest r;
    r.parentId = row.id;
    r.limit = 500;
    if (row.kind == MusicRow::Kind::Album)
        r.kind = music::Listing::AlbumTracks;
    else if (row.kind == MusicRow::Kind::Playlist)
        r.kind = music::Listing::PlaylistTracks;
    else
        return;
    Pending p{kind, 0, collectionFor(row)};
    m_pending[m_library->requestList(r)] = p;
    m_toast = "Loading...";
    m_toastUntil = m_clock + 1500;
}

void MusicScreen::applyPending(const music::ListResult& r, const Pending& pending)
{
    if (pending.kind == PendingKind::PickPlaylist) {
        // The playlist list for the picker: the first answer (cache or server) is used.
        if (r.playlists.items.empty() && !r.final)
            return;
        m_pending.erase(r.ticket);
        if (!r.ok && r.playlists.items.empty() && r.error == "Offline") {
            m_toast = "Playlists need a connection";
            m_toastUntil = m_clock + 2500;
            return;
        }
        openPickerFor(r.playlists.items);
        return;
    }
    const bool syncing = pending.kind == PendingKind::SyncDownload;
    if (syncing && r.fromCache)
        return; // only the server's list says what the playlist holds now
    // A playlist the server successfully reports as empty is empty: the copy follows it. That is
    // not the same as a failed or partial answer (handled below), which must change nothing.
    if (syncing && r.ok && r.final && !r.fromCache && pending.loaded.empty() &&
        r.tracks.items.empty() && r.tracks.total == 0 && r.request.start == 0) {
        m_pending.erase(r.ticket);
        if (m_downloads)
            m_downloads->enqueue(pending.collection, {});
        return;
    }
    // Later pages of a long list (the server answers one page at a time) only count from the
    // network; the first page may come from the cache when that holds the whole list.
    if (r.request.start > 0 && r.fromCache)
        return;
    std::vector<music::Track> all = pending.loaded;
    all.insert(all.end(), r.tracks.items.begin(), r.tracks.items.end());
    if (!r.fromCache && !r.tracks.items.empty() && static_cast<int>(all.size()) < r.tracks.total) {
        Pending next = pending;
        next.expected = r.tracks.total;
        next.loaded = std::move(all);
        music::ListingRequest more = r.request;
        more.start = static_cast<int>(next.loaded.size());
        m_pending.erase(r.ticket);
        m_toast = "Loading " + std::to_string(next.loaded.size()) + " of " +
                  std::to_string(r.tracks.total) + "...";
        m_toastUntil = m_clock + 2000;
        m_pending[m_library->requestList(more)] = std::move(next);
        return;
    }
    // Act only on the whole list: a cached answer is trimmed to its first rows, and a page that
    // failed halfway leaves a partial list (syncing a partial list would delete downloads).
    const int expected = std::max(pending.expected, r.tracks.total);
    const bool complete = !(r.final && !r.ok) && static_cast<int>(all.size()) >= expected;
    if (syncing && (all.empty() || !complete)) {
        if (r.final)
            m_pending.erase(r.ticket); // offline or empty: the copy stays as it is
        return;
    }
    if (all.empty() || !complete) {
        if (r.final) {
            m_pending.erase(r.ticket);
            m_toast = r.ok ? "Nothing to play" : "Couldn't load: " + r.error;
            m_toastUntil = m_clock + 2500;
        }
        return;
    }
    m_pending.erase(r.ticket); // a cached answer is good enough to act on
    if (syncing) {
        if (m_downloads)
            m_downloads->enqueue(pending.collection, all);
        return;
    }
    if (!m_player && pending.kind != PendingKind::Download &&
        pending.kind != PendingKind::AddToPlaylist)
        return;
    switch (pending.kind) {
    case PendingKind::PickPlaylist:
    case PendingKind::SyncDownload:
        break;
    case PendingKind::AddToPlaylist:
        startPlaylistPicker(all);
        break;
    case PendingKind::PlayAll:
        m_player->playTracks(all, 0, false);
        break;
    case PendingKind::ShuffleAll:
        m_player->playTracks(all, 0, true);
        break;
    case PendingKind::PlayNext: {
        if (!m_player->active()) {
            m_player->playTracks(all, 0, false);
            break;
        }
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            m_player->playNext(*it);
        m_toast = "Playing next";
        m_toastUntil = m_clock + 1500;
        break;
    }
    case PendingKind::Download:
        downloadTracks(pending.collection, all);
        break;
    case PendingKind::Append:
        if (!m_player->active()) {
            m_player->playTracks(all, 0, false);
            break;
        }
        for (const music::Track& t : all)
            m_player->append(t);
        m_toast = "Added to queue";
        m_toastUntil = m_clock + 1500;
        break;
    }
}

void MusicScreen::activateRow(const MusicRow& row)
{
    MusicFrame f;
    f.id = row.id;
    f.title = row.title;
    f.artId = row.artId;
    f.artTag = row.artTag;
    switch (row.kind) {
    case MusicRow::Kind::Track:
        playFromPane(activePane().frame.selected);
        break;
    case MusicRow::Kind::Album:
        f.kind = MusicPaneKind::AlbumTracks;
        f.subtitle = row.album.artist;
        openFrame(f);
        break;
    case MusicRow::Kind::Artist:
        f.kind = MusicPaneKind::ArtistAlbums;
        openFrame(f);
        break;
    case MusicRow::Kind::Playlist:
        f.kind = MusicPaneKind::PlaylistTracks;
        openFrame(f);
        break;
    case MusicRow::Kind::Action:
        if (row.id == kNewPlaylistId) {
            askPlaylistName("New playlist", "", {});
        } else if (row.id.compare(0, std::strlen(kDownloadPrefix), kDownloadPrefix) == 0) {
            f.kind = MusicPaneKind::DownloadedTracks;
            f.id = row.id.substr(std::strlen(kDownloadPrefix));
            openFrame(f);
        }
        break;
    default:
        break;
    }
}

void MusicScreen::openMenu(const MusicRow& row)
{
    m_menu = Menu{};
    m_menu.row = row;
    const MusicPane& pane = activePane();
    const bool inCollection = pane.frame.kind == MusicPaneKind::AlbumTracks ||
                              pane.frame.kind == MusicPaneKind::PlaylistTracks;
    const bool isDownloadRow =
        row.kind == MusicRow::Kind::Action &&
        row.id.compare(0, std::strlen(kDownloadPrefix), kDownloadPrefix) == 0;
    switch (row.kind) {
    case MusicRow::Kind::Track:
        m_menu.items = {
            {"Play next", kMenuPlayNext},
            {"Add to queue", kMenuAppend},
            {"Add to playlist...", kMenuAddToPlaylist},
            {isFavorite(row.track) ? "Remove from favorites" : "Add to favorites", kMenuFavorite}};
        if (m_view == View::Browse && pane.frame.kind == MusicPaneKind::PlaylistTracks &&
            !row.track.entryId.empty())
            m_menu.items.push_back({"Remove from this playlist", kMenuRemoveFromPlaylist});
        if (inCollection && m_view == View::Browse)
            m_menu.items.push_back({pane.frame.kind == MusicPaneKind::PlaylistTracks
                                        ? "Download playlist"
                                        : "Download album",
                                    kMenuDownloadPage});
        if (m_downloads && !m_downloads->hasTrack(row.track.id))
            m_menu.items.push_back({"Download track", kMenuDownload});
        if (!row.track.albumId.empty() && pane.frame.kind != MusicPaneKind::AlbumTracks)
            m_menu.items.push_back({"Go to album", kMenuGoAlbum});
        if (!row.track.artistId.empty())
            m_menu.items.push_back({"Go to artist", kMenuGoArtist});
        break;
    case MusicRow::Kind::Album:
    case MusicRow::Kind::Playlist:
        m_menu.items = {{"Play", kMenuPlay},
                        {"Shuffle", kMenuShuffle},
                        {"Play next", kMenuPlayNext},
                        {"Add to queue", kMenuAppend},
                        {"Add to playlist...", kMenuAddToPlaylist}};
        if (row.kind == MusicRow::Kind::Playlist && row.id != music::kFavoritesId)
            m_menu.items.push_back({"Delete playlist", kMenuDeletePlaylist});
        if (m_downloads) {
            // A copy made when lists were cut at 500 songs (or a playlist that has grown) is
            // topped up from here.
            if (isDownloaded(row) && row.kind == MusicRow::Kind::Playlist) {
                int wanted = 0; // songs the copy already covers, finished or still downloading
                for (const music::DownloadStatus& st : m_downloads->snapshot())
                    if (st.collection.id == row.playlist.id)
                        wanted = st.total;
                if (row.playlist.trackCount > wanted)
                    m_menu.items.push_back({"Download the rest", kMenuDownload});
            }
            if (isDownloaded(row) && row.kind == MusicRow::Kind::Playlist)
                m_menu.items.push_back(
                    {m_downloads->syncOf(row.playlist.id) ? "Stop keeping in sync" : "Keep in sync",
                     kMenuSync});
            m_menu.items.push_back(isDownloaded(row) ? MenuItem{"Remove download", kMenuRemove}
                                                     : MenuItem{"Download", kMenuDownload});
        }
        break;
    case MusicRow::Kind::Action:
        if (!isDownloadRow)
            return;
        m_menu.items = {{"Play", kMenuPlayDownloaded}, {"Shuffle", kMenuShuffleDownloaded}};
        if (m_downloads) {
            for (const music::DownloadStatus& s : m_downloads->snapshot()) {
                if (kDownloadPrefix + s.collection.id != row.id)
                    continue;
                if (s.failed)
                    m_menu.items.push_back({"Retry download", kMenuRetry});
                if (s.collection.kind == "playlist")
                    m_menu.items.push_back(
                        {s.collection.sync ? "Stop keeping in sync" : "Keep in sync", kMenuSync});
            }
        }
        m_menu.items.push_back({"Remove download", kMenuRemove});
        break;
    default:
        return;
    }
    m_menu.open = true;
}

void MusicScreen::runMenuAction(int action, const MusicRow& row)
{
    const bool track = row.kind == MusicRow::Kind::Track;
    switch (action) {
    case kMenuPlay:
        playCollection(row, PendingKind::PlayAll);
        break;
    case kMenuShuffle:
        playCollection(row, PendingKind::ShuffleAll);
        break;
    case kMenuPlayNext:
        if (track && m_player) {
            if (m_player->active()) {
                m_player->playNext(row.track);
                m_toast = "Playing next";
            } else {
                m_player->playTracks({row.track}, 0, false);
            }
            m_toastUntil = m_clock + 1500;
        } else {
            playCollection(row, PendingKind::PlayNext);
        }
        break;
    case kMenuAppend:
        if (track && m_player) {
            if (m_player->active()) {
                m_player->append(row.track);
                m_toast = "Added to queue";
            } else {
                m_player->playTracks({row.track}, 0, false);
            }
            m_toastUntil = m_clock + 1500;
        } else {
            playCollection(row, PendingKind::Append);
        }
        break;
    case kMenuDownload:
        if (track) {
            downloadTracks(collectionFor(row), {row.track});
        } else {
            playCollection(row, PendingKind::Download);
        }
        break;
    case kMenuDownloadPage: {
        const MusicPane& pane = activePane();
        if (pane.hasMore) {
            // Only part of the list is loaded: fetch all of it first.
            MusicRow whole;
            whole.kind = pane.frame.kind == MusicPaneKind::PlaylistTracks ? MusicRow::Kind::Playlist
                                                                          : MusicRow::Kind::Album;
            whole.id = pane.frame.id;
            whole.title = pane.frame.title;
            whole.artId = pane.frame.artId;
            whole.artTag = pane.frame.artTag;
            whole.album.artist = pane.frame.subtitle;
            playCollection(whole, PendingKind::Download);
            break;
        }
        std::vector<music::Track> tracks;
        for (const MusicRow& r : pane.rows)
            if (r.kind == MusicRow::Kind::Track)
                tracks.push_back(r.track);
        downloadTracks(collectionForPane(pane), tracks);
        break;
    }
    case kMenuSync: {
        if (!m_downloads)
            break;
        // A row in the Downloads tab carries the "dl:" prefix on its collection id.
        const std::string collectionId =
            row.id.compare(0, std::strlen(kDownloadPrefix), kDownloadPrefix) == 0
                ? row.id.substr(std::strlen(kDownloadPrefix))
                : row.id;
        const bool on = !m_downloads->syncOf(collectionId);
        m_downloads->setSync(collectionId, on);
        m_toast = on ? "Keeping " + row.title + " in sync" : "No longer syncing " + row.title;
        m_toastUntil = m_clock + 2500;
        if (on)
            syncDownloadedPlaylists();
        break;
    }
    case kMenuRemove: {
        if (!m_downloads)
            break;
        const bool isDlRow = row.id.compare(0, std::strlen(kDownloadPrefix), kDownloadPrefix) == 0;
        const std::string id = isDlRow ? row.id.substr(std::strlen(kDownloadPrefix)) : row.id;
        if (m_removeArmed != id) { // asks twice: removal cannot be undone
            m_removeArmed = id;
            m_toast = "Choose Remove again to delete " + row.title;
            m_toastUntil = m_clock + 3000;
            break;
        }
        m_removeArmed.clear();
        m_downloads->removeCollection(id);
        m_toast = "Removed " + row.title;
        m_toastUntil = m_clock + 2000;
        break;
    }
    case kMenuRetry:
        if (m_downloads && row.id.size() > std::strlen(kDownloadPrefix))
            m_downloads->retry(row.id.substr(std::strlen(kDownloadPrefix)));
        break;
    case kMenuPlayDownloaded:
    case kMenuShuffleDownloaded:
        if (m_downloads && m_player && row.id.size() > std::strlen(kDownloadPrefix)) {
            auto tracks = m_downloads->tracksOf(row.id.substr(std::strlen(kDownloadPrefix)));
            if (!tracks.empty())
                m_player->playTracks(std::move(tracks), 0, action == kMenuShuffleDownloaded);
        }
        break;
    case kMenuAddToPlaylist:
        if (track)
            startPlaylistPicker({row.track});
        else
            playCollection(row, PendingKind::AddToPlaylist);
        break;
    case kMenuRemoveFromPlaylist:
        removeFromPlaylist(row.track);
        break;
    case kMenuFavorite:
        toggleFavorite(row.track);
        break;
    case kMenuDeletePlaylist:
        deletePlaylistById(row.id, row.title);
        break;
    case kMenuGoAlbum: {
        m_view = View::Browse; // from Now Playing: leave it for the album's page
        m_lyricsView = false;
        MusicFrame f;
        f.kind = MusicPaneKind::AlbumTracks;
        f.id = row.track.albumId;
        f.title = row.track.album;
        f.subtitle = row.track.albumArtist;
        f.artId = row.track.albumId;
        f.artTag = row.track.albumImageTag;
        openFrame(f);
        break;
    }
    case kMenuGoArtist: {
        m_view = View::Browse;
        m_lyricsView = false;
        MusicFrame f;
        f.kind = MusicPaneKind::ArtistAlbums;
        f.id = row.track.artistId;
        f.title = row.track.artist;
        f.artId = row.track.artistId;
        openFrame(f);
        break;
    }
    default:
        break;
    }
}

bool MusicScreen::handleMenu(Action action)
{
    switch (action) {
    case Action::Up:
        m_menu.selected = (m_menu.selected + static_cast<int>(m_menu.items.size()) - 1) %
                          static_cast<int>(m_menu.items.size());
        return true;
    case Action::Down:
        m_menu.selected = (m_menu.selected + 1) % static_cast<int>(m_menu.items.size());
        return true;
    case Action::Confirm: {
        const MenuItem item = m_menu.items[m_menu.selected];
        const MusicRow row = m_menu.row;
        m_menu.open = false;
        runMenuAction(item.action, row);
        return true;
    }
    case Action::Back:
    case Action::ActionsMenu:
        m_menu.open = false;
        return true;
    default:
        return true; // the menu owns the keys while it is up
    }
}

bool MusicScreen::handleNowPlaying(Action action)
{
    if (!m_player || !miniPlayerVisible()) {
        m_view = View::Browse;
        return true;
    }
    const int queueSize = m_player->queue().size();
    switch (action) {
    case Action::Back:
        if (m_queueView)
            m_queueView = false;
        else if (m_lyricsView)
            m_lyricsView = false;
        else
            m_view = View::Browse;
        return true;
    case Action::Settings: // START
        m_view = View::Browse;
        return true;
    case Action::Menu: // SELECT
        m_queueView = !m_queueView;
        m_queueSelected = std::max(0, m_player->queue().position());
        return true;
    case Action::Confirm:
        if (m_queueView)
            m_player->jumpTo(m_queueSelected);
        else
            m_player->togglePause();
        return true;
    case Action::Up:
        if (m_queueView) {
            if (m_queueSelected > 0)
                --m_queueSelected;
        } else if (m_lyricsView) {
            m_lyricsScroll = std::max(0, m_lyricsScroll - 1);
        } else {
            m_lyricsView = true;
            requestLyrics();
        }
        return true;
    case Action::Down:
        if (m_queueView) {
            if (m_queueSelected + 1 < queueSize)
                ++m_queueSelected;
        } else if (m_lyricsView) {
            m_lyricsScroll =
                std::min(std::max(0, static_cast<int>(m_lyrics.size()) - 1), m_lyricsScroll + 1);
        } else {
            openMenu(trackRow(m_playerView.track, false)); // the playing song's options
        }
        return true;
    case Action::Left:
        if (!m_queueView)
            m_player->seekBy(-10);
        return true;
    case Action::Right:
        if (!m_queueView)
            m_player->seekBy(10);
        return true;
    case Action::PrevPage:
        m_player->previous();
        return true;
    case Action::NextPage:
        m_player->next();
        return true;
    case Action::PrevTab:
        m_player->previous();
        return true;
    case Action::NextTab:
        m_player->next();
        return true;
    case Action::Search:   // X
        if (m_queueView) { // the queue as a new playlist
            std::vector<music::Track> tracks;
            for (int i = 0; i < m_player->queue().size(); ++i)
                tracks.push_back(*m_player->queue().at(i));
            askPlaylistName("Save queue as playlist", "My queue", std::move(tracks));
        } else {
            m_player->setShuffle(!m_player->queue().shuffle());
        }
        return true;
    case Action::ActionsMenu: // Y
        if (m_queueView) {
            if (m_player->removeAt(m_queueSelected))
                m_queueSelected =
                    std::min(m_queueSelected, std::max(0, m_player->queue().size() - 1));
        } else {
            m_player->cycleRepeat();
        }
        return true;
    default:
        return false;
    }
}

void MusicScreen::settingsAction(int index)
{
    if (index == 0) {
        m_videoModeRequested = true;
    } else if (index == 1 && m_settings) {
        m_settings->streamKbps.store(music::MusicSettings::nextKbps(m_settings->streamKbps.load()));
        m_settings->save(kSettingsFile);
    } else if (index == 2 && m_settings) {
        m_settings->downloadKbps.store(
            music::MusicSettings::nextKbps(m_settings->downloadKbps.load()));
        m_settings->save(kSettingsFile);
    } else if (index == 3 && m_settings) {
        m_settings->albumGrid.store(!m_settings->albumGrid.load());
        m_settings->save(kSettingsFile);
        for (MusicPane& p : m_tabs[static_cast<int>(MusicTab::Library)].roots)
            clampPane(p);
        markDirty();
    } else if (index == 4) {
        ClockSettings& clock = ClockSettings::instance();
        clock.setHour24(!clock.hour24());
        clock.save("clock-settings.txt");
    } else if (index == 5) {
        m_zonePick = true;
        std::vector<std::string> items = {"Automatic (from network)", "Device setting"};
        for (const ClockZone& z : clockZones())
            items.push_back(z.label);
        m_picker.open("Time zone", items);
    } else if (index == 6) {
        if (!m_cacheClearArmed) {
            m_cacheClearArmed = true;
            m_toast = "Press A again to clear the music cache";
            m_toastUntil = m_clock + 3000;
            return;
        }
        m_cacheClearArmed = false;
        // Remove cached tracks, covers and lists (downloads are untouched).
        // On a worker thread; the track playing and the one queued next are left alone.
        std::set<std::string> keep;
        if (m_player)
            for (const std::string& path : m_player->inUsePaths())
                keep.insert(path);
        m_library->clearCaches(std::move(keep));
        m_toast = "Music cache cleared";
        m_toastUntil = m_clock + 2000;
    }
}

} // namespace miyoofin
