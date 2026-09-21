#include "HomeScreen.hpp"
#include "../../playback/OfflineLibraryProjection.hpp"
#include "../../library/OfflineLibraryQuery.hpp"

namespace miyoofin {

void HomeScreen::prepareOfflineProjection()
{
    // The legacy catalog is not a runtime authority in offline mode; an empty
    // catalog lets the projection synthesize only the downloaded branches.
    OfflineCatalogSnapshot catalog;
    OfflineLibraryProjection projection(
        m_cachedSnapshot, catalog,
        m_downloads ? m_downloads->snapshot() : DownloadSnapshot{});

    m_fetchOfflineTabs = offlineTabsFromSnapshot(m_cachedSnapshot);
    m_fetchOfflineMovies.clear();
    m_fetchOfflineSnapshot = m_cachedSnapshot;

    // Keep the tab skeleton but defer population until media pages are fetched.
    for (auto &tab : m_fetchOfflineTabs) {
        if (tab.name == "Movies") tab.rows = {{"Movies", {}}};
        if (tab.name == "Shows") tab.rows = {{"Shows", {}}};
    }

    // Filter each show view to items that are playable or have offline seasons.
    for (auto &view : m_fetchOfflineSnapshot.shows) {
        std::vector<MediaItem> filtered;
        for (const auto &item : view.items) {
            if (projection.playable(item.id)
                || !projection.seasons(item.id).empty()) {
                filtered.push_back(item);
            }
        }
        view.items = std::move(filtered);
    }

    m_fetchOfflinePrepared = true;
}

void HomeScreen::applyOfflineProjection()
{
    const std::vector<TabData> previous = m_tabs;
    const int selected = m_activeTab;
    if (!m_fetchOfflinePrepared) return;

    m_tabs = std::move(m_fetchOfflineTabs);
    m_activeTab = transitionTabIndex(previous, selected, m_tabs);
    m_offlineSnapshot = std::move(m_fetchOfflineSnapshot);
    m_fetchOfflinePrepared = false;
    resetMediaPaging();
    clampNavigation();
}

HomeScreen::OfflineSnapshotSignature HomeScreen::computeOfflineSignature(
    const DownloadSnapshot &downloads, std::uint64_t catalogGeneration)
{
    OfflineSnapshotSignature sig;
    sig.localBytes = downloads.localBytes;
    sig.reservedBytes = downloads.reservedBytes;
    sig.catalogGeneration = catalogGeneration;
    for (const auto &item : downloads.items) {
        if (OfflineLibraryQuery::isAvailable(item.state)) {
            sig.availableItemIds.insert(item.itemId);
            sig.totalDownloadedBytes += item.downloadedBytes;
            ++sig.availableItemCount;
        }
    }
    return sig;
}

bool HomeScreen::tryApplyCachedOfflineSnapshot()
{
    if (!m_haveCachedSnapshot || !m_haveOfflineSignature
        || !m_haveOfflineSnapshotCache || !m_downloads)
        return false;
    const DownloadSnapshot downloads = m_downloads->snapshot();
    // Cheap atomic load — no database query on the UI thread.  A catalog
    // sync that changed only metadata (titles, artwork tags, playback
    // state) advances the generation, so the stale cache misses here and
    // the snapshot is rebuilt instead of showing stale metadata forever.
    const auto sig = computeOfflineSignature(
        downloads, committedCatalogGeneration());
    if (sig != m_offlineSignature)
        return false;
    // Cache hit — apply the snapshot directly on the UI thread.
    // Mirrors applyPresentationProjection() but uses the cached offline
    // snapshot instead of rebuilding from downloads + metadata.
    m_cachedSnapshot = m_offlineSnapshotCache;
    m_haveCachedSnapshot = true;
    m_libraryOffline = true;
    applyPresentationProjection();
    return true;
}

} // namespace miyoofin
