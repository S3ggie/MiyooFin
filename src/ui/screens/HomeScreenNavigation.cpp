#include "HomeScreen.hpp"
#include "SeriesScreen.hpp"
#include "MovieDetailsScreen.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../../data/MovieTitle.hpp"
#include "../ShowsBrowser.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../app/ScreenStack.hpp"
#include "../BitmapFont.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace miyoofin {

static constexpr int VISIBLE_ROWS = 3;
static constexpr int SETTINGS_VISIBLE_ROWS = 6;
static constexpr int CARD_GAP = 6;
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;

static bool gridAtTopRow(int selected, int columns)
{
    return selected >= 0 && selected < columns;
}

static bool gridAtBottomRow(int selected, int count, int columns)
{
    if (count <= 0 || columns <= 0 || selected < 0)
        return false;
    selected = std::min(selected, count - 1);
    return selected / columns == (count - 1) / columns;
}

static int clampMovieGridScrollCompact(int selected, int count, int currentScroll)
{
    if (count <= 0)
        return 0;
    selected = std::max(0, std::min(selected, count - 1));
    const int selectedRow = selected / MOVIE_GRID_COLUMNS;
    const int lastRow = (count - 1) / MOVIE_GRID_COLUMNS;
    const int maxScroll = std::max(0, lastRow - MOVIE_GRID_ROWS + 1);

    currentScroll = std::max(0, std::min(currentScroll, maxScroll));
    if (selectedRow < currentScroll)
        currentScroll = selectedRow;
    if (selectedRow >= currentScroll + MOVIE_GRID_ROWS) {
        currentScroll = selectedRow - MOVIE_GRID_ROWS + 1;
    }
    return std::max(0, std::min(currentScroll, maxScroll));
}

void HomeScreen::refreshMovieFilter()
{
    const int movies = tabIndex("Movies");
    if (movies < 0)
        return;
    const bool moviesActive = activeTabNamed("Movies");
    const MediaItem *previousItem = currentItem();
    const std::string selectedId = previousItem ? previousItem->id
                                                : m_moviePreviewId;
    const int previousSelected = m_activeCard;
    const int previousScroll = m_rowScroll;
    std::vector<MediaItem> displayed;
    for (const auto &item : m_movieWindow) {
        if (movieMatchesAlphabetFilter(item.title, m_movieActiveLetter))
            displayed.push_back(item);
    }
    m_tabs[movies].rows = {{"Movies", std::move(displayed)}};
    if (!moviesActive) return;
    m_activeRow = 0;
    m_activeCard = restoreSelectionIndex(m_tabs[movies].rows[0].items,
                                         selectedId, previousSelected);
    m_rowScroll = preserveGridScroll(m_activeCard,
                                     static_cast<int>(m_tabs[movies].rows[0].items.size()),
                                     previousScroll, MOVIE_GRID_COLUMNS, MOVIE_GRID_ROWS);
    m_cardScroll = 0;
    const MediaItem *current = currentItem();
    if (!current || current->id != selectedId) {
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
    }
    if (current) m_moviePreviewId = current->id;
}

HomeScreen::ShowsFocusState HomeScreen::showsFocusAfterRefresh(
    ShowsFocusState previous, bool hasShows, bool hasAnime)
{
    if (previous == ShowsFocusState::AnimeGrid && hasAnime)
        return ShowsFocusState::AnimeGrid;
    if (previous == ShowsFocusState::ShowsGrid && hasShows)
        return ShowsFocusState::ShowsGrid;
    if (hasShows)
        return ShowsFocusState::ShowsGrid;
    if (hasAnime)
        return ShowsFocusState::AnimeGrid;
    return ShowsFocusState::AlphabetRail;
}

int HomeScreen::restoreSelectionIndex(const std::vector<MediaItem> &items,
                                      const std::string &selectedId,
                                      int fallback)
{
    if (items.empty())
        return 0;
    if (!selectedId.empty()) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (items[i].id == selectedId)
                return i;
        }
    }
    return std::max(0, std::min(
        fallback, static_cast<int>(items.size()) - 1));
}

int HomeScreen::preserveGridScroll(int selected, int count, int currentScroll,
                                   int columns, int rows)
{
    if (count <= 0 || columns <= 0 || rows <= 0)
        return 0;
    selected = std::max(0, std::min(selected, count - 1));
    const int lastRow = (count - 1) / columns;
    const int maxScroll = std::max(0, lastRow - rows + 1);
    currentScroll = std::max(0, std::min(currentScroll, maxScroll));
    const int selectedRow = selected / columns;
    if (selectedRow < currentScroll)
        currentScroll = selectedRow;
    if (selectedRow >= currentScroll + rows)
        currentScroll = selectedRow - rows + 1;
    return std::max(0, std::min(currentScroll, maxScroll));
}

void HomeScreen::refreshShowsFilter()
{
    const ShowsFocusState previousFocus =
        m_showsFocus == ShowsFocus::AnimeGrid ? ShowsFocusState::AnimeGrid
        : m_showsFocus == ShowsFocus::ShowsGrid ? ShowsFocusState::ShowsGrid
        : ShowsFocusState::AlphabetRail;
    const MediaItem *previousItem = showsSelectedItem();
    const std::string selectedId = previousItem ? previousItem->id : m_showsPreviewId;
    const int previousShowSelected = m_showSelected;
    const int previousAnimeSelected = m_animeSelected;
    const int previousShowScroll = m_showScroll;
    const int previousAnimeScroll = m_animeScroll;
    m_filteredShows.clear();
    m_filteredAnime.clear();
    for (const auto &item : m_showWindow)
        if (matchesAlphabetFilter(item.title, m_showsActiveLetter))
            m_filteredShows.push_back(item);
    for (const auto &item : m_animeWindow)
        if (matchesAlphabetFilter(item.title, m_showsActiveLetter))
            m_filteredAnime.push_back(item);
    const ShowsFocusState nextFocus = showsFocusAfterRefresh(
        previousFocus, !m_filteredShows.empty(), !m_filteredAnime.empty());
    m_showsFocus = nextFocus == ShowsFocusState::AnimeGrid ? ShowsFocus::AnimeGrid
        : nextFocus == ShowsFocusState::ShowsGrid ? ShowsFocus::ShowsGrid
        : ShowsFocus::AlphabetRail;
    m_showSelected = restoreSelectionIndex(
        m_filteredShows, selectedId, previousShowSelected);
    m_animeSelected = restoreSelectionIndex(
        m_filteredAnime, selectedId, previousAnimeSelected);
    m_showScroll = preserveGridScroll(
        m_showSelected, static_cast<int>(m_filteredShows.size()),
        previousShowScroll, SHOWS_GRID_COLUMNS, SHOWS_GRID_ROWS);
    m_animeScroll = preserveGridScroll(
        m_animeSelected, static_cast<int>(m_filteredAnime.size()),
        previousAnimeScroll, SHOWS_GRID_COLUMNS, SHOWS_GRID_ROWS);
    if (const MediaItem *item = showsSelectedItem())
        m_showsPreviewId = item->id;
}
const MediaItem *HomeScreen::showsSelectedItem() const
{
    const std::vector<MediaItem> *items =
        m_showsFocus == ShowsFocus::AnimeGrid ? &m_filteredAnime
                                              : &m_filteredShows;
    const int selected = m_showsFocus == ShowsFocus::AnimeGrid
        ? m_animeSelected : m_showSelected;
    if (selected >= 0 && selected < static_cast<int>(items->size()))
        return &(*items)[selected];
    for (const auto &item : m_filteredShows) {
        if (item.id == m_showsPreviewId)
            return &item;
    }
    for (const auto &item : m_filteredAnime) {
        if (item.id == m_showsPreviewId)
            return &item;
    }
    return nullptr;
}

void HomeScreen::clampShowsNavigation()
{
    if (!m_filteredShows.empty()) {
        m_showSelected = std::max(
            0, std::min(m_showSelected,
                        static_cast<int>(m_filteredShows.size()) - 1));
        m_showScroll = clampShowsGridScroll(
            m_showSelected, m_filteredShows.size(), m_showScroll);
    } else {
        m_showSelected = 0;
        m_showScroll = 0;
    }
    if (!m_filteredAnime.empty()) {
        m_animeSelected = std::max(
            0, std::min(m_animeSelected,
                        static_cast<int>(m_filteredAnime.size()) - 1));
        m_animeScroll = clampShowsGridScroll(
            m_animeSelected, m_filteredAnime.size(), m_animeScroll);
    } else {
        m_animeSelected = 0;
        m_animeScroll = 0;
    }
    if (const MediaItem *item = showsSelectedItem())
        m_showsPreviewId = item->id;
}

int HomeScreen::moveMovieGridCompact(int index, int count, int deltaRow, int deltaCol) const
{
    if (count <= 0) return 0;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    int row = index / MOVIE_GRID_COLUMNS + deltaRow;
    int col = index % MOVIE_GRID_COLUMNS + deltaCol;
    if (col < 0 || col >= MOVIE_GRID_COLUMNS || row < 0) return index;
    int target = row * MOVIE_GRID_COLUMNS + col;
    if (target >= count) return deltaRow > 0 ? count - 1 : index;
    return target;
}

const TabData &HomeScreen::currentTab() const
{
    static const TabData empty{"", {}};
    return m_activeTab>=0 && m_activeTab<(int)m_tabs.size() ? m_tabs[m_activeTab] : empty;
}

const char *HomeScreen::diagnosticTabName() const
{
    return currentTab().name.empty() ? "other" : currentTab().name.c_str();
}

bool HomeScreen::activeTabNamed(const char *name) const
{
    return currentTab().name == name;
}

int HomeScreen::tabIndex(const char *name) const
{
    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i) {
        if (m_tabs[i].name == name)
            return i;
    }
    return -1;
}

int HomeScreen::tabIndexAtPoint(const std::vector<TabData> &tabs, int x, int y)
{
    static constexpr int TAB_Y = 0;
    static constexpr int TAB_H = 24;
    if (y < TAB_Y || y >= TAB_Y + TAB_H)
        return -1;

    int tabX = 8;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
        const int tabWidth = static_cast<int>(tabs[i].name.size())
            * BitmapFont::GLYPH_W + 16;
        if (x >= tabX && x < tabX + tabWidth)
            return i;
        tabX += tabWidth;
    }
    return -1;
}

void HomeScreen::activateTab(int index)
{
    if (index < 0 || index >= static_cast<int>(m_tabs.size()))
        return;
    m_activeTab = index;
    m_movieRailFocused = false;
    m_activeRow = 0;
    m_activeCard = 0;
    m_rowScroll = 0;
    m_cardScroll = 0;
    clampNavigation();
    if (activeTabNamed("Movies") || activeTabNamed("Shows"))
        requestFetch(SDL_GetTicks());
    if ((activeTabNamed("Downloads") || activeTabNamed("Settings")) && m_downloads) {
        if (activeTabNamed("Downloads"))
            m_downloads->requestReconcile();
        refreshDownloads();
    }
}

const MediaRow *HomeScreen::currentRow() const
{
    const auto &rows = currentTab().rows;
    if (m_activeRow < (int)rows.size())
        return &rows[m_activeRow];
    return nullptr;
}

const MediaItem *HomeScreen::currentItem() const
{
    const MediaRow *row = currentRow();
    if (row && m_activeCard < (int)row->items.size())
        return &row->items[m_activeCard];
    return nullptr;
}

std::string HomeScreen::focusedHomeRowLabel() const
{
    const MediaRow *r = currentRow();
    return r ? r->label : "";
}

void HomeScreen::restoreHomeRowFocus(const std::string &label)
{
    if (!activeTabNamed("Home")) return;
    const auto &rows = currentTab().rows;
    if (rows.empty()) {
        m_activeRow = 0;
        return;
    }
    const int idx = homeRowIndexByLabel(rows, label);
    if (idx >= 0) {
        m_activeRow = idx;
    } else {
        if (m_activeRow < 0) m_activeRow = 0;
        if (m_activeRow >= static_cast<int>(rows.size()))
            m_activeRow = static_cast<int>(rows.size()) - 1;
        m_activeCard = 0;
        m_cardScroll = 0;
    }
}

void HomeScreen::clampNavigation()
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) {
        m_activeRow = 0;
        m_activeCard = 0;
        m_rowScroll = 0;
        m_cardScroll = 0;
        return;
    }
    if (activeTabNamed("Movies")) {
        m_activeRow = 0;
        const auto &items = rows[0].items;
        if (items.empty()) {
            m_activeCard = 0;
            m_rowScroll = 0;
            m_cardScroll = 0;
            return;
        }
        if (m_activeCard < 0) m_activeCard = 0;
        if (m_activeCard >= (int)items.size())
            m_activeCard = (int)items.size() - 1;
        m_rowScroll = clampMovieGridScrollCompact(
            m_activeCard, (int)items.size(), m_rowScroll);
        m_cardScroll = 0;
        if (const MediaItem *item = currentItem())
            m_moviePreviewId = item->id;
        return;
    }
    if (activeTabNamed("Shows")) {
        clampShowsNavigation();
        return;
    }
    if (m_activeRow < 0) m_activeRow = 0;
    if (m_activeRow >= (int)rows.size()) m_activeRow = (int)rows.size() - 1;
    const auto &items = rows[m_activeRow].items;
    if (items.empty()) {
        m_activeCard = 0;
        m_cardScroll = 0;
        return;
    }
    if (m_activeCard < 0) m_activeCard = 0;
    if (m_activeCard >= (int)items.size()) m_activeCard = (int)items.size() - 1;
    if (m_activeRow < m_rowScroll) m_rowScroll = m_activeRow;
    if (m_activeRow >= m_rowScroll + VISIBLE_ROWS)
        m_rowScroll = m_activeRow - VISIBLE_ROWS + 1;
    static constexpr int HMARGIN = 4;
    m_cardScroll = clampCardScroll(items, m_activeCard, m_cardScroll,
                                    640, HMARGIN, CARD_GAP);
}

bool HomeScreen::handlePointerClick(int x, int y)
{
    if (m_loadState != LoadState::Ready)
        return false;
    const int index = tabIndexAtPoint(m_tabs, x, y);
    if (index < 0)
        return false;
    activateTab(index);
    return true;
}

bool HomeScreen::handleAction(Action action)
{
    // Back always dismisses an armed logout prompt before any screen-specific
    // Back behavior (including the Movies alphabet rail).
    if (m_logoutArmed && action == Action::Back) {
        m_logoutArmed = false;
        m_logoutTimer = 0;
        return true;
    }
    if (m_logoutArmed && action != Action::ActionsMenu) {
        m_logoutArmed = false;
        m_logoutTimer = 0;
    }

    // Loading: only allow logout
    if (m_loadState == LoadState::Loading) {
        if (action == Action::ActionsMenu && !m_logoutArmed) {
            m_logoutArmed = true;
            m_logoutTimer = 3000;
            return true;
        }
        return false;
    }

    // Error: allow retry (A) and logout (Y)
    if (m_loadState == LoadState::Error) {
        if (action == Action::Confirm) {
            m_loadState = LoadState::Loading;
            m_fetchDone = false;
            m_fetchError.clear();
            m_fetchResult.clear();
            startFetch();
            return true;
        }
        if (action == Action::ActionsMenu && !m_logoutArmed) {
            m_logoutArmed = true;
            m_logoutTimer = 3000;
            return true;
        }
        return false;
    }

    // Ready: normal navigation
    if (activeTabNamed("Downloads") && handleDownloadsAction(action))
        return true;
    if (activeTabNamed("Settings")) {
        if (action == Action::Back) {
            if (m_settingsConfirmation != SettingsConfirmation::None) {
                m_settingsConfirmation = SettingsConfirmation::None;
                return true;
            }
            return false;
        }
        if (action == Action::Up || action == Action::Down) {
            const int previous = m_settingsSelected;
            if (action == Action::Up && m_settingsSelected > 0) {
                --m_settingsSelected;
            } else if (action == Action::Down
                       && m_settingsSelected < settingsRowCount(m_session) - 1) {
                ++m_settingsSelected;
            }
            if (m_settingsSelected != previous)
                m_settingsConfirmation = SettingsConfirmation::None;
            m_settingsScroll = std::max(
                0, std::min(m_settingsSelected,
                            settingsRowCount(m_session) - SETTINGS_VISIBLE_ROWS));
            return true;
        }
        if (action == Action::Confirm) {
            switch (settingsRowAction(m_settingsSelected,m_session)) {
            case SettingsRowAction::OfflineMode: {
                m_session.manualOfflineMode = !m_session.manualOfflineMode;
                if (m_libraryCoordinator)
                    m_libraryCoordinator->setManualOfflineMode(
                        m_session.manualOfflineMode);
                const bool sessionSaved = m_session.save();
                std::printf("[HomeScreen] manual_offline_mode=%s saved=%s\n",
                            m_session.manualOfflineMode ? "ON" : "OFF",
                            sessionSaved ? "yes" : "no");
                // Cancel any in-flight fetch so the mode switch happens as
                // soon as the fetch notices cancellation (bounded: between
                // pages), instead of running the old mode's sync to completion.
                if (m_fetchCancellation)
                    m_fetchCancellation->store(true);
                // When entering offline mode, try the instant cached path
                // first — avoids a full fetch when downloads haven't changed.
                if (m_session.manualOfflineMode
                    && tryApplyCachedOfflineSnapshot()) {
                    return true;
                }
                // Drive the existing manual-offline fetch path via startFetch().
                // The worker captures session by value, sees the toggled
                // manualOfflineMode, and either builds the offline snapshot
                // (offlineTabsFromSnapshot, removing Home/Search tabs) or runs
                // the full online sync.
                if (!startFetch())
                    m_offlineModeFetchPending = true;
                return true;
            }
            case SettingsRowAction::LocalAddress:
                m_localAddressRequested = true;
                return true;
            case SettingsRowAction::PublicAddress:
                m_publicAddressRequested = true;
                return true;
            case SettingsRowAction::ChangeServer:
            case SettingsRowAction::Logout: {
                const SettingsConfirmation requested =
                    settingsRowAction(m_settingsSelected, m_session)
                            == SettingsRowAction::ChangeServer
                        ? SettingsConfirmation::ChangeServer
                        : SettingsConfirmation::Logout;
                if (m_settingsConfirmation == requested) {
                    if (requested == SettingsConfirmation::ChangeServer)
                        m_changeServerRequested = true;
                    else
                        m_logoutRequested = true;
                    m_settingsConfirmation = SettingsConfirmation::None;
                } else {
                    m_settingsConfirmation = requested;
                }
                return true;
            }
            case SettingsRowAction::CheckForUpdates: {
                if (!m_updateManager.enabled())
                    return true;
                const auto stage = m_updateSnapshot.stage;
                if (stage == UpdateStage::ReadyToRestart) {
                    m_updateExitRequested = true;
                } else if (stage == UpdateStage::Available) {
                    if (m_settingsConfirmation
                        == SettingsConfirmation::CheckForUpdates) {
                        m_updateManager.confirmInstall();
                        m_settingsConfirmation = SettingsConfirmation::None;
                    } else {
                        m_settingsConfirmation = SettingsConfirmation::CheckForUpdates;
                    }
                } else if (stage == UpdateStage::Idle ||
                           stage == UpdateStage::UpToDate ||
                           stage == UpdateStage::Error) {
                    m_updateManager.checkForUpdates();
                }
                return true;
            }
            case SettingsRowAction::None:
                break;
            }
        }
        // Settings owns its account actions; do not let Y or X trigger Home actions.
        if (action == Action::ActionsMenu || action == Action::Search)
            return true;
        if (action != Action::NextTab && action != Action::PrevTab)
            return true;
        m_settingsScroll = std::max(
            0, std::min(m_settingsSelected,
                        settingsRowCount(m_session) - SETTINGS_VISIBLE_ROWS));
    }
    auto queueDownAtPageEdge = [this](MediaPageState &page, int selected,
                                      int count, int columns) {
        if (!gridAtBottomRow(selected, count, columns) || !page.hasMore)
            return false;
        page.pendingDown = true;
        requestMediaPage(page);
        return true;
    };

    switch (action) {
    case Action::Up:
        if (activeTabNamed("Shows")) {
            if (m_showsFocus == ShowsFocus::AlphabetRail) {
                if (m_showsAlphabetFocus > 0)
                    --m_showsAlphabetFocus;
            } else if (m_showsFocus == ShowsFocus::ShowsGrid) {
                if (gridAtTopRow(m_showSelected, SHOWS_GRID_COLUMNS))
                    requestEarlierMediaPage(m_showPage);
                else
                    m_showSelected = moveShowsGrid(
                        m_showSelected, m_filteredShows.size(), -1, 0);
            } else {
                if (gridAtTopRow(m_animeSelected, SHOWS_GRID_COLUMNS))
                    requestEarlierMediaPage(m_animePage);
                else
                    m_animeSelected = moveShowsGrid(
                        m_animeSelected, m_filteredAnime.size(), -1, 0);
            }
            clampShowsNavigation();
            return true;
        }
        if (activeTabNamed("Movies")) {
            if (m_movieRailFocused) {
                if (m_movieAlphabetFocus > 0)
                    --m_movieAlphabetFocus;
            } else if (currentRow()) {
                if (gridAtTopRow(m_activeCard, MOVIE_GRID_COLUMNS))
                    requestEarlierMediaPage(m_moviePage);
                else
                    m_activeCard = moveMovieGridCompact(
                        m_activeCard, static_cast<int>(currentRow()->items.size()),
                        -1, 0);
            }
        } else {
            const int previousRow = m_activeRow;
            --m_activeRow;
            clampNavigation();
            if (m_activeRow != previousRow) {
                m_cardScroll = 0;
                clampNavigation();
            }
        }
        clampNavigation();
        return true;
    case Action::Down:
        if (activeTabNamed("Shows")) {
            if (m_showsFocus == ShowsFocus::AlphabetRail) {
                if (m_showsAlphabetFocus < 25)
                    ++m_showsAlphabetFocus;
            } else if (m_showsFocus == ShowsFocus::ShowsGrid) {
                const bool heldForPage = queueDownAtPageEdge(
                    m_showPage, m_showSelected, m_filteredShows.size(),
                    SHOWS_GRID_COLUMNS);
                if (!heldForPage)
                    m_showSelected = moveShowsGrid(
                        m_showSelected, m_filteredShows.size(), 1, 0);
            } else {
                const bool heldForPage = queueDownAtPageEdge(
                    m_animePage, m_animeSelected, m_filteredAnime.size(),
                    SHOWS_GRID_COLUMNS);
                if (!heldForPage)
                    m_animeSelected = moveShowsGrid(
                        m_animeSelected, m_filteredAnime.size(), 1, 0);
            }
            clampShowsNavigation();
            return true;
        }
        if (activeTabNamed("Movies")) {
            if (m_movieRailFocused) {
                if (m_movieAlphabetFocus < 25)
                    ++m_movieAlphabetFocus;
            } else if (currentRow()) {
                const int count = static_cast<int>(currentRow()->items.size());
                const bool heldForPage = queueDownAtPageEdge(
                    m_moviePage, m_activeCard, count, MOVIE_GRID_COLUMNS);
                if (!heldForPage)
                    m_activeCard = moveMovieGridCompact(
                        m_activeCard, count, 1, 0);
            }
        } else {
            const int previousRow = m_activeRow;
            ++m_activeRow;
            clampNavigation();
            if (m_activeRow != previousRow) {
                m_cardScroll = 0;
                clampNavigation();
            }
        }
        clampNavigation();
        return true;
    case Action::Left:
        if (activeTabNamed("Shows")) {
            if (m_showsFocus == ShowsFocus::AlphabetRail)
                return true;
            if (m_showsFocus == ShowsFocus::ShowsGrid) {
                if (m_showSelected % 4) {
                    --m_showSelected;
                } else {
                    m_showsFocus = ShowsFocus::AlphabetRail;
                    m_showsAlphabetFocus = m_showsActiveLetter >= 0
                        ? m_showsActiveLetter
                        : alphabetFocus(m_filteredShows[m_showSelected].title);
                }
            } else if (m_animeSelected % 4) {
                --m_animeSelected;
            } else if (!m_filteredShows.empty()) {
                m_showsFocus = ShowsFocus::ShowsGrid;
                m_showSelected = crossShowsGridIndex(
                    m_animeSelected, m_filteredShows.size(), false);
            } else {
                m_showsFocus = ShowsFocus::AlphabetRail;
            }
            clampShowsNavigation();
            return true;
        }
        if (activeTabNamed("Movies")) {
            if (!m_movieRailFocused
                && (!currentRow() || m_activeCard % MOVIE_GRID_COLUMNS == 0)) {
                m_movieRailFocused = true;
                m_movieAlphabetFocus = m_movieActiveLetter >= 0
                    ? m_movieActiveLetter
                    : movieAlphabetFocus(currentItem() ? currentItem()->title : std::string());
            } else if (!m_movieRailFocused && currentRow()) {
                m_activeCard = moveMovieGridCompact(
                    m_activeCard, static_cast<int>(currentRow()->items.size()),
                    0, -1);
            }
        } else {
            --m_activeCard;
        }
        clampNavigation();
        return true;
    case Action::Right:
        if (activeTabNamed("Shows")) {
            if (m_showsFocus == ShowsFocus::AlphabetRail) {
                if (!m_filteredShows.empty())
                    m_showsFocus = ShowsFocus::ShowsGrid;
                else if (!m_filteredAnime.empty())
                    m_showsFocus = ShowsFocus::AnimeGrid;
            } else if (m_showsFocus == ShowsFocus::ShowsGrid) {
                if (m_showSelected % 4 < 3) {
                    ++m_showSelected;
                } else if (!m_filteredAnime.empty()) {
                    m_showsFocus = ShowsFocus::AnimeGrid;
                    m_animeSelected = crossShowsGridIndex(
                        m_showSelected, m_filteredAnime.size(), true);
                }
            } else if (m_animeSelected % 4 < 3) {
                ++m_animeSelected;
            }
            clampShowsNavigation();
            return true;
        }
        if (activeTabNamed("Movies")) {
            if (m_movieRailFocused) {
                m_movieRailFocused = false;
            } else if (currentRow()) {
                m_activeCard = moveMovieGridCompact(
                    m_activeCard, static_cast<int>(currentRow()->items.size()),
                    0, 1);
            }
        } else {
            ++m_activeCard;
        }
        clampNavigation();
        return true;
    case Action::NextTab:
        activateTab((m_activeTab + 1) % static_cast<int>(m_tabs.size()));
        return true;
    case Action::PrevTab:
        activateTab(m_activeTab > 0
                         ? m_activeTab - 1
                         : static_cast<int>(m_tabs.size()) - 1);
        return true;
    case Action::Search:
        return false;
    case Action::ActionsMenu:
        if (m_logoutArmed) {
            m_logoutRequested = true;
        } else {
            m_logoutArmed = true;
            m_logoutTimer = 3000;
        }
        return true;
    case Action::Confirm: {
        if (activeTabNamed("Shows")) {
            if (m_showsFocus == ShowsFocus::AlphabetRail) {
                m_showsActiveLetter =
                    m_showsActiveLetter == m_showsAlphabetFocus
                        ? -1 : m_showsAlphabetFocus;
                resetMediaPaging();
                return true;
            }
            if (const MediaItem *show = showsSelectedItem()) {
                m_stack->push(std::make_unique<SeriesScreen>(
                    m_session, *show, m_downloads, m_libraryOffline,
                    std::vector<MediaItem>{}, presentationOffline(),
                    m_libraryCoordinator, m_libraryQuery));
                return true;
            }
            return true;
        }
        if (activeTabNamed("Movies") && m_movieRailFocused) {
            m_movieActiveLetter = m_movieActiveLetter == m_movieAlphabetFocus ? -1 : m_movieAlphabetFocus;
            resetMediaPaging();
            return true;
        }
        if (activeTabNamed("Shows")
            && m_showsFocus == ShowsFocus::AlphabetRail) {
            m_showsFocus = !m_filteredShows.empty()
                ? ShowsFocus::ShowsGrid
                : !m_filteredAnime.empty() ? ShowsFocus::AnimeGrid
                                            : ShowsFocus::AlphabetRail;
            return true;
        }
        const MediaItem *item = currentItem();
        if (item) {
            printf("[HomeScreen] Select: %s (%s)\n",
                   item->title.c_str(), item->type.c_str());
            if (item->type == "show") {
                m_stack->push(std::make_unique<SeriesScreen>(
                    m_session, *item, m_downloads, m_libraryOffline,
                    std::vector<MediaItem>{}, presentationOffline(),
                    m_libraryCoordinator, m_libraryQuery));
                return true;
            }
            if (item->type == "movie") {
                UiDiagnostics::Scope openScope("HomeScreen::open MovieDetailsScreen");
                std::unique_ptr<Screen> movieScreen;
                {
                    UiDiagnostics::Scope constructionScope("MovieDetailsScreen::construction");
                    std::shared_ptr<const DecodedImage> gridArtwork;
                    const auto artwork = m_rowArtwork.find(rowArtworkKey(*item));
                    if (artwork != m_rowArtwork.end()
                        && artwork->second.status == RowArtworkStatus::Loaded
                        && artwork->second.image
                        && !artwork->second.image->empty()) {
                        gridArtwork = artwork->second.image;
                    }
                    movieScreen = std::make_unique<MovieDetailsScreen>(
                        m_session, *item, m_downloads, std::move(gridArtwork));
                }
                m_stack->push(std::move(movieScreen));
                return true;
            }
            if (item->type == "episode") {
                if (!item->seriesId.empty() && !item->seasonId.empty()) {
                    MediaItem series;
                    series.id = item->seriesId;
                    series.title = item->seriesName;
                    series.type = "show";

                    MediaItem season;
                    season.id = item->seasonId;
                    season.seriesId = series.id;
                    season.type = "season";
                    season.indexNumber = item->parentIndexNumber;
                    if (item->parentIndexNumber > 0) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "Season %d",
                                      item->parentIndexNumber);
                        season.title = buf;
                    } else {
                        season.title = "Season";
                    }

                    m_stack->push(std::make_unique<EpisodeBrowserScreen>(
                        m_session, series, season, item->id, m_downloads,
                        m_libraryOffline, presentationOffline(),
                        m_libraryCoordinator, m_libraryQuery));
                    return true;
                }
                printf("[HomeScreen] Cannot open episode browser: "
                       "missing series/season context\n");
            }
        }
        return true;
    }
    case Action::Back:
        if (m_logoutArmed) {
            m_logoutArmed = false;
            m_logoutTimer = 0;
            return true;
        }
        if (activeTabNamed("Movies") && m_movieRailFocused) {
            m_movieRailFocused = false;
            return true;
        }
        return false;
    default:
        return false;
    }
}

void HomeScreen::applyPendingDown(MediaPageState &state)
{
    if (!state.pendingDown)
        return;
    state.pendingDown = false;
    if (state.type == "movie") {
        if (activeTabNamed("Movies")) {
            if (const MediaRow *row = currentRow())
                m_activeCard = moveMovieGridCompact(
                    m_activeCard, static_cast<int>(row->items.size()), 1, 0);
            clampNavigation();
        }
        return;
    }
    if (!activeTabNamed("Shows"))
        return;
    if (state.type == "anime")
        m_animeSelected = moveShowsGrid(
            m_animeSelected, m_filteredAnime.size(), 1, 0);
    else
        m_showSelected = moveShowsGrid(
            m_showSelected, m_filteredShows.size(), 1, 0);
    clampShowsNavigation();
}

} // namespace miyoofin
