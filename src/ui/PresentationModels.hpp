#ifndef MIYOOFIN_PRESENTATION_MODELS_HPP
#define MIYOOFIN_PRESENTATION_MODELS_HPP

#include "../data/MediaItem.hpp"
#include <string>
#include <vector>

namespace miyoofin {

/// A horizontal row of media items (one section on the home screen).
struct MediaRow {
    std::string label;
    std::vector<MediaItem> items;
};

/// A top-level tab with its rows.
struct TabData {
    std::string name;
    std::vector<MediaRow> rows;
};

} // namespace miyoofin

#endif // MIYOOFIN_PRESENTATION_MODELS_HPP
