#ifndef MIYOOFIN_HOME_TABS_HPP
#define MIYOOFIN_HOME_TABS_HPP

#include "../cache/LibraryCache.hpp"
#include "PresentationModels.hpp"
#include <string>
#include <vector>

namespace miyoofin {

void updateContinueWatchingRow(std::vector<TabData> &tabs, const std::vector<MediaItem> &items);
void updateRecentlyAddedRow(std::vector<TabData> &tabs, const std::vector<MediaItem> &items);
struct HomeMediaWindows {
    std::vector<MediaItem> movies;
    std::vector<MediaItem> shows;
};
HomeMediaWindows mediaWindowsFromTabs(const std::vector<TabData> &tabs);
std::vector<TabData> tabsFromSnapshot(const LibrarySnapshot &snapshot);
std::vector<TabData> offlineTabsFromSnapshot(const LibrarySnapshot &snapshot);
std::vector<std::string> tabNames(const std::vector<TabData> &tabs);
int transitionTabIndex(const std::vector<TabData> &from, int selected, const std::vector<TabData> &to);
std::vector<MediaItem> combineMovieViews(const std::vector<CachedLibraryView> &views);

/// Linear search for the first row with a matching label.  Returns -1 if
/// absent or if `rows` is empty.  Duplicates return the first index.
int homeRowIndexByLabel(const std::vector<MediaRow> &rows, const std::string &label);

}

#endif
