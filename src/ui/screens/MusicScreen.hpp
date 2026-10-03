#ifndef MIYOOFIN_MUSIC_SCREEN_HPP
#define MIYOOFIN_MUSIC_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../download/DownloadManager.hpp"
#include "../../music/MusicDownloads.hpp"
#include "../../music/MusicLibrary.hpp"
#include "../../music/MusicPlayer.hpp"
#include "../../music/MusicSettings.hpp"
#include "../../net/Session.hpp"
#include "../BatteryMonitor.hpp"
#include "../MusicUiState.hpp"
#include "ChoiceMenu.hpp"
#include <SDL2/SDL.h>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace miyoofin {

/// One line in a Music list: a heading, a track, an album, an artist, a playlist or a
/// settings entry. Rows are built from server pages and carry what the actions need.
struct MusicRow
{
    enum class Kind
    {
        Heading,
        Track,
        Album,
        Artist,
        Playlist,
        Action
    } kind = Kind::Heading;
    std::string id, title, subtitle, right;
    std::string artId, artTag; // cover (empty = none)
    music::Track track;
    music::Album album;
    music::Artist artist;
    music::Playlist playlist;
    int progress = -1; // 0-100 for a download in flight, -1 = none
    bool selectable() const
    {
        return kind != Kind::Heading;
    }
};

/// A page of rows with its cursor. The cursor part (MusicFrame) is what gets persisted.
struct MusicPane
{
    MusicFrame frame;
    std::vector<MusicRow> rows;
    std::uint64_t ticket = 0, ticket2 = 0; // outstanding listing requests (Home has two)
    bool requested = false, failed = false, loadedOnce = false, hasMore = false;
    int total = 0;
    std::uint64_t builtRevision = 0; // download state the rows were built from
    std::string error;
    // Home is assembled from two listings.
    std::vector<music::Album> homeAlbums;
    std::vector<music::Track> homeTracks;
};

/// MiyooFin Music: the root screen of the music mode. Five tabs under an always-visible
/// header, a mini-player strip while something plays, and a full Now Playing view. All
/// network and disk work happens on MusicLibrary/MusicPlayer workers.
class MusicScreen : public Screen
{
  public:
    MusicScreen(const Session& session, music::MusicPlayer* player, music::MusicSettings* settings,
                music::MusicDownloads* downloads);
    ~MusicScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface* fb) override;
    const char* diagnosticName() const override
    {
        return "MusicScreen";
    }
    bool deferDestruction() const override
    {
        return true;
    }

    /// True once when the user asked to go back to MiyooFin (App swaps the root screen).
    bool takeVideoModeRequest()
    {
        const bool requested = m_videoModeRequested;
        m_videoModeRequested = false;
        return requested;
    }
    /// Writes the page state now (App calls this before leaving the mode).
    void saveState();

    // ---- exposed for tests ----
    int activeTabForTest() const
    {
        return m_activeTab;
    }
    int rowCountForTest() const
    {
        return static_cast<int>(activePane().rows.size());
    }
    int selectedForTest() const
    {
        return activePane().frame.selected;
    }
    std::string selectedTitleForTest() const
    {
        const MusicPane& p = activePane();
        return p.frame.selected >= 0 && p.frame.selected < static_cast<int>(p.rows.size())
                   ? p.rows[p.frame.selected].title
                   : std::string();
    }
    int drillDepthForTest() const
    {
        return static_cast<int>(m_tabs[m_activeTab].drill.size());
    }
    MusicPaneKind paneKindForTest() const
    {
        return activePane().frame.kind;
    }
    const std::string& toastForTest() const
    {
        return m_toast;
    }
    bool pickerOpenForTest() const
    {
        return m_picker.active();
    }
    bool menuOpenForTest() const
    {
        return m_menu.open;
    }
    static std::vector<std::string> tabNames();
    static music::ListingRequest requestFor(const MusicFrame& frame, int start);

  private:
    enum class View
    {
        Browse,
        NowPlaying
    };
    enum class PendingKind
    {
        SyncDownload,
        PlayAll,
        ShuffleAll,
        PlayNext,
        Append,
        Download,
        AddToPlaylist, // fetch an album/playlist's tracks, then pick the playlist
        PickPlaylist   // the playlist list for the picker has arrived
    };
    struct Pending
    {
        PendingKind kind;
        int startIndex = 0;
        music::DownloadCollection collection; // for Download
        std::vector<music::Track> loaded;     // pages of a long list fetched so far
    };
    struct MenuItem
    {
        std::string label;
        int action;
    };
    struct Menu
    {
        bool open = false;
        int selected = 0;
        std::vector<MenuItem> items;
        MusicRow row; // what the menu acts on
    };

    // tabs and panes
    MusicPane& activePane();
    const MusicPane& activePane() const;
    std::vector<MusicPane>& rootsOf(int tab);
    MusicPane makePane(const MusicFrame& frame) const;
    void buildFromState(const MusicUiState& state);
    MusicUiState snapshotState() const;
    void setTab(int tab);
    void openFrame(const MusicFrame& frame);
    void popFrame();
    void requestPane(MusicPane& pane, bool nextPage);
    void applyListResult(const music::ListResult& result);
    void rebuildRows(MusicPane& pane);
    void restoreSelection(MusicPane& pane, const std::string& selectedId);
    void clampPane(MusicPane& pane);
    void refreshSettingsRows(MusicPane& pane);
    void syncResumeRow(MusicPane& pane);
    void syncNewPlaylistRow(MusicPane& pane);
    // playlists
    void syncDownloadedPlaylists(); // refresh the copies marked "keep in sync"
    bool m_syncStarted = false;
    void startPlaylistPicker(std::vector<music::Track> tracks);
    void openPickerFor(const std::vector<music::Playlist>& playlists);
    void chosePlaylist(int index);
    void askPlaylistName(const std::string& title, const std::string& initial,
                         std::vector<music::Track> tracks);
    void createPlaylistNamed(const std::string& name, std::vector<music::Track> tracks);
    void deletePlaylistById(const std::string& id, const std::string& name);
    void removeFromPlaylist(const music::Track& track);
    void applyJobResults();
    void refreshPlaylists();
    void refreshDownloadRows(MusicPane& pane);
    music::DownloadCollection collectionFor(const MusicRow& row) const;
    music::DownloadCollection collectionForPane(const MusicPane& pane) const;
    void downloadTracks(const music::DownloadCollection& collection,
                        const std::vector<music::Track>& tracks);
    bool isDownloaded(const MusicRow& row) const;
    void markDirty()
    {
        m_stateDirty = true;
    }

    // actions
    bool handleBrowse(Action action);
    bool handleNowPlaying(Action action);
    bool handleMenu(Action action);
    void activateRow(const MusicRow& row);
    void openMenu(const MusicRow& row);
    void runMenuAction(int action, const MusicRow& row);
    void playFromPane(int rowIndex);
    void playCollection(const MusicRow& row, PendingKind kind);
    void applyPending(const music::ListResult& result, const Pending& pending);
    void cycleLetter(int delta);
    void settingsAction(int index);

    // covers
    void requestVisibleCovers();
    SDL_Surface* cover(const std::string& id, const std::string& tag, int size, bool request);
    void takeCovers();
    void trimCovers();

    // rendering
    void renderBrowse(SDL_Surface* fb);
    void renderPane(SDL_Surface* fb, const MusicPane& pane, int top, int bottom);
    void renderSubBar(SDL_Surface* fb);
    void renderDetailHeader(SDL_Surface* fb, const MusicPane& pane, int top);
    void renderMiniPlayer(SDL_Surface* fb, int top);
    void renderNowPlaying(SDL_Surface* fb);
    void renderMenu(SDL_Surface* fb);
    void renderFooter(SDL_Surface* fb, bool miniPlayerShown);
    void drawCover(SDL_Surface* fb, const std::string& id, const std::string& tag, int x, int y,
                   int size, int requestSize, const std::string& label);
    bool miniPlayerVisible() const;
    int contentBottom() const;
    int visibleRows(const MusicPane& pane) const;
    bool isGrid(const MusicPane& pane) const; ///< Albums tab shown as a cover grid
    void renderGrid(SDL_Surface* fb, const MusicPane& pane, int top, int bottom);
    int detailHeaderHeight(const MusicPane& pane) const;

    Session m_session;
    music::MusicPlayer* m_player;
    music::MusicSettings* m_settings;
    music::MusicDownloads* m_downloads;
    std::uint64_t m_downloadsRevision = 0;
    std::string m_removeArmed; // collection or playlist whose removal awaits a second press
    ChoiceMenu m_picker;       // "add to which playlist?"
    std::vector<music::Track> m_pickTracks;
    std::vector<music::Playlist> m_pickPlaylists;
    std::unique_ptr<music::MusicLibrary> m_library;
    BatteryMonitor m_battery;

    int m_activeTab = 0;
    struct TabRuntime
    {
        std::vector<MusicPane> roots;
        int section = 0;
        std::vector<MusicPane> drill;
    };
    std::vector<TabRuntime> m_tabs;
    View m_view = View::Browse;
    // Lyrics (Now Playing, Up): fetched per track on a job, synced lines follow the position.
    bool m_lyricsView = false;
    std::string m_lyricsFor; // track id the lyrics below belong to
    bool m_lyricsLoading = false;
    std::vector<music::LyricLine> m_lyrics;
    int m_lyricsScroll = 0; // first line shown when the lyrics are not synced
    void requestLyrics();
    void renderLyrics(SDL_Surface* fb);
    bool m_queueView = false; // Now Playing shows the queue instead of the art
    int m_queueSelected = 0;
    Menu m_menu;
    std::map<std::uint64_t, Pending> m_pending;
    bool m_videoModeRequested = false;
    bool m_cacheClearArmed = false;
    std::string m_toast;
    Uint32 m_toastUntil = 0;

    // cover surfaces: "id:tag:size" -> surface, bounded LRU
    std::map<std::string, SDL_Surface*> m_covers;
    std::deque<std::string> m_coverOrder;
    std::set<std::string> m_coverRequested;
    std::set<std::string> m_coverMissing;

    music::PlayerView m_playerView; // one snapshot per frame (view() copies strings)
    bool m_stateDirty = false;
    Uint32 m_stateSavedAt = 0;
    Uint32 m_clock = 0;
    std::uint64_t m_frame = 0;
};

} // namespace miyoofin

#endif
