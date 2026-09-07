#ifndef MIYOOFIN_HOME_TABS_HPP
#define MIYOOFIN_HOME_TABS_HPP

#include "../cache/LibraryCache.hpp"
#include "../data/MediaItem.hpp"
#include <string>
#include <vector>

namespace miyoofin {

void updateContinueWatchingRow(std::vector<TabData> &tabs, const std::vector<MediaItem> &items);
std::vector<TabData> tabsFromSnapshot(const LibrarySnapshot &snapshot);
std::vector<TabData> offlineTabsFromSnapshot(const LibrarySnapshot &snapshot);
std::vector<std::string> tabNames(const std::vector<TabData> &tabs);
int transitionTabIndex(const std::vector<TabData> &from, int selected, const std::vector<TabData> &to);
std::vector<MediaItem> combineMovieViews(const std::vector<CachedLibraryView> &views);

}

#endif
