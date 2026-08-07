#ifndef MIYOOFIN_MOCK_DATA_HPP
#define MIYOOFIN_MOCK_DATA_HPP

#include <vector>
#include <string>
#include "MediaItem.hpp"

namespace miyoofin {

/// A horizontal row of media items (one section on the home screen).
struct MediaRow {
    std::string          label;
    std::vector<MediaItem> items;
};

/// A top-level tab with its rows.
struct TabData {
    std::string           name;
    std::vector<MediaRow> rows;
};

/// Returns the singleton mock dataset.
const std::vector<TabData>& getMockTabs();

} // namespace miyoofin

#endif // MIYOOFIN_MOCK_DATA_HPP