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

} // namespace miyoofin
