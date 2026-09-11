#ifndef MIYOOFIN_LIBRARY_QUERY_HPP
#define MIYOOFIN_LIBRARY_QUERY_HPP

#include "../catalog/CatalogDb.hpp"
#include <future>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace miyoofin::library {

struct MediaPage {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    bool hasMore = false;
    std::vector<MediaItem> items;
    std::map<std::string, std::vector<CatalogDbMediaPageMembership>> membershipsByItem;
    CatalogDbPageCursor next;
};

struct HierarchyPage {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::vector<MediaItem> items;
};

class LibraryQuery {
public:
    LibraryQuery(std::shared_ptr<CatalogDb> db, std::uint64_t scopeEpoch);
    std::future<MediaPage> movies(int alphabetLetter, std::size_t limit,
                                   const CatalogDbPageCursor &after = {});
    std::future<MediaPage> shows(int alphabetLetter, std::size_t limit,
                                 const CatalogDbPageCursor &after = {});
    std::future<HierarchyPage> seasons(const std::string &seriesId);
    std::future<HierarchyPage> episodes(const std::string &seasonId);
    std::uint64_t scopeEpoch() const { return m_scopeEpoch; }

private:
    std::shared_ptr<CatalogDb> m_db;
    CatalogDbJobMetadata metadata() const;
    std::uint64_t m_scopeEpoch;
};

}
#endif
