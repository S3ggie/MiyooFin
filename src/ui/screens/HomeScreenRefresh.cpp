#include "HomeScreen.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../playback/OfflinePlaybackJournal.hpp"
#include "../../app/UiDiagnostics.hpp"

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
        DownloadSnapshot snapshot=downloads->snapshot();
        std::vector<OfflinePlaybackEntry> missing;
        if(valid) {
            std::vector<OfflinePlaybackEntry> journal;
            if(OfflinePlaybackJournal::load(journalPath,journal,nullptr))
                for(const auto &entry:journal)if(entry.serverMissing&&!entry.conflict)missing.push_back(entry);
        }
        m_downloadRefreshResult=std::move(snapshot);
        m_downloadJournalResult=std::move(missing);
        m_downloadRefreshDone=true;
    });
}

void HomeScreen::finishDownloadRefresh()
{
    UiDiagnostics::Scope scope("HomeScreen::publishDownloadSnapshot");
    if(m_downloadRefreshThread.joinable())m_downloadRefreshThread.join();
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

void HomeScreen::startResumeRefresh()
{
    if (m_resumeRefreshThread.joinable())
        m_resumeRefreshThread.join();

    m_resumeRefreshDone = false;
    m_resumeRefreshInFlight = true;
    m_resumeRefreshSucceeded = false;
    m_resumeRefreshCacheSaved = false;
    m_resumeRefreshError.clear();
    m_resumeRefreshResult.clear();

    Session session=m_session;
    std::string url = session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid = m_session.userId;
    std::string devId = m_session.deviceId;

    LibrarySnapshot snapshot=m_cachedSnapshot;
    const std::string cachePath=LibraryCache::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
    m_resumeRefreshThread = std::thread([this, session, url, token, uid, devId, snapshot, cachePath]() mutable {
        std::vector<MediaItem> items;
        std::string error;
        if (RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getResumeItems(base, token, uid, devId, 12,items, error);},error)) {
            m_resumeRefreshResult = std::move(items);
            m_resumeRefreshSucceeded = true;
            snapshot.continueWatching=m_resumeRefreshResult;
            m_resumeRefreshCacheSaved=LibraryCache::save(cachePath,snapshot);
            startPosterSync(snapshot);
        } else {
            m_resumeRefreshError = error;
        }
        m_resumeRefreshDone = true;
    });
}

void HomeScreen::finishResumeRefresh()
{
    if (m_resumeRefreshThread.joinable())
        m_resumeRefreshThread.join();
    m_resumeRefreshDone = false;
    m_resumeRefreshInFlight = false;

    if (!m_resumeRefreshSucceeded) {
        printf("[HomeScreen] Continue Watching refresh failed: %s\n",
               m_resumeRefreshError.c_str());
    } else {
        updateContinueWatchingRow(m_tabs, m_resumeRefreshResult);
        m_cachedSnapshot.continueWatching = m_resumeRefreshResult;
        if (!m_resumeRefreshCacheSaved)
            printf("[HomeScreen] Continue Watching cache save failed\n");
        clampNavigation();
        printf("[HomeScreen] Continue Watching refreshed: %zu items\n",
               m_resumeRefreshResult.size());
    }

    if (m_resumeRefreshPending) {
        m_resumeRefreshPending = false;
        startResumeRefresh();
    }
}

} // namespace miyoofin
