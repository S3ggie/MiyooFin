#include "HomeTabs.hpp"
#include "MovieTitle.hpp"
#include <algorithm>
#include <map>

namespace miyoofin {

void updateContinueWatchingRow(std::vector<TabData> &tabs, const std::vector<MediaItem> &items)
{
    auto homeIt = std::find_if(tabs.begin(), tabs.end(), [](const TabData &tab) { return tab.name == "Home"; });
    if (homeIt == tabs.end()) return;
    auto &rows = homeIt->rows;
    auto cwIt = std::find_if(rows.begin(), rows.end(), [](const MediaRow &row) { return row.label == "Continue Watching"; });
    if (!items.empty()) {
        if (cwIt != rows.end()) cwIt->items = items;
        else {
            if (rows.size() == 1 && rows[0].label.empty() && rows[0].items.empty()) rows.clear();
            auto recentlyAdded = std::find_if(rows.begin(), rows.end(), [](const MediaRow &row) { return row.label == "Recently Added"; });
            rows.insert(recentlyAdded, {"Continue Watching", items});
        }
    } else if (cwIt != rows.end()) {
        rows.erase(cwIt);
        if (rows.empty()) rows.push_back({"", {}});
    }
}

std::vector<MediaItem> combineMovieViews(const std::vector<CachedLibraryView> &views)
{
    std::vector<MediaItem> out; std::map<std::string, bool> seen;
    for (const auto &v : views) for (const auto &i : v.items)
        if (seen.emplace(i.id, true).second) out.push_back(i);
    std::sort(out.begin(), out.end(), movieOrganizationalLess);
    return out;
}

std::vector<TabData> tabsFromSnapshot(const LibrarySnapshot &s)
{
    std::vector<TabData> tabs; std::vector<MediaRow> home;
    if (!s.continueWatching.empty()) home.push_back({"Continue Watching", s.continueWatching});
    if (!s.recentlyAdded.empty()) home.push_back({"Recently Added", s.recentlyAdded});
    if (home.empty()) home.push_back({"", {}});
    tabs.push_back({"Home", std::move(home)});
    TabData movies{"Movies", {{"Movies", combineMovieViews(s.movies)}}};
    if (movies.rows.empty()) movies.rows.push_back({"No movies found", {}});
    tabs.push_back(std::move(movies));
    TabData shows{"Shows", {}}; for (const auto &v : s.shows) shows.rows.push_back({v.name, v.items});
    if (shows.rows.empty()) shows.rows.push_back({"No shows found", {}});
    tabs.push_back(std::move(shows));
    tabs.push_back({"Downloads", {{"", {}}}}); tabs.push_back({"Settings", {{"", {}}}});
    return tabs;
}

std::vector<TabData> offlineTabsFromSnapshot(const LibrarySnapshot &snapshot)
{
    auto tabs = tabsFromSnapshot(snapshot);
    tabs.erase(std::remove_if(tabs.begin(), tabs.end(), [](const TabData &tab) { return tab.name == "Home" || tab.name == "Search"; }), tabs.end());
    return tabs;
}

std::vector<std::string> tabNames(const std::vector<TabData> &tabs)
{ std::vector<std::string> names; for (const auto &tab : tabs) names.push_back(tab.name); return names; }

int transitionTabIndex(const std::vector<TabData> &from, int selected, const std::vector<TabData> &to)
{
    const std::string name = (selected >= 0 && selected < (int)from.size()) ? from[selected].name : "";
    for (int i = 0; i < (int)to.size(); ++i) if (to[i].name == name) return i;
    for (int i = 0; i < (int)to.size(); ++i) if (to[i].name == "Movies") return i;
    return to.empty() ? 0 : std::min(std::max(selected, 0), (int)to.size() - 1);
}

}
