#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "../../src/catalog/CatalogPrimitives.hpp"
#include "../../src/catalog/CatalogCompatibility.hpp"
#include "../../src/net/JellyfinLibraryEvents.hpp"

namespace {

bool catalogParityItemEqual(const MediaItem &expected,
                            const MediaItem &actual)
{
    const bool equal = expected.id == actual.id
        && expected.title == actual.title
        && expected.overview == actual.overview
        && expected.year == actual.year
        && expected.rating == actual.rating
        && expected.genre == actual.genre && expected.type == actual.type
        && expected.etag == actual.etag && expected.genres == actual.genres
        && expected.played == actual.played
        && expected.progress == actual.progress
        && expected.playbackPositionTicks == actual.playbackPositionTicks
        && expected.imageTags == actual.imageTags
        && expected.indexNumber == actual.indexNumber
        && expected.parentIndexNumber == actual.parentIndexNumber
        && expected.runTimeTicks == actual.runTimeTicks
        && expected.seriesName == actual.seriesName
        && expected.seriesId == actual.seriesId
        && expected.seasonId == actual.seasonId && expected.artR == actual.artR
        && expected.artG == actual.artG && expected.artB == actual.artB;
    if (!equal) {
        std::printf("[catalog parity] MediaItem mismatch for synthetic id %s\n",
                    expected.id.c_str());
    }
    return equal;
}

void checkCatalogParityItems(const std::vector<MediaItem> &legacy,
                             const std::vector<MediaItem> &sqlite)
{
    CHECK(legacy.size() == sqlite.size());
    const std::size_t count = std::min(legacy.size(), sqlite.size());
    for (std::size_t index = 0; index < count; ++index) {
        CHECK(legacy[index].id == sqlite[index].id);
        CHECK(catalogParityItemEqual(legacy[index], sqlite[index]));
    }
}

MediaItem catalogParityItem(const std::string &id, const std::string &type,
                            const std::string &title)
{
    MediaItem item;
    item.id = id;
    item.type = type;
    item.title = title;
    item.overview = "Parity overview " + id;
    item.year = 2026;
    item.rating = 7.25f;
    item.genre = "Drama";
    item.genres = {"Drama", "Science Fiction"};
    item.etag = "parity-etag-" + id;
    item.played = true;
    item.progress = 0.375f;
    item.playbackPositionTicks = 1234567;
    item.imageTags = {{"Backdrop", "backdrop-" + id},
                      {"Primary", "primary-" + id}};
    item.indexNumber = 1;
    item.parentIndexNumber = 2;
    item.runTimeTicks = 9876543;
    item.seriesName = "Parity 世界 Series";
    item.artR = 11;
    item.artG = 22;
    item.artB = 33;
    return item;
}

bool configureCatalogParityDb(CatalogDb &db, const std::string &url,
                              const std::string &user)
{
    const auto epoch = db.configureScope(url, user);
    if (!db.waitForIdleForTest(std::chrono::seconds(2))) {
        return false;
    }
    const auto state = db.scopeState();
    return state.requestedEpoch == epoch && state.ready;
}

static std::vector<std::string> collectCatalogPageIds(
    CatalogDb &db, const std::string &type, int letter, std::size_t pageSize,
    std::uint64_t epoch)
{
    std::vector<std::string> ids;
    CatalogDbPageCursor cursor;
    for (std::size_t page = 0; page < 4096; ++page) {
        const auto result = db.readMediaPage(
            type, letter, pageSize, cursor, {0, epoch, {}}).get();
        CHECK(result.success && result.workerOwned && !result.cancelled
              && !result.superseded);
        std::set<std::string> pageIds;
        for (const auto &item : result.items) {
            CHECK(pageIds.insert(item.id).second);
            CHECK(std::find(ids.begin(), ids.end(), item.id) == ids.end());
            ids.push_back(item.id);
        }
        if (!result.hasMore) {
            CHECK(!result.items.empty() || ids.empty());
            break;
        }
        CHECK(result.next.valid);
        cursor = result.next;
    }
    return ids;
}

} // namespace

