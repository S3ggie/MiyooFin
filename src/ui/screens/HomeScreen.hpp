#ifndef MIYOOFIN_HOME_SCREEN_HPP
#define MIYOOFIN_HOME_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../net/Session.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../ArtworkLayout.hpp"
#include <atomic>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {

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

    void startFetch();
    void finishFetch();

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
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_SCREEN_HPP