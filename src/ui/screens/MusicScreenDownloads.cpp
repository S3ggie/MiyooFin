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

namespace {

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

} // namespace

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

} // namespace miyoofin
