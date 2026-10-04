#pragma once

#include "LibraryCoordinator.hpp"
#include "../net/JellyfinLibraryEvents.hpp"
#include <cstdint>
#include <ctime>
#include <string>

// Helpers shared by the LibraryCoordinator*.cpp implementation units. LibraryCoordinator is one
// class with one set of locks, atomics and workers; its method definitions are split across
// units by responsibility (see docs/architecture.md), and these file-local helpers move with them.
namespace miyoofin {
namespace library {
namespace coordinator_detail {

inline constexpr bool kCoordinatorStartupSyncEnabled = true;

inline std::int64_t coordinatorWallClockMs()
{
    return static_cast<std::int64_t>(std::time(nullptr)) * 1000;
}

inline bool coordinatorSupportedLibraryView(const LibraryView& view)
{
    return view.collectionType == "movies" || view.collectionType == "tvshows";
}

inline bool coordinatorLiveChangeIsEmpty(const JellyfinLibraryChangeBatch& batch)
{
    return !batch.catchUpRequired && !batch.userDataChanged && batch.itemsAdded.empty() &&
           batch.itemsUpdated.empty() && batch.itemsRemoved.empty();
}

inline void appendCoordinatorGateReason(std::string& reasons, const char* reason, bool blocked)
{
    if (!blocked)
        return;
    if (!reasons.empty())
        reasons += ',';
    reasons += reason;
}

inline bool coordinatorHierarchyIdentityMatches(const HierarchyRequest& left,
                                                const HierarchyRequest& right)
{
    if (left.kind != right.kind || left.generation != right.generation)
        return false;
    if (left.kind == HierarchyTaskKind::HomePrefetch)
        return false;
    if (left.series.id != right.series.id)
        return false;
    return left.kind != HierarchyTaskKind::SeasonEpisodes || left.season.id == right.season.id;
}

} // namespace coordinator_detail
} // namespace library
} // namespace miyoofin
