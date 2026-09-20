#include "HomeScreen.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../playback/OfflinePlaybackJournal.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"

namespace miyoofin {

void HomeScreen::startDownloadRefresh()
{
    if (!m_downloads) { m_downloadSnapshot = {}; return; }
    if (m_downloadRefreshInFlight) return;
    if (m_downloadRefreshThread.joinable()) m_downloadRefreshThread.join();
    m_downloadRefreshDone=false;m_downloadRefreshInFlight=true;
    std::shared_ptr<DownloadManager> downloads=m_downloads;
    const std::string journalPath=OfflinePlaybackJournal::path("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
    const bool valid=m_session.valid();
    m_downloadRefreshThread=std::thread([this,downloads,journalPath,valid] {
        PerformanceTelemetry &telemetry=performanceTelemetry();
        telemetry.setWorkerActive(WorkerId::HomeDownloadRefresh, true);
        telemetry.setWorkerQueueDepth(WorkerId::HomeDownloadRefresh, 1);
        TelemetryTimer refreshTimer;
        DownloadSnapshot snapshot=downloads->snapshot();
        std::vector<OfflinePlaybackEntry> missing;
        if(valid) {
            std::vector<OfflinePlaybackEntry> journal;
            if(OfflinePlaybackJournal::load(journalPath,journal,nullptr))
                for(const auto &entry:journal)if(entry.serverMissing&&!entry.conflict)missing.push_back(entry);
        }
        m_downloadRefreshResult=std::move(snapshot);
        m_downloadJournalResult=std::move(missing);
        if (refreshTimer.active()) (void)refreshTimer.elapsedUs();
        telemetry.addWorkerCompleted(WorkerId::HomeDownloadRefresh);
        telemetry.setWorkerActive(WorkerId::HomeDownloadRefresh, false);
        telemetry.setWorkerQueueDepth(WorkerId::HomeDownloadRefresh, 0);
        m_downloadRefreshDone=true;
    });
}

void HomeScreen::finishDownloadRefresh()
{
    UiDiagnostics::Scope scope("HomeScreen::publishDownloadSnapshot");
    // Thread join deferred to next startDownloadRefresh() or joinAllWorkers().
    m_downloadRefreshDone=false;m_downloadRefreshInFlight=false;
    m_downloadSnapshot=std::move(m_downloadRefreshResult);
    m_missingJournalEntries=std::move(m_downloadJournalResult);
    m_downloadRefreshTimer=500;
    m_downloadHierarchy=buildDownloadHierarchy(m_downloadSnapshot,m_downloadExpanded);
    const auto &rows=m_downloadHierarchy.visible;
    m_downloadSelected=downloadHierarchySelection(rows,m_downloadSelectedId,m_downloadSelected);
    m_downloadScroll=clampDownloadScroll(m_downloadSelected,(int)rows.size(),m_downloadScroll,5);
    m_downloadSelectedId=rows.empty()?"":rows[m_downloadSelected].id;
    if (!m_downloadConfirmId.empty() && m_downloadConfirmId != m_downloadSelectedId) {
        m_downloadConfirmId.clear();
        m_downloadConfirmItemIds.clear();
    }
    if (!m_journalDiscardConfirmId.empty() && (m_missingJournalEntries.empty() || m_missingJournalEntries.front().itemId != m_journalDiscardConfirmId))
        m_journalDiscardConfirmId.clear();
}

} // namespace miyoofin
