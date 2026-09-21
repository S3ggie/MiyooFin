#ifndef MIYOOFIN_LIBRARY_QUERY_HPP
#define MIYOOFIN_LIBRARY_QUERY_HPP

#include "../data/MediaItem.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace miyoofin {

class CatalogDb;

namespace library {

class LibraryCoordinator;

enum class LibraryQueryErrorCategory : unsigned char {
    None,
    InvalidIdentity,
    ScopeNotReady,
    OpenFailed,
    WrongApplicationId,
    UnsupportedVersion,
    CorruptOrIo,
    ConfigurationFailed,
    SqliteError,
    Superseded,
};

struct LibraryPageCursor {
    std::string sortKey;
    std::string title;
    std::string id;
    bool valid = false;
};

struct LibraryMembership {
    std::string viewId;
    std::string viewName;
    std::string collectionType;
};

struct MediaPage {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    LibraryQueryErrorCategory error = LibraryQueryErrorCategory::None;
    std::string message;
    bool hasMore = false;
    std::vector<MediaItem> items;
    std::map<std::string, std::vector<LibraryMembership>> membershipsByItem;
    LibraryPageCursor next;
};

struct HierarchyPage {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    LibraryQueryErrorCategory error = LibraryQueryErrorCategory::None;
    std::string message;
    std::vector<MediaItem> items;
};

class LibraryQuery {
public:
    std::future<MediaPage> movies(int alphabetLetter, std::size_t limit,
                                   const LibraryPageCursor &after = {},
                                   const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<MediaPage> shows(int alphabetLetter, std::size_t limit,
                                 const LibraryPageCursor &after = {},
                                 const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<MediaPage> anime(int alphabetLetter, std::size_t limit,
                                 const LibraryPageCursor &after = {});
    std::future<HierarchyPage> seasons(
        const std::string &seriesId,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<HierarchyPage> episodes(
        const std::string &seasonId,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<HierarchyPage> itemsByIds(
        const std::vector<std::string> &itemIds,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::uint64_t scopeEpoch() const { return m_scopeEpoch; }
    bool scopeReady() const;

private:
    friend class LibraryCoordinator;

    LibraryQuery(std::shared_ptr<CatalogDb> db, std::uint64_t scopeEpoch);

    std::shared_ptr<CatalogDb> m_db;
    std::uint64_t m_scopeEpoch;
};

}
}
#endif
