#ifndef MIYOOFIN_HOME_TABS_HPP
#define MIYOOFIN_HOME_TABS_HPP

#include "../cache/LibraryCache.hpp"
#include "PresentationModels.hpp"
#include <string>
#include <vector>

namespace miyoofin {

void updateContinueWatchingRow(std::vector<TabData>& tabs, const std::vector<MediaItem>& items);
void updateRecentlyAddedRow(std::vector<TabData>& tabs, const std::vector<MediaItem>& items);
/// Copies watch state onto grid copies of movies after the Home rails refresh:
/// movies the rails report take the rail's state; movies that were in the
/// previous Continue Watching rail but are in neither current rail lose their
/// resume progress (finished or dropped).
void applyRailWatchState(std::vector<MediaItem>& movies,
                         const std::vector<MediaItem>& previousContinue,
                         const std::vector<MediaItem>& continueNow,
                         const std::vector<MediaItem>& recentNow);
std::vector<TabData>
buildTabs(const std::vector<MediaItem>& continueWatching,
          const std::vector<MediaItem>& recentlyAdded,
          const std::vector<std::pair<std::string, std::vector<MediaItem>>>& moviesByView,
          const std::vector<std::pair<std::string, std::vector<MediaItem>>>& showsByView);
struct HomeMediaWindows
{
    std::vector<MediaItem> movies;
    std::vector<MediaItem> shows;
};
HomeMediaWindows mediaWindowsFromTabs(const std::vector<TabData>& tabs);
std::vector<TabData> tabsFromSnapshot(const LibrarySnapshot& snapshot);
std::vector<TabData> offlineTabsFromSnapshot(const LibrarySnapshot& snapshot);
std::vector<std::string> tabNames(const std::vector<TabData>& tabs);
int transitionTabIndex(const std::vector<TabData>& from, int selected,
                       const std::vector<TabData>& to);
std::vector<MediaItem> combineMovieViews(const std::vector<CachedLibraryView>& views);

/// Linear search for the first row with a matching label.  Returns -1 if
/// absent or if `rows` is empty.  Duplicates return the first index.
int homeRowIndexByLabel(const std::vector<MediaRow>& rows, const std::string& label);

}

#endif
