#include "HomeScreen.hpp"
#include "../../playback/OfflineLibraryProjection.hpp"

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

} // namespace miyoofin
