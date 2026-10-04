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

} // namespace miyoofin
