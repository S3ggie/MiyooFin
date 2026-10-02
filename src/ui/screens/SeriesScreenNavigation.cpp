#include "SeriesScreen.hpp"
#include "../../net/WatchedSync.hpp"
#include "SeriesScreenInternal.hpp"
#include "../../app/ScreenStack.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/ArtworkUrl.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace miyoofin {

bool SeriesScreen::handleAction(Action action)
{
    if (m_watchedMenu.active()) {
        if (m_watchedMenu.handle(action) == ChoiceMenu::Result::Chosen) {
            const bool series = m_watchedMenu.chosen() == 1;
            bool allPlayed = !m_seasons.empty();
            if (series) {
                for (const MediaItem& s : m_seasons)
                    allPlayed = allPlayed && s.played;
            } else if (m_selectedSeason >= 0 && m_selectedSeason < (int)m_seasons.size()) {
                allPlayed = m_seasons[m_selectedSeason].played;
            }
            m_watchedTarget = !allPlayed;
            m_watchedConfirm = series ? 2 : 1;
        }
        return true;
    }
    if (m_watchedConfirm != 0) {
        if (action == Action::Confirm) {
            if (m_watchedConfirm == 2) {
                WatchedSync::instance().enqueue(m_series.id, m_watchedTarget);
                for (MediaItem& s : m_seasons)
                    s.played = m_watchedTarget;
                m_toast =
                    m_watchedTarget ? "Series marked as watched" : "Series marked as unwatched";
            } else if (m_selectedSeason >= 0 && m_selectedSeason < (int)m_seasons.size()) {
                MediaItem& s = m_seasons[m_selectedSeason];
                WatchedSync::instance().enqueue(s.id, m_watchedTarget);
                s.played = m_watchedTarget;
                m_toast =
                    m_watchedTarget ? "Season marked as watched" : "Season marked as unwatched";
            }
            m_toastLeftMs = 2500;
        }
        if (action == Action::Confirm || action == Action::Back)
            m_watchedConfirm = 0;
        return true;
    }
    if (action == Action::Menu && !m_seasons.empty() && !m_confirmDownload) {
        bool seriesPlayed = true;
        for (const MediaItem& s : m_seasons)
            seriesPlayed = seriesPlayed && s.played;
        const bool seasonPlayed = m_selectedSeason >= 0 &&
                                  m_selectedSeason < (int)m_seasons.size() &&
                                  m_seasons[m_selectedSeason].played;
        m_watchedMenu.open(
            "Watched",
            {seasonPlayed ? "Mark this season unwatched" : "Mark this season watched",
             seriesPlayed ? "Mark the whole series unwatched" : "Mark the whole series watched"});
        return true;
    }
    if (m_audioMenu.active()) {
        if (m_audioMenu.handle(action) == AudioChoiceMenu::Result::Picked && m_downloads)
            m_downloads->enqueue(m_audioMenu.takeItems());
        return true;
    }
    if (m_loadState == LoadState::Loading) {
        if (action == Action::Back) {
            m_stack->pop();
            return true;
        }
        return false;
    }

    if (m_loadState == LoadState::Error) {
        switch (action) {
        case Action::Back:
            m_stack->pop();
            return true;
        case Action::Confirm:
            fetchSeasons();
            return true;
        default:
            return false;
        }
    }

    // Ready — 2-column grid navigation
    int total = (int)m_seasons.size();
    int col = m_selectedSeason % GRID_COLS;

    if (m_confirmDownload) {
        if (action == Action::Back) {
            m_confirmDownload = false;
            return true;
        }
        if (action == Action::Confirm && m_downloads && m_planId) {
            auto p = m_downloads->planSnapshot(m_planId);
            if (p.state == DownloadPlanState::Ready && p.plan.canFit &&
                !m_audioMenu.begin(p.plan.items))
                m_downloads->enqueue(p.plan.items);
            m_confirmDownload = false;
        }
        return true;
    }
    // Y downloads the highlighted season; X expands and downloads the series.
    // Both requests are fully expanded and preflighted by DownloadManager.
    if ((action == Action::ActionsMenu || action == Action::Search) && m_downloads && total > 0) {
        bool whole = action == Action::Search;
        if (m_planId && m_planWholeSeries == whole &&
            m_downloads->planSnapshot(m_planId).state == DownloadPlanState::Ready)
            m_confirmDownload = true;
        else {
            m_planWholeSeries = whole;
            m_planId = whole
                           ? m_downloads->requestSeriesPlan(m_series)
                           : m_downloads->requestSeasonPlan(m_series, m_seasons[m_selectedSeason]);
        }
        return true;
    }

    switch (action) {
    case Action::Left:
        if (col > 0 && m_selectedSeason > 0) {
            m_selectedSeason--;
            clampGridScroll();
        }
        return true;

    case Action::Right:
        if (col < GRID_COLS - 1 && m_selectedSeason + 1 < total) {
            m_selectedSeason++;
            clampGridScroll();
        }
        return true;

    case Action::Up: {
        // Move up one grid row (same column)
        int newIdx = m_selectedSeason - GRID_COLS;
        if (newIdx >= 0) {
            m_selectedSeason = newIdx;
            clampGridScroll();
        }
        return true;
    }

    case Action::Down: {
        // Move down one grid row (same column if possible)
        int newIdx = m_selectedSeason + GRID_COLS;
        if (newIdx < total) {
            m_selectedSeason = newIdx;
            clampGridScroll();
        } else if (m_selectedSeason + 1 < total) {
            // Overshoot: land on the last item instead
            m_selectedSeason = total - 1;
            clampGridScroll();
        }
        return true;
    }

    case Action::Confirm: {
        const MediaItem& season = m_seasons[m_selectedSeason];
        printf("[SeriesScreen] Select season: %s index=%d\n", season.title.c_str(),
               season.indexNumber);
        m_stack->push(std::make_unique<EpisodeBrowserScreen>(
            m_session, m_series, season, "", m_downloads, m_networkOffline, m_downloadedOnly,
            m_libraryCoordinator, m_libraryQuery));
        return true;
    }

    case Action::Back:
        m_stack->pop();
        return true;

    case Action::PrevTab: {
        m_overviewScroll -= 3;
        if (m_overviewScroll < 0)
            m_overviewScroll = 0;
        return true;
    }

    case Action::NextTab: {
        auto lines = wrapOverview(m_series.overview.c_str(), META_WRAP);
        const int vis = overviewVisibleLines();
        int maxS = (int)lines.size() - vis;
        if (maxS < 0)
            maxS = 0;
        m_overviewScroll += 3;
        if (m_overviewScroll > maxS)
            m_overviewScroll = maxS;
        return true;
    }

    default:
        return false;
    }
}

}
