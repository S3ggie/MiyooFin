#ifndef MIYOOFIN_LIBRARY_CHANGE_TYPES_HPP
#define MIYOOFIN_LIBRARY_CHANGE_TYPES_HPP

#include "../catalog/CatalogDb.hpp"
#include "../data/MediaItem.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace miyoofin {
namespace library {

struct LiveLibraryChangeResult {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    bool catchUpRequired = false;
    bool userDataChanged = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::uint64_t generation = 0;
    std::uint64_t committedGeneration = 0;
    std::int64_t checkpointMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
    std::size_t itemsFetched = 0;
    std::size_t itemsUpserted = 0;
    std::size_t itemsRemoved = 0;
    std::vector<MediaItem> items;
    std::vector<std::string> removedIds;
};

} // namespace library
} // namespace miyoofin

#endif // MIYOOFIN_LIBRARY_CHANGE_TYPES_HPP
