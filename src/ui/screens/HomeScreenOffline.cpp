#include "HomeScreen.hpp"

namespace miyoofin {

void HomeScreen::prepareOfflineProjection()
{
    // Offline preparation is performed by HomeLibraryController on its worker.
}

void HomeScreen::applyOfflineProjection()
{
    const std::vector<TabData> previous = m_tabs;
    const int selected = m_activeTab;
    if (!m_haveOfflineSnapshotCache)
        return;
    m_tabs = offlineTabsFromSnapshot(m_offlineSnapshotCache);
    m_activeTab = transitionTabIndex(previous, selected, m_tabs);
    m_offlineSnapshot = m_offlineSnapshotCache;
    resetMediaPaging();
    clampNavigation();
}

HomeScreen::OfflineSnapshotSignature
HomeScreen::computeOfflineSignature(const DownloadSnapshot& downloads,
                                    std::uint64_t catalogGeneration)
{
    return HomeLibraryController::computeOfflineSignature(downloads, catalogGeneration);
}

bool HomeScreen::tryApplyCachedOfflineSnapshot()
{
    if (!m_haveCachedSnapshot || !m_haveOfflineSignature || !m_haveOfflineSnapshotCache ||
        !m_downloads)
        return false;
    const DownloadSnapshot downloads = m_downloads->snapshot();
    const auto signature = computeOfflineSignature(downloads, committedCatalogGeneration());
    if (signature != m_offlineSignature)
        return false;
    m_cachedSnapshot = m_offlineSnapshotCache;
    m_haveCachedSnapshot = true;
    m_libraryOffline = true;
    applyPresentationProjection();
    return true;
}

} // namespace miyoofin
