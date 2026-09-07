#include "EpisodeBrowserScreen.hpp"

namespace miyoofin {

void EpisodeBrowserScreen::handleDownloadConfirmation(Action action)
{
    if(action==Action::Confirm && m_downloads && m_planId){auto p=m_downloads->planSnapshot(m_planId);if(p.state==DownloadPlanState::Ready&&p.plan.canFit)m_downloads->enqueue(p.plan.items);m_confirmDownload=false;}
}

void EpisodeBrowserScreen::requestSeasonDownloadPlan()
{
    if(m_planId && m_planIsSeason && m_downloads->planSnapshot(m_planId).state==DownloadPlanState::Ready) m_confirmDownload=true;
    else {m_confirmDownload=false; m_planIsSeason=true; m_planId=m_downloads->requestPlan(m_episodes);}
}

void EpisodeBrowserScreen::handleDownloadButtonAction()
{
    if (m_confirmDownload) { if(m_downloads&&m_planId){auto p=m_downloads->planSnapshot(m_planId);if(p.state==DownloadPlanState::Ready&&p.plan.canFit)m_downloads->enqueue(p.plan.items);}m_confirmDownload=false; }
    else if(m_downloads && m_selectedEpisode>=0 && m_selectedEpisode<(int)m_episodes.size()) { bool season=m_actionBtn==ActionButton::DownloadSeason; if(m_planId && m_planIsSeason==season && m_downloads->planSnapshot(m_planId).state==DownloadPlanState::Ready) m_confirmDownload=true; else {m_planIsSeason=season;m_planId=m_downloads->requestPlan(season?m_episodes:std::vector<MediaItem>{m_episodes[m_selectedEpisode]});} }
}

} // namespace miyoofin
