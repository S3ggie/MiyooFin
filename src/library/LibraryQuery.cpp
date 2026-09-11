#include "LibraryQuery.hpp"

namespace miyoofin::library {

LibraryQuery::LibraryQuery(std::shared_ptr<CatalogDb> db, std::uint64_t epoch)
    : m_db(std::move(db)), m_scopeEpoch(epoch) {}

CatalogDbJobMetadata LibraryQuery::metadata() const {
    CatalogDbJobMetadata m;
    m.scopeEpoch = m_scopeEpoch;
    return m;
}

std::future<MediaPage> LibraryQuery::movies(int letter, std::size_t limit,
                                             const CatalogDbPageCursor &after) {
    auto f = m_db->readMediaPage("movie", letter, limit, after, metadata());
    return std::async(std::launch::async, [f = std::move(f)]() mutable {
        auto r = f.get(); MediaPage out;
        out.success=r.success; out.cancelled=r.cancelled; out.superseded=r.superseded;
        out.error=r.error; out.message=std::move(r.message); out.hasMore=r.hasMore;
        out.items=std::move(r.items); out.next=std::move(r.next); return out;
    });
}

std::future<MediaPage> LibraryQuery::shows(int letter, std::size_t limit,
                                            const CatalogDbPageCursor &after) {
    auto f = m_db->readMediaPage("show", letter, limit, after, metadata());
    return std::async(std::launch::async, [f = std::move(f)]() mutable {
        auto r = f.get(); MediaPage out;
        out.success=r.success; out.cancelled=r.cancelled; out.superseded=r.superseded;
        out.error=r.error; out.message=std::move(r.message); out.hasMore=r.hasMore;
        out.items=std::move(r.items); out.next=std::move(r.next); return out;
    });
}

std::future<HierarchyPage> LibraryQuery::seasons(const std::string &id) {
    auto f=m_db->getSeasons(id, metadata());
    return std::async(std::launch::async,[f=std::move(f)]() mutable { auto r=f.get(); HierarchyPage o; o.success=r.success;o.cancelled=r.cancelled;o.superseded=r.superseded;o.error=r.error;o.message=std::move(r.message);o.items=std::move(r.items);return o; });
}
std::future<HierarchyPage> LibraryQuery::episodes(const std::string &id) {
    auto f=m_db->getEpisodes(id, metadata());
    return std::async(std::launch::async,[f=std::move(f)]() mutable { auto r=f.get(); HierarchyPage o; o.success=r.success;o.cancelled=r.cancelled;o.superseded=r.superseded;o.error=r.error;o.message=std::move(r.message);o.items=std::move(r.items);return o; });
}
}
