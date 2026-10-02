#include "HomeScreen.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../download/DownloadSupport.hpp"
#include "../../playback/OfflinePlaybackJournal.hpp"
#include "../../playback/PlaybackRequest.hpp"

namespace miyoofin {

void HomeScreen::refreshDownloads()
{
    if (m_downloadRefreshWorker.reap())
        finishDownloadRefresh();
    if (!m_downloadRefreshWorker.busy() && m_downloadRefreshTimer == 0)
        startDownloadRefresh();
}

bool HomeScreen::handleDownloadsAction(Action action)
{
    HomeDownloadsState& dl = m_downloadsState;
    const auto& rows = dl.hierarchy.visible;
    // Discards the missing playback-journal entry when this press confirms it.
    auto pressJournalDiscard = [this, &dl]() {
        const std::string id = dl.pressJournalDiscard();
        if (id.empty())
            return;
        const std::string path = OfflinePlaybackJournal::path(
            "cache", LibraryCache::scopeKey(m_session.serverUrl, m_session.userId));
        OfflinePlaybackJournal::discardMissing(path, id, nullptr);
        refreshDownloads();
    };
    if (action == Action::Up || action == Action::Down) {
        dl.moveSelection(action == Action::Up ? -1 : 1);
        return true;
    }
    if (rows.empty()) {
        if (action == Action::Search && !dl.missingJournal.empty()) {
            pressJournalDiscard();
            return true;
        }
        return action == Action::Confirm || action == Action::ActionsMenu;
    }
    const DownloadHierarchyRow& row = rows[dl.selected];
    if (action == Action::Back && !dl.confirmId.empty()) {
        dl.clearConfirm();
        return true;
    }
    if (action == Action::Confirm && dl.toggleSelectedParent())
        return true;
    // Y belongs to Downloads even when the selected row has no removal
    // operation (such as Series/Season parents or malformed leaves).  Letting
    // it fall through would trigger Home's global logout action.
    if (action == Action::ActionsMenu && downloadHierarchyConsumesActionsMenu(row)) {
        // DownloadManager::erase cancels an active transfer before it removes
        // its local manifest/segments and store entry.
        for (const std::string& itemId : dl.pressRemoval())
            m_downloads->erase(itemId, nullptr);
        return true;
    }
    if (!row.item)
        return action == Action::Confirm;
    const DownloadItem& item = *row.item;
    if (action == Action::Search && item.state == DownloadState::UpdateAvailable) {
        m_downloads->redownload(item.itemId);
        return true;
    }
    if (action == Action::Search && !dl.missingJournal.empty()) {
        pressJournalDiscard();
        return true;
    }
    if (action != Action::Confirm)
        return false;
    switch (downloadPrimaryControl(item.state)) {
    case DownloadPrimaryControl::Pause:
        m_downloads->pause(item.itemId);
        return true;
    case DownloadPrimaryControl::Resume:
        m_downloads->resume(item.itemId);
        return true;
    case DownloadPrimaryControl::Retry:
        m_downloads->retry(item.itemId);
        return true;
    case DownloadPrimaryControl::Play: {
        std::string error;
        const char* type = item.itemType == "episode" ? "episode" : "movie";
        if (PlaybackRequest::writeWithSourceAndDurationTo(
                PlaybackRequest::defaultPath(), item.itemId, type, item.playbackPositionTicks,
                "local", m_downloads->scope(), item.runtimeTicks, error)) {
            m_stack->requestExternalPlayback(ScreenStack::ExternalPlaybackSource::Local);
        }
        return true;
    }
    default:
        return true;
    }
}

} // namespace miyoofin
