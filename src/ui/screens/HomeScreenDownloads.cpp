#include "HomeScreen.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../app/UiDiagnostics.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../download/DownloadSupport.hpp"
#include "../../playback/OfflinePlaybackJournal.hpp"
#include "../../playback/PlaybackRequest.hpp"

namespace miyoofin {

void HomeScreen::refreshDownloads()
{
    if (m_downloadRefreshDone) finishDownloadRefresh();
    if (!m_downloadRefreshInFlight && m_downloadRefreshTimer == 0)
        startDownloadRefresh();
}

bool HomeScreen::handleDownloadsAction(Action action)
{
    const auto &rows=m_downloadHierarchy.visible;
    if (action == Action::Up || action == Action::Down) {
        m_downloadSelected += action == Action::Up ? -1 : 1;
        m_downloadSelected = clampDownloadSelection(m_downloadSelected, (int)rows.size());
        m_downloadScroll = clampDownloadScroll(m_downloadSelected, (int)rows.size(), m_downloadScroll, 5);
        m_downloadSelectedId = rows.empty() ? "" : rows[m_downloadSelected].id;
        m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear();
        return true;
    }
    if (rows.empty()) {
        if (action == Action::Search && !m_missingJournalEntries.empty()) {
            const std::string &id=m_missingJournalEntries.front().itemId;
            if (m_journalDiscardConfirmId == id) {
                const std::string path=OfflinePlaybackJournal::path("cache", LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
                OfflinePlaybackJournal::discardMissing(path,id,nullptr);
                m_journalDiscardConfirmId.clear(); refreshDownloads();
            } else m_journalDiscardConfirmId=id;
            return true;
        }
        return action == Action::Confirm || action == Action::ActionsMenu;
    }
    const DownloadHierarchyRow &row=rows[m_downloadSelected];
    if (action == Action::Back && !m_downloadConfirmId.empty()) {
        m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear(); return true;
    }
    if ((row.kind==DownloadHierarchyRowKind::Series || row.kind==DownloadHierarchyRowKind::Season) && action==Action::Confirm) {
        if(row.expanded) m_downloadExpanded.erase(row.id); else m_downloadExpanded.insert(row.id);
        m_downloadHierarchy=buildDownloadHierarchy(m_downloadSnapshot,m_downloadExpanded);
        m_downloadSelected=downloadHierarchySelection(m_downloadHierarchy.visible,row.id,m_downloadSelected);
        m_downloadScroll=clampDownloadScroll(m_downloadSelected,(int)m_downloadHierarchy.visible.size(),m_downloadScroll,5);
        m_downloadSelectedId=row.id; m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear(); return true;
    }
    // Y belongs to Downloads even when the selected row has no removal
    // operation (such as Series/Season parents or malformed leaves).  Letting
    // it fall through would trigger Home's global logout action.
    if (action == Action::ActionsMenu && downloadHierarchyConsumesActionsMenu(row)) {
        if (row.item && downloadCanRemove(*row.item)) {
            const DownloadItem &item=*row.item;
            if (m_downloadConfirmId == row.id) {
                m_downloads->erase(item.itemId, nullptr);
                m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear();
            } else { m_downloadConfirmId = row.id; m_downloadConfirmItemIds={item.itemId}; }
        } else if (downloadHierarchyIsBulkRemovalParent(row)) {
            if (downloadHierarchyBulkRemovalConfirmed(m_downloadConfirmId,row)) {
                // DownloadManager::erase cancels an active transfer before it
                // removes its local manifest/segments and store entry.
                for (const std::string &itemId : m_downloadConfirmItemIds)
                    m_downloads->erase(itemId, nullptr);
                m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear();
            } else {
                m_downloadConfirmItemIds=downloadHierarchyBulkRemovalItemIds(row,m_downloadSnapshot);
                if (!m_downloadConfirmItemIds.empty()) m_downloadConfirmId=row.id;
            }
        }
        return true;
    }
    if (!row.item) return action==Action::Confirm;
    const DownloadItem &item=*row.item;
    if (action == Action::Search && item.state == DownloadState::UpdateAvailable) { m_downloads->redownload(item.itemId); return true; }
    if (action == Action::Search && !m_missingJournalEntries.empty()) {
        const std::string &id=m_missingJournalEntries.front().itemId;
        if (m_journalDiscardConfirmId == id) {
            const std::string path=OfflinePlaybackJournal::path("cache", LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
            OfflinePlaybackJournal::discardMissing(path,id,nullptr);
            m_journalDiscardConfirmId.clear();
            refreshDownloads();
        } else m_journalDiscardConfirmId=id;
        return true;
    }
    if (action != Action::Confirm) return false;
    switch (downloadPrimaryControl(item.state)) {
    case DownloadPrimaryControl::Pause: m_downloads->pause(item.itemId); return true;
    case DownloadPrimaryControl::Resume: m_downloads->resume(item.itemId); return true;
    case DownloadPrimaryControl::Retry: m_downloads->retry(item.itemId); return true;
    case DownloadPrimaryControl::Play: {
        std::string error;
        const char *type = item.itemType == "episode" ? "episode" : "movie";
        if (PlaybackRequest::writeWithSourceTo(PlaybackRequest::defaultPath(), item.itemId, type,
                item.playbackPositionTicks, "local", m_downloads->scope(), error))
            m_stack->requestExternalPlayback();
        return true;
    }
    default: return true;
    }
}

} // namespace miyoofin
