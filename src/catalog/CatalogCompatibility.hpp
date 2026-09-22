#ifndef MIYOOFIN_CATALOG_COMPATIBILITY_HPP
#define MIYOOFIN_CATALOG_COMPATIBILITY_HPP

#include "CatalogDb.hpp"
#include "../cache/LibraryCache.hpp"
#include <future>

namespace miyoofin {

struct CatalogCompatibilitySeedRequest
{
    LibrarySnapshot snapshot;
};

struct CatalogCompatibilitySeedResult
{
    bool success = false;
    bool workerOwned = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::size_t itemsUpserted = 0;
    std::size_t viewsWritten = 0;
    std::size_t homeItemsWritten = 0;
};

struct CatalogCompatibilityReadResult
{
    bool success = false;
    bool workerOwned = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    LibrarySnapshot snapshot;
};

// The only public owner of the legacy LibrarySnapshot seed/read boundary.
// Implementations enqueue work on CatalogDb; this class never opens SQLite.
class CatalogCompatibility
{
  public:
    static std::future<CatalogCompatibilitySeedResult>
    seedLibrarySnapshot(CatalogDb& db, const LibrarySnapshot& snapshot,
                        const CatalogDbJobMetadata& metadata = {});
#ifdef MIYOOFIN_TEST_BUILD
    static std::future<CatalogCompatibilitySeedResult>
    seedLibrarySnapshotForTest(CatalogDb& db, const LibrarySnapshot& snapshot, int failAfterWrites,
                               const CatalogDbJobMetadata& metadata = {});
#endif // MIYOOFIN_TEST_BUILD
    static std::future<CatalogCompatibilityReadResult>
    readLibrarySnapshot(CatalogDb& db, const CatalogDbJobMetadata& metadata = {});
};

}

#endif
