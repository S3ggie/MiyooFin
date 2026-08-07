#ifndef MIYOOFIN_HOME_SCREEN_HPP
#define MIYOOFIN_HOME_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MockData.hpp"

namespace miyoofin {

/// The main Jellyfin-style home screen with top tabs, horizontal
/// media rows, card grid, and info panel for the selected item.
class HomeScreen : public Screen {
public:
    HomeScreen();
    ~HomeScreen() override = default;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

private:
    // Tab / row / card navigation state
    int m_activeTab;     // 0 = Home, 1 = Movies, 2 = Shows, 3 = Search, 4 = Downloads
    int m_activeRow;     // index into current tab's rows
    int m_activeCard;    // index into current row's items
    int m_rowScroll;     // vertical scroll offset (first visible row index)
    int m_cardScroll;    // horizontal scroll offset (first visible card index)

    // Cached mock data reference
    const std::vector<TabData> &m_tabs;

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
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_SCREEN_HPP