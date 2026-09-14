#include "CatalogCompatibility.hpp"

namespace miyoofin {

std::future<CatalogCompatibilitySeedResult>
CatalogCompatibility::seedLibrarySnapshot(
    CatalogDb &db, const LibrarySnapshot &snapshot,
    const CatalogDbJobMetadata &metadata)
{
    CatalogCompatibilitySeedRequest request;
    request.snapshot = snapshot;
    return db.enqueueLibrarySeed(request, metadata);
}

#ifdef MIYOOFIN_TEST_BUILD
std::future<CatalogCompatibilitySeedResult>
CatalogCompatibility::seedLibrarySnapshotForTest(
    CatalogDb &db, const LibrarySnapshot &snapshot, int failAfterWrites,
    const CatalogDbJobMetadata &metadata)
{
    CatalogCompatibilitySeedRequest request;
    request.snapshot = snapshot;
    return db.enqueueLibrarySeed(request, metadata,
                                 CatalogDbFailureSpec{-1, -1, failAfterWrites});
}
#endif // MIYOOFIN_TEST_BUILD

std::future<CatalogCompatibilityReadResult>
CatalogCompatibility::readLibrarySnapshot(
    CatalogDb &db, const CatalogDbJobMetadata &metadata)
{
    return db.enqueueLibraryRead(metadata);
}

}
