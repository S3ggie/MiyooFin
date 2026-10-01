#include "HomeScreen.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../playback/OfflinePlaybackJournal.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"

namespace miyoofin {

void HomeScreen::startDownloadRefresh()
{
    if (!m_downloads) {
        m_downloadsState.snapshot = {};
        return;
    }
    if (m_downloadRefreshWorker.busy())
        return;
    std::shared_ptr<DownloadManager> downloads = m_downloads;
    const std::string journalPath = OfflinePlaybackJournal::path(
        "cache", LibraryCache::scopeKey(m_session.serverUrl, m_session.userId));
    const bool valid = m_session.valid();
    m_downloadRefreshWorker.start([this, downloads, journalPath, valid](const CancelToken&) {
        PerformanceTelemetry& telemetry = performanceTelemetry();
        telemetry.setWorkerActive(WorkerId::HomeDownloadRefresh, true);
        telemetry.setWorkerQueueDepth(WorkerId::HomeDownloadRefresh, 1);
        TelemetryTimer refreshTimer;
        DownloadSnapshot snapshot = downloads->snapshot();
        std::vector<OfflinePlaybackEntry> missing;
        if (valid) {
            std::vector<OfflinePlaybackEntry> journal;
            if (OfflinePlaybackJournal::load(journalPath, journal, nullptr)) {
                for (const auto& entry : journal) {
                    if (entry.serverMissing && !entry.conflict)
                        missing.push_back(entry);
                }
            }
        }
        m_downloadRefreshResult = std::move(snapshot);
        m_downloadJournalResult = std::move(missing);
        if (refreshTimer.active())
            (void)refreshTimer.elapsedUs();
        telemetry.addWorkerCompleted(WorkerId::HomeDownloadRefresh);
        telemetry.setWorkerActive(WorkerId::HomeDownloadRefresh, false);
        telemetry.setWorkerQueueDepth(WorkerId::HomeDownloadRefresh, 0);
    });
}

void HomeScreen::finishDownloadRefresh()
{
    UiDiagnostics::Scope scope("HomeScreen::publishDownloadSnapshot");
    // The worker was already reaped by refreshDownloads().
    m_downloadsState.publish(std::move(m_downloadRefreshResult),
                             std::move(m_downloadJournalResult));
    m_downloadRefreshTimer = 500;
}

} // namespace miyoofin
