#include "LibraryQuery.hpp"
#include "../catalog/CatalogDb.hpp"

namespace miyoofin::library {

namespace {

LibraryQueryErrorCategory toLibraryError(CatalogDbErrorCategory error)
{
    switch (error) {
    case CatalogDbErrorCategory::None:
        return LibraryQueryErrorCategory::None;
    case CatalogDbErrorCategory::InvalidIdentity:
        return LibraryQueryErrorCategory::InvalidIdentity;
    case CatalogDbErrorCategory::ScopeNotReady:
        return LibraryQueryErrorCategory::ScopeNotReady;
    case CatalogDbErrorCategory::OpenFailed:
        return LibraryQueryErrorCategory::OpenFailed;
    case CatalogDbErrorCategory::WrongApplicationId:
        return LibraryQueryErrorCategory::WrongApplicationId;
    case CatalogDbErrorCategory::UnsupportedVersion:
        return LibraryQueryErrorCategory::UnsupportedVersion;
    case CatalogDbErrorCategory::CorruptOrIo:
        return LibraryQueryErrorCategory::CorruptOrIo;
    case CatalogDbErrorCategory::ConfigurationFailed:
        return LibraryQueryErrorCategory::ConfigurationFailed;
    case CatalogDbErrorCategory::SqliteError:
        return LibraryQueryErrorCategory::SqliteError;
    case CatalogDbErrorCategory::Superseded:
        return LibraryQueryErrorCategory::Superseded;
    }
    return LibraryQueryErrorCategory::SqliteError;
}

CatalogDbPageCursor toCatalogCursor(const LibraryPageCursor& cursor)
{
    CatalogDbPageCursor out;
    out.sortKey = cursor.sortKey;
    out.title = cursor.title;
    out.id = cursor.id;
    out.valid = cursor.valid;
    return out;
}

LibraryPageCursor toLibraryCursor(CatalogDbPageCursor cursor)
{
    LibraryPageCursor out;
    out.sortKey = std::move(cursor.sortKey);
    out.title = std::move(cursor.title);
    out.id = std::move(cursor.id);
    out.valid = cursor.valid;
    return out;
}

std::vector<LibraryMembership>
toLibraryMemberships(std::vector<CatalogDbMediaPageMembership> memberships)
{
    std::vector<LibraryMembership> out;
    out.reserve(memberships.size());
    for (auto& membership : memberships) {
        LibraryMembership converted;
        converted.viewId = std::move(membership.viewId);
        converted.viewName = std::move(membership.viewName);
        converted.collectionType = std::move(membership.collectionType);
        out.push_back(std::move(converted));
    }
    return out;
}

CatalogDbJobMetadata metadataFor(std::uint64_t scopeEpoch,
                                 const std::shared_ptr<std::atomic_bool>& cancellation)
{
    CatalogDbJobMetadata metadata;
    metadata.scopeEpoch = scopeEpoch;
    metadata.cancellation = cancellation;
    return metadata;
}

MediaPage toLibraryPage(CatalogDbMediaPageResult result)
{
    MediaPage out;
    out.success = result.success;
    out.cancelled = result.cancelled;
    out.superseded = result.superseded;
    out.error = toLibraryError(result.error);
    out.message = std::move(result.message);
    out.hasMore = result.hasMore;
    out.items = std::move(result.items);
    for (auto& entry : result.membershipsByItem)
        out.membershipsByItem.emplace(std::move(entry.first),
                                      toLibraryMemberships(std::move(entry.second)));
    out.next = toLibraryCursor(std::move(result.next));
    return out;
}

HierarchyPage toLibraryHierarchyPage(CatalogDbHierarchyResult result)
{
    HierarchyPage out;
    out.success = result.success;
    out.cancelled = result.cancelled;
    out.superseded = result.superseded;
    out.error = toLibraryError(result.error);
    out.message = std::move(result.message);
    out.items = std::move(result.items);
    return out;
}

} // namespace

LibraryQuery::LibraryQuery(std::shared_ptr<CatalogDb> db, std::uint64_t epoch)
    : m_db(std::move(db)), m_scopeEpoch(epoch)
{}

std::future<MediaPage> LibraryQuery::movies(int letter, std::size_t limit,
                                            const LibraryPageCursor& after,
                                            const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto f = m_db->readMediaPage("movie", letter, limit, toCatalogCursor(after),
                                 metadataFor(m_scopeEpoch, cancellation));
    return std::async(std::launch::async,
                      [f = std::move(f)]() mutable { return toLibraryPage(f.get()); });
}

std::future<MediaPage> LibraryQuery::shows(int letter, std::size_t limit,
                                           const LibraryPageCursor& after,
                                           const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto f = m_db->readMediaPage("show", letter, limit, toCatalogCursor(after),
                                 metadataFor(m_scopeEpoch, cancellation),
                                 CatalogDbMediaPageFilter::Supported);
    return std::async(std::launch::async,
                      [f = std::move(f)]() mutable { return toLibraryPage(f.get()); });
}

bool LibraryQuery::scopeReady() const
{
    if (!m_db || m_scopeEpoch == 0)
        return false;
    const CatalogDbScopeState state = m_db->scopeState();
    return state.requestedEpoch == m_scopeEpoch && state.ready;
}

std::future<MediaPage> LibraryQuery::anime(int letter, std::size_t limit,
                                           const LibraryPageCursor& after)
{
    auto f = m_db->readMediaPage("show", letter, limit, toCatalogCursor(after),
                                 metadataFor(m_scopeEpoch, {}), CatalogDbMediaPageFilter::Anime);
    return std::async(std::launch::async,
                      [f = std::move(f)]() mutable { return toLibraryPage(f.get()); });
}

std::future<HierarchyPage>
LibraryQuery::seasons(const std::string& id, const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto f = m_db->getSeasons(id, metadataFor(m_scopeEpoch, cancellation));
    return std::async(std::launch::async,
                      [f = std::move(f)]() mutable { return toLibraryHierarchyPage(f.get()); });
}
std::future<HierarchyPage>
LibraryQuery::episodes(const std::string& id, const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto f = m_db->getEpisodes(id, metadataFor(m_scopeEpoch, cancellation));
    return std::async(std::launch::async,
                      [f = std::move(f)]() mutable { return toLibraryHierarchyPage(f.get()); });
}
std::future<HierarchyPage>
LibraryQuery::itemsByIds(const std::vector<std::string>& itemIds,
                         const std::shared_ptr<std::atomic_bool>& cancellation)
{
    auto f = m_db->readMediaItemsByIds(itemIds, metadataFor(m_scopeEpoch, cancellation));
    return std::async(std::launch::async,
                      [f = std::move(f)]() mutable { return toLibraryHierarchyPage(f.get()); });
}
}
