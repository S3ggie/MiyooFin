#include "test_support.hpp"
#include "../src/library/LibraryCoordinator.hpp"
#include "cases/test_catalog_migration_support.hpp"
#include "cases/test_catalog_parity_support.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace miyoofin;

namespace {

constexpr const char* kMovieA = "__lq_movie_a__";
constexpr const char* kMovieB = "__lq_movie_b__";
constexpr const char* kShowNormal = "__lq_show_normal__";
constexpr const char* kShowAnime = "__lq_show_anime__";
constexpr const char* kSeries = "__lq_series__";
constexpr const char* kSeason = "__lq_season__";
constexpr const char* kEpisode = "__lq_episode__";

std::string libraryQueryUrl(const char* name)
{
    return std::string("https://library-query-") + name + "-" +
           std::to_string(static_cast<long long>(::getpid())) + ".example";
}

// Seed one scope with movie/show pages (including an anime view and a shared
// anime membership) plus a series/season/episode hierarchy so every LibraryQuery
// method has deterministic input.
void seedLibraryQueryScope(CatalogDb& db, std::uint64_t epoch)
{
    MediaItem movieA = catalogParityItem(kMovieA, "movie", "Alpha Movie");
    MediaItem movieB = catalogParityItem(kMovieB, "movie", "Beta Movie");
    MediaItem normal = catalogParityItem(kShowNormal, "show", "Normal Show");
    normal.genres = {"Drama"};
    MediaItem anime = catalogParityItem(kShowAnime, "show", "Anime Show");
    anime.genres = {"Anime"};

    LibrarySnapshot snapshot;
    snapshot.movies = {{"lq-movies", "Movies", "movies", {movieA, movieB}}};
    snapshot.shows = {{"lq-shows", "Shows", "tvshows", {normal, anime}},
                      {"lq-anime", "Anime Library", "tvshows", {anime}}};
    CHECK(CatalogCompatibility::seedLibrarySnapshot(db, snapshot, {0, epoch, {}}).get().success);

    MediaItem series;
    series.id = kSeries;
    series.type = "show";
    series.title = "Query Series";
    MediaItem season;
    season.id = kSeason;
    season.type = "season";
    season.seriesId = kSeries;
    season.title = "Query Season";
    MediaItem episode = catalogParityItem(kEpisode, "episode", "Query Episode");
    episode.seriesId = kSeries;
    episode.seasonId = kSeason;
    episode.seriesName = series.title;
    CHECK(
        db.upsertSeriesHierarchy(series, {season}, {{kSeason, {episode}}}, 1, 1000).get().success);
}

struct LibraryQueryTestScope
{
    std::string url;
    std::string user;
    CatalogMigrationTestPaths paths;
    std::shared_ptr<CatalogDb> db;
    std::uint64_t epoch = 0;
};

LibraryQueryTestScope makeLibraryQueryScope(const char* name)
{
    LibraryQueryTestScope scope;
    scope.url = libraryQueryUrl(name);
    scope.user = std::string("library-query-") + name + "-user";
    scope.paths = catalogMigrationTestPaths(scope.url, scope.user);
    removeCatalogMigrationTestPaths(scope.paths);
    scope.db = std::make_shared<CatalogDb>();
    scope.epoch = scope.db->configureScope(scope.url, scope.user);
    CHECK(scope.db->waitForIdleForTest(std::chrono::seconds(2)));
    seedLibraryQueryScope(*scope.db, scope.epoch);
    return scope;
}

void removeLibraryQueryScope(LibraryQueryTestScope& scope)
{
    scope.db.reset();
    removeCatalogMigrationTestPaths(scope.paths);
}

// Every query method must return without waiting on the CatalogDb worker, and
// the conversion must not run on the caller thread. Pausing the CatalogDb
// worker makes both observable deterministically: the calls return, and the
// returned futures are still pending.
void testLibraryQueryContinuationIsNonBlocking()
{
    std::printf("[test] LibraryQuery continuation is nonblocking while CatalogDb is paused\n");
    auto scope = makeLibraryQueryScope("nonblocking");
    {
        Session session;
        session.serverUrl = scope.url;
        session.userId = scope.user;
        library::LibraryCoordinator coordinator(session, scope.db, scope.epoch);
        const auto query = coordinator.query();
        CHECK(query != nullptr);

        scope.db->setWorkerPausedForTest(true);
        auto movieFuture = query->movies(-1, 8);
        auto showFuture = query->shows(-1, 8);
        auto animeFuture = query->anime(-1, 8);
        auto seasonFuture = query->seasons(kSeries);
        auto episodeFuture = query->episodes(kSeason);
        auto byIdsFuture = query->itemsByIds({kMovieA});
        // Reaching this point proves none of the calls blocked on the paused
        // CatalogDb worker. The futures must still be pending, proving the
        // conversion did not run inline on the caller thread either.
        CHECK(movieFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        CHECK(showFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        CHECK(animeFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        CHECK(seasonFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        CHECK(episodeFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        CHECK(byIdsFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

        // A caller must release the pause before the executor is torn down; the
        // executor joins its worker, which cannot finish a pending CatalogDb
        // read while the worker is held.
        scope.db->setWorkerPausedForTest(false);
        CHECK(scope.db->waitForIdleForTest(std::chrono::seconds(2)));

        const auto moviePage = movieFuture.get();
        CHECK(moviePage.success && moviePage.items.size() == 2);
        const auto showPage = showFuture.get();
        CHECK(showPage.success && showPage.items.size() == 2);
        const auto animePage = animeFuture.get();
        CHECK(animePage.success && animePage.items.size() == 1);
        const auto seasonsPage = seasonFuture.get();
        CHECK(seasonsPage.success && seasonsPage.items.size() == 1);
        const auto episodesPage = episodeFuture.get();
        CHECK(episodesPage.success && episodesPage.items.size() == 1);
        const auto byIdsPage = byIdsFuture.get();
        CHECK(byIdsPage.success && byIdsPage.items.size() == 1 &&
              byIdsPage.items.front().id == kMovieA);
    }
    removeLibraryQueryScope(scope);
    std::printf("[test] LibraryQuery continuation is nonblocking while CatalogDb is paused OK\n");
}

void testLibraryQueryConvertedResults()
{
    std::printf("[test] LibraryQuery converted domain results\n");
    auto scope = makeLibraryQueryScope("converted");
    {
        Session session;
        session.serverUrl = scope.url;
        session.userId = scope.user;
        library::LibraryCoordinator coordinator(session, scope.db, scope.epoch);
        const auto query = coordinator.query();
        CHECK(query != nullptr);

        // Movies: paging, cursor, and membership mapping.
        const library::LibraryPageCursor initial;
        const auto first = query->movies(-1, 1, initial).get();
        CHECK(first.success && first.items.size() == 1 && first.hasMore);
        CHECK(first.next.valid && first.next.title == first.items.front().title &&
              first.next.id == first.items.front().id && !first.next.sortKey.empty());
        const auto& movieMemberships = first.membershipsByItem.at(first.items.front().id);
        CHECK(movieMemberships.size() == 1);
        CHECK(movieMemberships.front().viewId == "lq-movies");
        CHECK(movieMemberships.front().viewName == "Movies");
        CHECK(movieMemberships.front().collectionType == "movies");

        const auto second = query->movies(-1, 1, first.next).get();
        CHECK(second.success && second.items.size() == 1 && !second.hasMore);
        CHECK(second.items.front().id != first.items.front().id);
        CHECK(second.next.valid && second.next.id == second.items.front().id &&
              second.next.title == second.items.front().title);

        // Shows: the anime show belongs to both the shows and anime views.
        const auto shows = query->shows(-1, 8).get();
        CHECK(shows.success && shows.items.size() == 2);
        CHECK(shows.membershipsByItem.at(kShowNormal).size() == 1);
        CHECK(shows.membershipsByItem.at(kShowAnime).size() == 2);
        bool sawShowsView = false;
        bool sawAnimeView = false;
        for (const auto& membership : shows.membershipsByItem.at(kShowAnime)) {
            if (membership.viewId == "lq-shows")
                sawShowsView = true;
            if (membership.viewId == "lq-anime")
                sawAnimeView = true;
        }
        CHECK(sawShowsView && sawAnimeView);

        // Anime filter keeps only the anime-membership/genre show.
        const auto anime = query->anime(-1, 8).get();
        CHECK(anime.success && anime.items.size() == 1 && anime.items.front().id == kShowAnime);

        // Hierarchy conversions.
        const auto seasons = query->seasons(kSeries).get();
        CHECK(seasons.success && seasons.items.size() == 1 && seasons.items.front().id == kSeason);
        const auto episodes = query->episodes(kSeason).get();
        CHECK(episodes.success && episodes.items.size() == 1 &&
              episodes.items.front().id == kEpisode);

        // By-IDs conversion; CatalogDb orders by id.
        const auto byIds = query->itemsByIds({kMovieB, kShowNormal}).get();
        CHECK(byIds.success && byIds.items.size() == 2);
        CHECK(byIds.items[0].id == kMovieB && byIds.items[1].id == kShowNormal);

        // Spot-check that item fields survived the domain conversion.
        const auto movie = query->itemsByIds({kMovieA}).get();
        CHECK(movie.success && movie.items.size() == 1);
        CHECK(movie.items.front().title == "Alpha Movie");
        CHECK(movie.items.front().type == "movie");
        CHECK(movie.items.front().overview == std::string("Parity overview ") + kMovieA);
        CHECK(movie.items.front().year == 2026);
    }
    removeLibraryQueryScope(scope);
    std::printf("[test] LibraryQuery converted domain results OK\n");
}

void testLibraryQueryCancellationAndErrors()
{
    std::printf("[test] LibraryQuery cancellation and error propagation\n");
    auto scope = makeLibraryQueryScope("errors");
    {
        Session session;
        session.serverUrl = scope.url;
        session.userId = scope.user;
        library::LibraryCoordinator coordinator(session, scope.db, scope.epoch);
        const auto query = coordinator.query();
        CHECK(query != nullptr);

        auto cancellation = std::make_shared<std::atomic_bool>(true);
        // Media-page reads report cancellation without a mapped error category.
        const auto cancelledPage = query->movies(-1, 8, {}, cancellation).get();
        CHECK(cancelledPage.cancelled && !cancelledPage.success &&
              cancelledPage.error == library::LibraryQueryErrorCategory::None);
        // Hierarchy reads reject a pre-cancelled request as superseded.
        const auto cancelledSeasons = query->seasons(kSeries, cancellation).get();
        CHECK(cancelledSeasons.cancelled && !cancelledSeasons.success &&
              cancelledSeasons.error == library::LibraryQueryErrorCategory::Superseded);
        const auto cancelledItems = query->itemsByIds({kMovieA}, cancellation).get();
        CHECK(cancelledItems.cancelled && !cancelledItems.success &&
              cancelledItems.error == library::LibraryQueryErrorCategory::Superseded);

        // Invalid media-page requests map to the SQLite error category.
        const auto invalid = query->movies(-1, 0).get();
        CHECK(!invalid.success && !invalid.cancelled &&
              invalid.error == library::LibraryQueryErrorCategory::SqliteError);

        // An empty by-IDs request is a configuration failure.
        const auto emptyByIds = query->itemsByIds({}).get();
        CHECK(!emptyByIds.success &&
              emptyByIds.error == library::LibraryQueryErrorCategory::ConfigurationFailed);
    }
    removeLibraryQueryScope(scope);
    std::printf("[test] LibraryQuery cancellation and error propagation OK\n");
}

void testLibraryQuerySupersededEpoch()
{
    std::printf("[test] LibraryQuery stale-epoch supersession\n");
    auto scope = makeLibraryQueryScope("superseded");
    const std::uint64_t staleEpoch = scope.epoch;
    const std::uint64_t currentEpoch = scope.db->configureScope(scope.url, scope.user);
    CHECK(scope.db->waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(currentEpoch != staleEpoch);
    {
        Session session;
        session.serverUrl = scope.url;
        session.userId = scope.user;
        library::LibraryCoordinator coordinator(session, scope.db, staleEpoch);
        const auto query = coordinator.query();
        CHECK(query != nullptr);

        const auto stalePage = query->movies(-1, 8).get();
        CHECK(!stalePage.success && stalePage.superseded &&
              stalePage.error == library::LibraryQueryErrorCategory::Superseded);
        const auto staleSeasons = query->seasons(kSeries).get();
        CHECK(!staleSeasons.success && staleSeasons.superseded &&
              staleSeasons.error == library::LibraryQueryErrorCategory::Superseded);
    }
    removeLibraryQueryScope(scope);
    std::printf("[test] LibraryQuery stale-epoch supersession OK\n");
}

// The continuation executor has a bounded FIFO. Pausing the CatalogDb worker
// makes the single conversion worker block on the first read, so every later
// continuation queues behind it; once the queue reaches capacity the next
// submissions must be shed with an already-ready structured queue-full result.
// Submitting capacity+2 (rather than capacity+1) makes at least one rejection
// guaranteed whether or not the worker has popped the first continuation yet.
// No sleeps: readiness is observed directly.
void testLibraryQueryContinuationQueueCapacity()
{
    std::printf("[test] LibraryQuery continuation queue capacity sheds deterministically\n");
    auto scope = makeLibraryQueryScope("capacity");
    {
        Session session;
        session.serverUrl = scope.url;
        session.userId = scope.user;
        library::LibraryCoordinator coordinator(session, scope.db, scope.epoch);
        const auto query = coordinator.query();
        CHECK(query != nullptr);

        const std::size_t capacity = library::LibraryQuery::kMaxPendingContinuations;
        // Keep the burst under CatalogDb's own pending-job cap so every read is
        // accepted and stays pending while paused; only the continuation queue
        // should shed load.
        CHECK(capacity + 2 <= CatalogDb::kMaxPendingJobs);

        // --- Media pages -----------------------------------------------------
        scope.db->setWorkerPausedForTest(true);
        std::vector<std::future<library::MediaPage>> mediaFutures;
        mediaFutures.reserve(capacity + 2);
        for (std::size_t i = 0; i < capacity + 2; ++i)
            mediaFutures.push_back(query->movies(-1, 8));

        std::vector<bool> mediaRejected(mediaFutures.size(), false);
        std::size_t mediaRejectedCount = 0;
        for (std::size_t i = 0; i < mediaFutures.size(); ++i) {
            if (mediaFutures[i].wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
                mediaRejected[i] = true;
                ++mediaRejectedCount;
            }
        }
        CHECK(mediaRejectedCount >= 1);

        // Release CatalogDb and confirm every accepted continuation still
        // resolves. get() must not throw for either kind (no broken_promise).
        scope.db->setWorkerPausedForTest(false);
        CHECK(scope.db->waitForIdleForTest(std::chrono::seconds(2)));
        std::size_t mediaAcceptedCount = 0;
        for (std::size_t i = 0; i < mediaFutures.size(); ++i) {
            const auto page = mediaFutures[i].get();
            if (mediaRejected[i]) {
                CHECK(!page.success && !page.cancelled && !page.superseded);
                CHECK(page.error == library::LibraryQueryErrorCategory::OpenFailed);
                CHECK(page.message == "LibraryQuery continuation queue is full");
            } else {
                CHECK(page.success && page.items.size() == 2);
                ++mediaAcceptedCount;
            }
        }
        CHECK(mediaAcceptedCount + mediaRejectedCount == mediaFutures.size());
        CHECK(mediaAcceptedCount >= 1);

        // --- Hierarchy pages use the same bounded submit path ----------------
        scope.db->setWorkerPausedForTest(true);
        std::vector<std::future<library::HierarchyPage>> hierarchyFutures;
        hierarchyFutures.reserve(capacity + 2);
        for (std::size_t i = 0; i < capacity + 2; ++i)
            hierarchyFutures.push_back(query->seasons(kSeries));

        std::vector<bool> hierarchyRejected(hierarchyFutures.size(), false);
        std::size_t hierarchyRejectedCount = 0;
        for (std::size_t i = 0; i < hierarchyFutures.size(); ++i) {
            if (hierarchyFutures[i].wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
                hierarchyRejected[i] = true;
                ++hierarchyRejectedCount;
            }
        }
        CHECK(hierarchyRejectedCount >= 1);

        scope.db->setWorkerPausedForTest(false);
        CHECK(scope.db->waitForIdleForTest(std::chrono::seconds(2)));
        std::size_t hierarchyAcceptedCount = 0;
        for (std::size_t i = 0; i < hierarchyFutures.size(); ++i) {
            const auto page = hierarchyFutures[i].get();
            if (hierarchyRejected[i]) {
                CHECK(!page.success && !page.cancelled && !page.superseded);
                CHECK(page.error == library::LibraryQueryErrorCategory::OpenFailed);
                CHECK(page.message == "LibraryQuery continuation queue is full");
            } else {
                CHECK(page.success && page.items.size() == 1);
                ++hierarchyAcceptedCount;
            }
        }
        CHECK(hierarchyAcceptedCount + hierarchyRejectedCount == hierarchyFutures.size());
        CHECK(hierarchyAcceptedCount >= 1);
    }
    removeLibraryQueryScope(scope);
    std::printf("[test] LibraryQuery continuation queue capacity sheds deterministically OK\n");
}

// Destroying the executor with continuations still queued must fulfill every
// outstanding promise (structured stopped result) instead of producing a
// broken_promise. The CatalogDb worker is resumed before teardown so the
// in-flight continuation can finish and the join cannot deadlock.
void testLibraryQueryDestructionFulfillsQueuedContinuations()
{
    std::printf("[test] LibraryQuery destruction fulfills queued continuations\n");
    auto scope = makeLibraryQueryScope("destruct");
    std::vector<std::future<library::MediaPage>> futures;
    {
        Session session;
        session.serverUrl = scope.url;
        session.userId = scope.user;
        library::LibraryCoordinator coordinator(session, scope.db, scope.epoch);
        const auto query = coordinator.query();
        CHECK(query != nullptr);

        scope.db->setWorkerPausedForTest(true);
        for (int i = 0; i < 4; ++i)
            futures.push_back(query->movies(-1, 8));
        scope.db->setWorkerPausedForTest(false);
    } // coordinator/query destructor runs here with continuations still queued

    CHECK(scope.db->waitForIdleForTest(std::chrono::seconds(2)));
    for (auto& future : futures) {
        const auto page = future.get(); // must not throw broken_promise
        CHECK(page.success || page.error != library::LibraryQueryErrorCategory::None);
    }
    removeLibraryQueryScope(scope);
    std::printf("[test] LibraryQuery destruction fulfills queued continuations OK\n");
}

// Structural guard for this refactor: the per-query std::async(launch::async)
// continuation sites must be gone from LibraryQuery.cpp. Comments are stripped
// so prose cannot satisfy or break the check.
void testLibraryQuerySourceHasNoAsyncLaunch()
{
    std::printf("[test] LibraryQuery source has no per-query std::async(launch::async)\n");
    const std::string querySource =
        stripSourceComments(readTestBytes("src/library/LibraryQuery.cpp"));
    CHECK(!querySource.empty());
    CHECK(sourceLacks(querySource, "std::async"));
    CHECK(sourceLacks(querySource, "launch::async"));
    std::printf("[test] LibraryQuery source has no per-query std::async(launch::async) OK\n");
}

} // namespace

int main()
{
    testLibraryQuerySourceHasNoAsyncLaunch();
    testLibraryQueryConvertedResults();
    testLibraryQueryContinuationIsNonBlocking();
    testLibraryQueryCancellationAndErrors();
    testLibraryQuerySupersededEpoch();
    testLibraryQueryContinuationQueueCapacity();
    testLibraryQueryDestructionFulfillsQueuedContinuations();
    return miyoofin_test::finish("library_query");
}
