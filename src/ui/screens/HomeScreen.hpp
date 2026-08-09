#ifndef MIYOOFIN_HOME_SCREEN_HPP
#define MIYOOFIN_HOME_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../net/Session.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../ArtworkLayout.hpp"
#include <atomic>
#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {
struct LibrarySyncSchedule {
    bool inFlight=false, pending=false, hasStarted=false; Uint32 lastStart=0;
    bool request(Uint32 now) { if(inFlight){pending=true;return false;} if(hasStarted && now-lastStart<60000)return false; inFlight=true;hasStarted=true;lastStart=now;return true; }
    bool complete(Uint32 now) { inFlight=false; if(!pending || now-lastStart<60000) return false; pending=false; inFlight=true;lastStart=now;return true; }
};

/// The main Jellyfin-style home screen with top tabs, horizontal
/// media rows, card grid, and info panel for the selected item.
/// Fetches real library data from the server on a background thread.
class HomeScreen : public Screen {
public:
    explicit HomeScreen(const Session &session);
    ~HomeScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

    /// True when the user has confirmed logout (App handles the transition).
    bool logoutRequested() const { return m_logoutRequested; }

    /// Replace, insert, or remove Home's Continue Watching row.
    /// Public so the row behaviour can be tested without a network request.
    static void updateContinueWatchingRow(std::vector<TabData> &tabs,
                                          const std::vector<MediaItem> &items)
    {
        auto homeIt = std::find_if(tabs.begin(), tabs.end(),
            [](const TabData &tab) { return tab.name == "Home"; });
        if (homeIt == tabs.end()) return;

        auto &rows = homeIt->rows;
        auto cwIt = std::find_if(rows.begin(), rows.end(),
            [](const MediaRow &row) { return row.label == "Continue Watching"; });
        if (!items.empty()) {
            if (cwIt != rows.end()) {
                cwIt->items = items;
            } else {
                if (rows.size() == 1 && rows[0].label.empty() && rows[0].items.empty())
                    rows.clear();
                auto recentlyAdded = std::find_if(rows.begin(), rows.end(),
                    [](const MediaRow &row) { return row.label == "Recently Added"; });
                rows.insert(recentlyAdded, {"Continue Watching", items});
            }
        } else if (cwIt != rows.end()) {
            rows.erase(cwIt);
            if (rows.empty()) rows.push_back({"", {}});
        }
    }

    // --- Row artwork helpers (public for testing) -------------------------

    /// Build the row-artwork identity key for a media item.
    /// Format: "itemId:Primary:imageTag:WxH"
    static std::string rowArtworkKey(const MediaItem &item);

    /// Row artwork state map — public so tests can inspect it.
    std::map<std::string, RowArtworkEntry> m_rowArtwork;

    /// FIFO eviction order: oldest key at front.
    std::vector<std::string> m_rowArtworkOrder;

private:
    enum class LoadState { Loading, Ready, Error };

    LoadState m_loadState = LoadState::Loading;

    // Tab / row / card navigation state
    int m_activeTab;     // 0 = Home, 1 = Movies, 2 = Shows, 3 = Search, 4 = Downloads
    int m_activeRow;
    int m_activeCard;
    int m_rowScroll;
    int m_cardScroll;

    // Tab data (owned, populated by background fetch)
    std::vector<TabData> m_tabs;

    // Session info for API calls
    Session m_session;
    std::string m_userName;

    // Logout (two-step confirm on Y)
    bool m_logoutArmed = false;
    Uint32 m_logoutTimer = 0;
    bool m_logoutRequested = false;

    // Background fetch
    std::thread m_fetchThread;
    std::atomic<bool> m_fetchDone{false};
    std::string m_fetchError;
    std::vector<TabData> m_fetchResult;
    LibrarySnapshot m_cachedSnapshot;
    LibrarySnapshot m_remoteSnapshot;
    bool m_haveCachedSnapshot = false;
    LibrarySyncSchedule m_syncSchedule;
    std::thread m_posterThread;

    void startFetch();
    void requestFetch(Uint32 now);
    void finishFetch();
    void startPosterSync(const LibrarySnapshot &snapshot);

    // Lightweight refresh used when returning to an already-loaded Home.
    std::thread m_resumeRefreshThread;
    std::atomic<bool> m_resumeRefreshDone{false};
    std::atomic<bool> m_resumeRefreshInFlight{false};
    bool m_resumeRefreshPending = false;
    bool m_resumeRefreshSucceeded = false;
    std::string m_resumeRefreshError;
    std::vector<MediaItem> m_resumeRefreshResult;

    void startResumeRefresh();
    void finishResumeRefresh();

    // Helpers
    const TabData &currentTab() const;
    const MediaRow *currentRow() const;
    const MediaItem *currentItem() const;

    void clampNavigation();
    void drawTabBar(SDL_Surface *fb);
    void drawInfoPanel(SDL_Surface *fb);
    void drawRowList(SDL_Surface *fb);
    void drawCard(SDL_Surface *fb, int x, int y, int w, int h,
                  const MediaItem &item, bool selected);
    void drawPlaceholderTab(SDL_Surface *fb, const char *message);
    void drawBottomHints(SDL_Surface *fb);
    void drawLoadingState(SDL_Surface *fb);
    void drawErrorState(SDL_Surface *fb);

    // Selected artwork state (B5b)
    DecodedImage m_selectedArtwork;
    std::string  m_selectedArtworkId;      // "itemId:imageTag" identity key
    bool         m_selectedArtworkAttempted = false;

    void tryLoadSelectedArtwork();

    // Row artwork loading (B5d2a)
    void tryLoadOneRowArtwork();
    void evictRowArtworkIfNeeded();
    void drawMovieGrid(SDL_Surface *fb);
    static std::vector<TabData> tabsFromSnapshot(const LibrarySnapshot &snapshot);
    static std::vector<MediaItem> combineMovieViews(const std::vector<CachedLibraryView> &views);
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_SCREEN_HPP
