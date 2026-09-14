#include "HomeScreen.hpp"
#include "../../playback/OfflineLibraryProjection.hpp"

namespace miyoofin {

void HomeScreen::prepareOfflineProjection() { OfflineCatalogSnapshot catalog; OfflineLibraryProjection p(m_cachedSnapshot,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{}); m_fetchOfflineTabs=offlineTabsFromSnapshot(m_cachedSnapshot);m_fetchOfflineMovies.clear();m_fetchOfflineSnapshot=m_cachedSnapshot;for(auto &tab:m_fetchOfflineTabs){if(tab.name=="Movies")tab.rows={{"Movies",{}}};if(tab.name=="Shows")tab.rows={{"Shows",{}}};}for(auto &view:m_fetchOfflineSnapshot.shows){std::vector<MediaItem>filtered;for(const auto&i:view.items)if(p.playable(i.id)||!p.seasons(i.id).empty())filtered.push_back(i);view.items=std::move(filtered);}m_fetchOfflinePrepared=true; }
void HomeScreen::applyOfflineProjection() { const std::vector<TabData> previous=m_tabs;const int selected=m_activeTab;if(!m_fetchOfflinePrepared)return;m_tabs=std::move(m_fetchOfflineTabs);m_activeTab=transitionTabIndex(previous,selected,m_tabs);m_offlineSnapshot=std::move(m_fetchOfflineSnapshot);m_fetchOfflinePrepared=false;resetMediaPaging();clampNavigation(); }
}
