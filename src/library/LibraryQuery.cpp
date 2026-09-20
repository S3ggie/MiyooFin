#include "LibraryQuery.hpp"

namespace miyoofin::library {

LibraryQuery::LibraryQuery(std::shared_ptr<CatalogDb> db, std::uint64_t epoch)
    : m_db(std::move(db)), m_scopeEpoch(epoch) {}

CatalogDbJobMetadata LibraryQuery::metadata(
    const std::shared_ptr<std::atomic_bool> &cancellation) const {
    CatalogDbJobMetadata m;
    m.scopeEpoch = m_scopeEpoch;
    m.cancellation = cancellation;
    return m;
}

std::future<MediaPage> LibraryQuery::movies(int letter, std::size_t limit,
                                             const CatalogDbPageCursor &after,
                                             const std::shared_ptr<std::atomic_bool> &cancellation) {
    auto f = m_db->readMediaPage("movie", letter, limit, after,
                                 metadata(cancellation));
    return std::async(std::launch::async, [f = std::move(f)]() mutable {
        auto r = f.get(); MediaPage out;
        out.success=r.success; out.cancelled=r.cancelled; out.superseded=r.superseded;
        out.error=r.error; out.message=std::move(r.message); out.hasMore=r.hasMore;
        out.items=std::move(r.items); out.membershipsByItem=std::move(r.membershipsByItem); out.next=std::move(r.next); return out;
    });
}

std::future<MediaPage> LibraryQuery::shows(int letter, std::size_t limit,
                                            const CatalogDbPageCursor &after,
                                            const std::shared_ptr<std::atomic_bool> &cancellation) {
    auto f = m_db->readMediaPage("show", letter, limit, after,
                                 metadata(cancellation),
                                 CatalogDbMediaPageFilter::Supported);
    return std::async(std::launch::async, [f = std::move(f)]() mutable {
        auto r = f.get(); MediaPage out;
        out.success=r.success; out.cancelled=r.cancelled; out.superseded=r.superseded;
        out.error=r.error; out.message=std::move(r.message); out.hasMore=r.hasMore;
        out.items=std::move(r.items); out.membershipsByItem=std::move(r.membershipsByItem); out.next=std::move(r.next); return out;
    });
}

bool LibraryQuery::scopeReady() const
{
    if (!m_db || m_scopeEpoch == 0)
        return false;
    const CatalogDbScopeState state = m_db->scopeState();
    return state.requestedEpoch == m_scopeEpoch && state.ready;
}

std::future<MediaPage> LibraryQuery::anime(int letter, std::size_t limit,
                                            const CatalogDbPageCursor &after) {
    auto f = m_db->readMediaPage("show", letter, limit, after, metadata(),
                                 CatalogDbMediaPageFilter::Anime);
    return std::async(std::launch::async, [f = std::move(f)]() mutable {
        auto r = f.get(); MediaPage out;
        out.success=r.success; out.cancelled=r.cancelled; out.superseded=r.superseded;
        out.error=r.error; out.message=std::move(r.message); out.hasMore=r.hasMore;
        out.items=std::move(r.items); out.membershipsByItem=std::move(r.membershipsByItem); out.next=std::move(r.next); return out;
    });
}

std::future<HierarchyPage> LibraryQuery::seasons(
    const std::string &id,
    const std::shared_ptr<std::atomic_bool> &cancellation) {
    auto f=m_db->getSeasons(id, metadata(cancellation));
    return std::async(std::launch::async,[f=std::move(f)]() mutable { auto r=f.get(); HierarchyPage o; o.success=r.success;o.cancelled=r.cancelled;o.superseded=r.superseded;o.error=r.error;o.message=std::move(r.message);o.items=std::move(r.items);return o; });
}
std::future<HierarchyPage> LibraryQuery::episodes(
    const std::string &id,
    const std::shared_ptr<std::atomic_bool> &cancellation) {
    auto f=m_db->getEpisodes(id, metadata(cancellation));
    return std::async(std::launch::async,[f=std::move(f)]() mutable { auto r=f.get(); HierarchyPage o; o.success=r.success;o.cancelled=r.cancelled;o.superseded=r.superseded;o.error=r.error;o.message=std::move(r.message);o.items=std::move(r.items);return o; });
}
std::future<HierarchyPage> LibraryQuery::itemsByIds(
    const std::vector<std::string> &itemIds,
    const std::shared_ptr<std::atomic_bool> &cancellation) {
    auto f=m_db->readMediaItemsByIds(itemIds, metadata(cancellation));
    return std::async(std::launch::async,[f=std::move(f)]() mutable { auto r=f.get(); HierarchyPage o; o.success=r.success;o.cancelled=r.cancelled;o.superseded=r.superseded;o.error=r.error;o.message=std::move(r.message);o.items=std::move(r.items);return o; });
}
}
