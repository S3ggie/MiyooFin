#ifndef MIYOOFIN_TEST_ARTWORK_EPISODE_SUPPORT_HPP
#define MIYOOFIN_TEST_ARTWORK_EPISODE_SUPPORT_HPP

// Shared helpers for the test_artwork_episode* groups. Helpers are inline so a group that does not
// use one does not warn.

#include "../test_support.hpp"

// ===================================================================
// B5d2a tests — Row artwork loading state (no rendering)
// ===================================================================

#include <map>

// Helper: simulate visible scheduling (same RAM-cache filtering as Home).
static inline std::vector<std::string>
pickCandidates(const std::vector<MediaItem>& items,
               const std::map<std::string, RowArtworkStatus>& statusMap)
{
    std::vector<std::string> candidates;
    for (const auto& item : items) {
        std::string key = buildRowArtworkKey(item);
        if (key.empty())
            continue;
        if (statusMap.find(key) == statusMap.end())
            candidates.push_back(std::move(key));
    }
    return candidates;
}

#endif
