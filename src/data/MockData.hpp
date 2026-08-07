#ifndef MIYOOFIN_MOCK_DATA_HPP
#define MIYOOFIN_MOCK_DATA_HPP

#include <vector>
#include <string>
#include "MediaItem.hpp"

namespace miyoofin {

/// Returns the singleton mock dataset (used for tests and fallback).
const std::vector<TabData>& getMockTabs();

} // namespace miyoofin

#endif // MIYOOFIN_MOCK_DATA_HPP