#include "HomeScreen.hpp"
#include "SeriesScreen.hpp"
#include "MovieDetailsScreen.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../MovieTitle.hpp"
#include "../ShowsBrowser.hpp"
#include "../../app/UiDiagnostics.hpp"
#include "../../app/ScreenStack.hpp"
#include <algorithm>
#include <cstdio>
#include <memory>

namespace miyoofin {

static constexpr int VISIBLE_ROWS = 3;
static constexpr int SETTINGS_VISIBLE_ROWS = 6;
static constexpr int CARD_GAP = 6;
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;

static int clampMovieGridScrollCompact(int selected, int count, int currentScroll)
{
    if (count <= 0) return 0;
    selected = std::max(0, std::min(selected, count - 1));
    const int selectedRow = selected / MOVIE_GRID_COLUMNS;
    const int lastRow = (count - 1) / MOVIE_GRID_COLUMNS;
    const int maxScroll = std::max(0, lastRow - MOVIE_GRID_ROWS + 1);

    currentScroll = std::max(0, std::min(currentScroll, maxScroll));
    if (selectedRow < currentScroll) currentScroll = selectedRow;
    if (selectedRow >= currentScroll + MOVIE_GRID_ROWS)
        currentScroll = selectedRow - MOVIE_GRID_ROWS + 1;
    return std::max(0, std::min(currentScroll, maxScroll));
}

void HomeScreen::refreshMovieFilter()
{
    const int movies=tabIndex("Movies"); if (movies < 0) return;
    std::vector<MediaItem> displayed;
    for (const auto &item : m_movieWindow) {
        if (movieMatchesAlphabetFilter(item.title, m_movieActiveLetter))
            displayed.push_back(item);
    }
    m_tabs[movies].rows = {{"Movies", std::move(displayed)}};
    m_activeRow = 0; m_activeCard = 0; m_rowScroll = 0; m_cardScroll = 0;
    m_selectedArtwork = {}; m_selectedArtworkId.clear(); m_selectedArtworkAttempted = false;
}

void HomeScreen::refreshShowsFilter()
{
    const std::string selectedId = m_showsPreviewId;
    m_filteredShows.clear();
    m_filteredAnime.clear();
    for (const auto &item : m_showWindow)
        if (matchesAlphabetFilter(item.title, m_showsActiveLetter))
            m_filteredShows.push_back(item);
    for (const auto &item : m_animeWindow)
        if (matchesAlphabetFilter(item.title, m_showsActiveLetter))
            m_filteredAnime.push_back(item);
    m_showSelected = m_animeSelected = m_showScroll = m_animeScroll = 0;
    m_showsFocus = !m_filteredShows.empty() ? ShowsFocus::ShowsGrid
        : !m_filteredAnime.empty() ? ShowsFocus::AnimeGrid
        : ShowsFocus::AlphabetRail;
    auto restore = [&](const std::vector<MediaItem> &items, int &selected) {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            if (items[i].id == selectedId) {
                selected = i;
                return true;
            }
        }
        return false;
    };
    if (!restore(m_filteredShows, m_showSelected))
        restore(m_filteredAnime, m_animeSelected);
    if (const MediaItem *item = showsSelectedItem())
        m_showsPreviewId = item->id;
}
const MediaItem *HomeScreen::showsSelectedItem() const { const std::vector<MediaItem>*v=m_showsFocus==ShowsFocus::AnimeGrid?&m_filteredAnime:&m_filteredShows;int n=m_showsFocus==ShowsFocus::AnimeGrid?m_animeSelected:m_showSelected;if(n>=0&&n<(int)v->size())return &(*v)[n];for(const auto&i:m_filteredShows)if(i.id==m_showsPreviewId)return &i;for(const auto&i:m_filteredAnime)if(i.id==m_showsPreviewId)return &i;return nullptr; }
void HomeScreen::clampShowsNavigation() { if(!m_filteredShows.empty()){m_showSelected=std::max(0,std::min(m_showSelected,(int)m_filteredShows.size()-1));m_showScroll=clampShowsGridScroll(m_showSelected,m_filteredShows.size(),m_showScroll);}else m_showSelected=m_showScroll=0;if(!m_filteredAnime.empty()){m_animeSelected=std::max(0,std::min(m_animeSelected,(int)m_filteredAnime.size()-1));m_animeScroll=clampShowsGridScroll(m_animeSelected,m_filteredAnime.size(),m_animeScroll);}else m_animeSelected=m_animeScroll=0; }

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

const char *HomeScreen::diagnosticTabName() const { return currentTab().name.empty() ? "other" : currentTab().name.c_str(); }
bool HomeScreen::activeTabNamed(const char *name) const { return currentTab().name==name; }
int HomeScreen::tabIndex(const char *name) const { for(int i=0;i<(int)m_tabs.size();++i) if(m_tabs[i].name==name) return i; return -1; }

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

void HomeScreen::clampNavigation()
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) {
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
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
        return;
    }
    if (activeTabNamed("Shows")) { clampShowsNavigation(); return; }
    if (m_activeRow < 0) m_activeRow = 0;
    if (m_activeRow >= (int)rows.size()) m_activeRow = (int)rows.size() - 1;
    const auto &items = rows[m_activeRow].items;
    if (items.empty()) {
        m_activeCard = 0; m_cardScroll = 0;
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
            m_logoutArmed = true; m_logoutTimer = 3000; return true;
        }
        return false;
    }

    // Error: allow retry (A) and logout (Y)
    if (m_loadState == LoadState::Error) {
        if (action == Action::Confirm) {
            m_loadState = LoadState::Loading;
            m_fetchDone = false; m_fetchError.clear(); m_fetchResult.clear();
            startFetch(); return true;
        }
        if (action == Action::ActionsMenu && !m_logoutArmed) {
            m_logoutArmed = true; m_logoutTimer = 3000; return true;
        }
        return false;
    }

    // Ready: normal navigation
    if (activeTabNamed("Downloads") && handleDownloadsAction(action)) return true;
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
            if (action == Action::Up && m_settingsSelected > 0) --m_settingsSelected;
            else if (action == Action::Down && m_settingsSelected < settingsRowCount(m_session)-1) ++m_settingsSelected;
            if (m_settingsSelected != previous)
                m_settingsConfirmation = SettingsConfirmation::None;
            m_settingsScroll=std::max(0,std::min(m_settingsSelected,settingsRowCount(m_session)-SETTINGS_VISIBLE_ROWS));
            return true;
        }
        if (action == Action::Confirm) {
            switch (settingsRowAction(m_settingsSelected,m_session)) {
            case SettingsRowAction::OfflineMode:
                m_session.manualOfflineMode = !m_session.manualOfflineMode;
                m_session.save();
                if (m_session.manualOfflineMode) applyPresentationProjection();
                else if (!m_libraryOffline) restoreOnlinePresentation();
                return true;
            case SettingsRowAction::LocalAddress:
                m_localAddressRequested = true;
                return true;
            case SettingsRowAction::PublicAddress:
                m_publicAddressRequested = true;
                return true;
            case SettingsRowAction::ChangeServer:
            case SettingsRowAction::Logout: {
                const SettingsConfirmation requested = settingsRowAction(m_settingsSelected,m_session) == SettingsRowAction::ChangeServer
                    ? SettingsConfirmation::ChangeServer : SettingsConfirmation::Logout;
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
            case SettingsRowAction::None:
                break;
            }
        }
        // Settings owns its account actions; do not let Y or X trigger Home actions.
        if (action == Action::ActionsMenu || action == Action::Search) return true;
        if (action != Action::NextTab && action != Action::PrevTab) return true;
        m_settingsScroll=std::max(0,std::min(m_settingsSelected,settingsRowCount(m_session)-SETTINGS_VISIBLE_ROWS));
    }
    switch (action) {
    case Action::Up:
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail){if(m_showsAlphabetFocus>0)--m_showsAlphabetFocus;}else if(m_showsFocus==ShowsFocus::ShowsGrid)m_showSelected=moveShowsGrid(m_showSelected,m_filteredShows.size(),-1,0);else m_animeSelected=moveShowsGrid(m_animeSelected,m_filteredAnime.size(),-1,0);clampShowsNavigation();return true;}
        if (activeTabNamed("Movies")) { if (m_movieRailFocused) { if (m_movieAlphabetFocus > 0) --m_movieAlphabetFocus; } else if (currentRow()) m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),-1,0); }
        else m_activeRow--;
        clampNavigation(); return true;
    case Action::Down:
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail){if(m_showsAlphabetFocus<25)++m_showsAlphabetFocus;}else if(m_showsFocus==ShowsFocus::ShowsGrid)m_showSelected=moveShowsGrid(m_showSelected,m_filteredShows.size(),1,0);else m_animeSelected=moveShowsGrid(m_animeSelected,m_filteredAnime.size(),1,0);clampShowsNavigation();return true;}
        if (activeTabNamed("Movies")) { if (m_movieRailFocused) { if (m_movieAlphabetFocus < 25) ++m_movieAlphabetFocus; } else if (currentRow()) m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),1,0); }
        else m_activeRow++;
        clampNavigation(); return true;
    case Action::Left:
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail)return true;if(m_showsFocus==ShowsFocus::ShowsGrid){if(m_showSelected%4)m_showSelected--;else{m_showsFocus=ShowsFocus::AlphabetRail;m_showsAlphabetFocus=m_showsActiveLetter>=0?m_showsActiveLetter:alphabetFocus(m_filteredShows[m_showSelected].title);}}else{if(m_animeSelected%4)m_animeSelected--;else if(!m_filteredShows.empty()){m_showsFocus=ShowsFocus::ShowsGrid;m_showSelected=crossShowsGridIndex(m_animeSelected,m_filteredShows.size(),false);}else m_showsFocus=ShowsFocus::AlphabetRail;}clampShowsNavigation();return true;}
        if (activeTabNamed("Movies")) {
            if (!m_movieRailFocused && (!currentRow() || m_activeCard % MOVIE_GRID_COLUMNS == 0)) {
                m_movieRailFocused = true;
                m_movieAlphabetFocus = m_movieActiveLetter >= 0
                    ? m_movieActiveLetter
                    : movieAlphabetFocus(currentItem() ? currentItem()->title : std::string());
            } else if (!m_movieRailFocused && currentRow()) {
                m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),0,-1);
            }
        }
        else m_activeCard--;
        clampNavigation(); return true;
    case Action::Right:
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail){if(!m_filteredShows.empty())m_showsFocus=ShowsFocus::ShowsGrid;else if(!m_filteredAnime.empty())m_showsFocus=ShowsFocus::AnimeGrid;}else if(m_showsFocus==ShowsFocus::ShowsGrid){if(m_showSelected%4<3)m_showSelected++;else if(!m_filteredAnime.empty()){m_showsFocus=ShowsFocus::AnimeGrid;m_animeSelected=crossShowsGridIndex(m_showSelected,m_filteredAnime.size(),true);}}else if(m_animeSelected%4<3)m_animeSelected++;clampShowsNavigation();return true;}
        if (activeTabNamed("Movies")) { if (m_movieRailFocused) m_movieRailFocused=false; else if (currentRow()) m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),0,1); }
        else m_activeCard++;
        clampNavigation(); return true;
    case Action::NextTab:
        m_activeTab = (m_activeTab + 1) % (int)m_tabs.size();
        m_movieRailFocused = false;
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); if (activeTabNamed("Movies") || activeTabNamed("Shows")) requestFetch(SDL_GetTicks()); if((activeTabNamed("Downloads")||activeTabNamed("Settings"))&&m_downloads) { if(activeTabNamed("Downloads"))m_downloads->requestReconcile(); refreshDownloads(); } return true;
    case Action::PrevTab:
        m_activeTab--;
        if (m_activeTab < 0) m_activeTab = (int)m_tabs.size() - 1;
        m_movieRailFocused = false;
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); if (activeTabNamed("Movies") || activeTabNamed("Shows")) requestFetch(SDL_GetTicks()); if((activeTabNamed("Downloads")||activeTabNamed("Settings"))&&m_downloads) { if(activeTabNamed("Downloads"))m_downloads->requestReconcile(); refreshDownloads(); } return true;
    case Action::Search: return false;
    case Action::ActionsMenu:
        if (m_logoutArmed) { m_logoutRequested = true; }
        else { m_logoutArmed = true; m_logoutTimer = 3000; }
        return true;
    case Action::Confirm: {
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail){m_showsActiveLetter=m_showsActiveLetter==m_showsAlphabetFocus?-1:m_showsAlphabetFocus;resetMediaPaging();return true;}if(const MediaItem*i=showsSelectedItem()){m_stack->push(std::make_unique<SeriesScreen>(m_session,*i,m_downloads,m_libraryOffline,std::vector<MediaItem>{},presentationOffline(),m_catalogDb,m_catalogMetadata.scopeEpoch));return true;}return true;}
        if (activeTabNamed("Movies") && m_movieRailFocused) {
            m_movieActiveLetter = m_movieActiveLetter == m_movieAlphabetFocus ? -1 : m_movieAlphabetFocus;
            resetMediaPaging();
            return true;
        }
        if(activeTabNamed("Shows")&&m_showsFocus==ShowsFocus::AlphabetRail){m_showsFocus=!m_filteredShows.empty()?ShowsFocus::ShowsGrid:!m_filteredAnime.empty()?ShowsFocus::AnimeGrid:ShowsFocus::AlphabetRail;return true;}
        const MediaItem *item = currentItem();
        if (item) {
            printf("[HomeScreen] Select: %s (%s)\n", item->title.c_str(), item->type.c_str());
            if (item->type == "show") {
                m_stack->push(std::make_unique<SeriesScreen>(m_session, *item,m_downloads,m_libraryOffline,std::vector<MediaItem>{},presentationOffline(),m_catalogDb,m_catalogMetadata.scopeEpoch));
                return true;
            }
            if (item->type == "movie") {
                UiDiagnostics::Scope openScope("HomeScreen::open MovieDetailsScreen");
                std::unique_ptr<Screen> movieScreen;
                {
                    UiDiagnostics::Scope constructionScope("MovieDetailsScreen::construction");
                    std::shared_ptr<const DecodedImage> gridArtwork;
                    const auto artwork=m_rowArtwork.find(rowArtworkKey(*item));
                    if(artwork!=m_rowArtwork.end() && artwork->second.status==RowArtworkStatus::Loaded
                        && artwork->second.image && !artwork->second.image->empty())
                        gridArtwork=artwork->second.image;
                    movieScreen=std::make_unique<MovieDetailsScreen>(m_session,*item,m_downloads,std::move(gridArtwork));
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
                    season.type = "season";
                    season.indexNumber = item->parentIndexNumber;
                    if (item->parentIndexNumber > 0) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "Season %d", item->parentIndexNumber);
                        season.title = buf;
                    } else {
                        season.title = "Season";
                    }

                    m_stack->push(std::make_unique<EpisodeBrowserScreen>(
                        m_session, series, season, item->id, m_downloads,
                        m_libraryOffline, presentationOffline(), m_catalogDb,
                        m_catalogMetadata.scopeEpoch));
                    return true;
                }
                printf("[HomeScreen] Cannot open episode browser: missing series/season context\n");
            }
        }
        return true;
    }
    case Action::Back:
        if (m_logoutArmed) { m_logoutArmed = false; m_logoutTimer = 0; return true; }
        if (activeTabNamed("Movies") && m_movieRailFocused) {
            m_movieRailFocused = false;
            return true;
        }
        return false;
    default: return false;
    }
}

} // namespace miyoofin
