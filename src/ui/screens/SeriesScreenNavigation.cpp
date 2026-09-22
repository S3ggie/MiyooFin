#include "SeriesScreen.hpp"
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
            if (p.state == DownloadPlanState::Ready && p.plan.canFit)
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
        int overviewStartY = META_Y + BitmapFont::GLYPH_H + 2;
        if (m_series.year > 0 || !m_series.genre.empty())
            overviewStartY += BitmapFont::GLYPH_H + 2;
        int vis = ((FB_H - BOTTOM_H) - overviewStartY) / BitmapFont::GLYPH_H;
        if (vis < 1)
            vis = 1;
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
