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

} // namespace miyoofin
