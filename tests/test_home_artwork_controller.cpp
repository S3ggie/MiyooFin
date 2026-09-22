#include "test_support.hpp"
#include "../src/ui/screens/HomeArtworkController.hpp"

using miyoofin::HomeArtworkController;

static HomeArtworkController::DecodeRequest
decodeRequest(const char* id, miyoofin::ArtworkContext context = miyoofin::ArtworkContext::HomeGrid)
{
    HomeArtworkController::DecodeRequest request;
    request.identity = {id, miyoofin::ImageType::Primary, "tag", 64, 96};
    request.context = context;
    return request;
}

static bool takeOne(HomeArtworkController& controller,
                    std::deque<HomeArtworkController::DecodeResult>& results)
{
    for (int i = 0; i < 100; ++i) {
        controller.takeDecodeResults(results);
        if (!results.empty())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

static bool waitForCompleted(HomeArtworkController& controller, std::size_t completed)
{
    for (int i = 0; i < 100; ++i) {
        if (controller.artworkProgress().completed >= completed)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

static void writeTinyCachedPoster(const std::string& item)
{
    const std::string bytes = miyoofin_test::readTestBytes("tests/fixtures/tiny.jpg");
    CHECK(!bytes.empty());
    CHECK(miyoofin::ImageCache::writeToCache(item, miyoofin::ImageType::Primary, "tag", 64, 96,
                                             reinterpret_cast<const unsigned char*>(bytes.data()),
                                             bytes.size()));
}

static void testControllerHasNoHomeOrSdlDependency()
{
    const std::string header =
        miyoofin_test::readTestBytes("src/ui/screens/HomeArtworkController.hpp");
    const std::string source =
        miyoofin_test::readTestBytes("src/ui/screens/HomeArtworkController.cpp");
    CHECK(header.find("HomeScreen") == std::string::npos);
    CHECK(source.find("HomeScreen") == std::string::npos);
    CHECK(header.find("#include <SDL") == std::string::npos);
    CHECK(source.find("#include <SDL") == std::string::npos);
    CHECK(header.find("LibrarySync") == std::string::npos);
    CHECK(source.find("LibrarySync") == std::string::npos);
}

static void testIdentityAndImmutableResultMetadata()
{
    HomeArtworkController::RequestIdentity identity{"item", miyoofin::ImageType::Primary, "tag", 64,
                                                    96};
    CHECK(HomeArtworkController::identityKey(identity) == "item:Primary:tag:64x96");
    CHECK(HomeArtworkController::shouldProcessPosterJob(true, true));
    CHECK(!HomeArtworkController::shouldProcessPosterJob(false, true));
    CHECK(HomeArtworkController::shouldProcessPosterJob(false, false));

    HomeArtworkController controller(miyoofin::Session{}, 1);
    controller.setShowsWorkingSet(9, {"item:Primary:tag:64x96"});
    auto request = decodeRequest("item", miyoofin::ArtworkContext::HomeShows);
    request.showsWorkingSetGeneration = 9;
    controller.requestDecode(request);

    std::deque<HomeArtworkController::DecodeResult> results;
    CHECK(takeOne(controller, results));
    CHECK(results.front().identity == identity);
    CHECK(results.front().showsWorkingSetGeneration == 9);
    CHECK(results.front().context == miyoofin::ArtworkContext::HomeShows);
    CHECK(results.front().requestSequence != 0);
    controller.completeDecodeResult(results.front(), true);
    controller.requestStopAllWorkers();
    controller.joinAllWorkers();
}

static void testPosterDeferralPriorityCacheHitAndProgress()
{
    HomeArtworkController controller(miyoofin::Session{}, 1);
    const std::string lowItem = "home-artwork-controller-low";
    const std::string highItem = "home-artwork-controller-high";
    writeTinyCachedPoster(lowItem);
    writeTinyCachedPoster(highItem);
    controller.setLowPriorityDeferred(true);
    const miyoofin::HomePosterJob lowJob{lowItem, miyoofin::ImageType::Primary, "tag", 64, 96};
    const miyoofin::HomePosterJob highJob{highItem, miyoofin::ImageType::Primary, "tag", 64, 96};
    controller.queuePosterJobs({lowJob});
    controller.queuePosterJobs({lowJob});
    CHECK(controller.queuedPosterJobs() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK(controller.artworkProgress().completed == 0);

    // High priority bypasses the population gate while the low item remains
    // queued.  A cache hit completes without requiring an HTTP route.
    controller.queuePosterJobs({highJob}, true);
    CHECK(waitForCompleted(controller, 1));
    CHECK(controller.queuedPosterJobs() == 1);
    CHECK(miyoofin::ImageCache::isCached(highItem, miyoofin::ImageType::Primary, "tag", 64, 96));

    controller.setLowPriorityDeferred(false);
    CHECK(waitForCompleted(controller, 2));
    CHECK(controller.queuedPosterJobs() == 0);
    CHECK(!controller.artworkProgress().active);
    CHECK(miyoofin::ImageCache::isCached(lowItem, miyoofin::ImageType::Primary, "tag", 64, 96));

    controller.requestStopAllWorkers();
    CHECK(controller.admissionClosed());
    controller.queuePosterJobs({lowJob});
    CHECK(controller.queuedPosterJobs() == 0);
    controller.joinAllWorkers();
    CHECK(controller.workersJoined());
    miyoofin::ImageCache::removeCached(lowItem, miyoofin::ImageType::Primary, "tag", 64, 96);
    miyoofin::ImageCache::removeCached(highItem, miyoofin::ImageType::Primary, "tag", 64, 96);
}

static void testLowThenHighPromotesQueuedPoster()
{
    const std::string item = "home-artwork-controller-promote";
    writeTinyCachedPoster(item);
    const miyoofin::HomePosterJob job{item, miyoofin::ImageType::Primary, "tag", 64, 96};
    HomeArtworkController controller(miyoofin::Session{}, 1);
    controller.setLowPriorityDeferred(true);
    controller.queuePosterJobs({job});
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK(controller.artworkProgress().completed == 0);
    CHECK(controller.artworkProgress().total == 1);
    CHECK(controller.queuedPosterJobs() == 1);

    // The duplicate must move, not be rejected as already outstanding: high
    // priority work runs even while low-priority work remains deferred.
    controller.queuePosterJobs({job}, true);
    CHECK(waitForCompleted(controller, 1));
    CHECK(controller.queuedPosterJobs() == 0);
    CHECK(controller.artworkProgress().total == 1);
    CHECK(!controller.artworkProgress().active);
    CHECK(miyoofin::ImageCache::isCached(item, miyoofin::ImageType::Primary, "tag", 64, 96));

    controller.requestStopAllWorkers();
    controller.joinAllWorkers();
    miyoofin::ImageCache::removeCached(item, miyoofin::ImageType::Primary, "tag", 64, 96);
}

static void testMixedHighBatchPreservesPromotionPositions()
{
    HomeArtworkController controller(miyoofin::Session{}, 1, false);
    const miyoofin::HomePosterJob lowA{"batch-a", miyoofin::ImageType::Primary, "tag", 64, 96};
    const miyoofin::HomePosterJob lowC{"batch-c", miyoofin::ImageType::Primary, "tag", 64, 96};
    const miyoofin::HomePosterJob newB{"batch-b", miyoofin::ImageType::Primary, "tag", 64, 96};
    const miyoofin::HomePosterJob newD{"batch-d", miyoofin::ImageType::Primary, "tag", 64, 96};
    controller.setLowPriorityDeferred(true);
    controller.queuePosterJobs({lowA, lowC});
    controller.queuePosterJobs({newB, lowA, newD}, true);

    const std::vector<std::string> expectedHigh = {HomeArtworkController::posterJobKey(newB),
                                                   HomeArtworkController::posterJobKey(lowA),
                                                   HomeArtworkController::posterJobKey(newD)};
    const std::vector<std::string> expectedLow = {HomeArtworkController::posterJobKey(lowC)};
    CHECK(controller.queuedPosterJobKeys(true) == expectedHigh);
    CHECK(controller.queuedPosterJobKeys(false) == expectedLow);
    CHECK(controller.artworkProgress().total == 4);
    CHECK(controller.queuedPosterJobs() == 4);
    controller.requestStopAllWorkers();
    controller.joinAllWorkers();
}

static void testDecodeQueueBoundAndShowsStaleRemoval()
{
    HomeArtworkController controller(miyoofin::Session{}, 1, false);
    for (int i = 0; i < HomeArtworkController::kDecodeQueueLimit + 8; ++i) {
        auto request =
            decodeRequest(("home-artwork-controller-queue-" + std::to_string(i)).c_str());
        controller.requestDecode(std::move(request));
    }
    CHECK(controller.queuedDecodeJobs() == HomeArtworkController::kDecodeQueueLimit);

    controller.setShowsWorkingSet(1, {"show-a:Primary:tag:64x96"});
    auto stale = decodeRequest("show-a", miyoofin::ArtworkContext::HomeShows);
    stale.showsWorkingSetGeneration = 1;
    controller.requestDecode(std::move(stale));
    CHECK(controller.queuedDecodeJobs() == HomeArtworkController::kDecodeQueueLimit);
    // The old queue is full, so use a fresh controller to isolate the
    // queued-generation removal assertion from the capacity assertion.
    controller.requestStopAllWorkers();
    controller.joinAllWorkers();

    HomeArtworkController shows(miyoofin::Session{}, 1, false);
    shows.setShowsWorkingSet(1, {"show-a:Primary:tag:64x96"});
    auto showRequest = decodeRequest("show-a", miyoofin::ArtworkContext::HomeShows);
    showRequest.showsWorkingSetGeneration = 1;
    shows.requestDecode(std::move(showRequest));
    CHECK(shows.queuedDecodeJobs() == 1);
    shows.setShowsWorkingSet(2, {"show-b:Primary:tag:64x96"});
    CHECK(shows.queuedDecodeJobs() == 0);
    shows.requestStopAllWorkers();
    shows.joinAllWorkers();
}

static void testStaleImmutableResultRejection()
{
    HomeArtworkController::DecodeResult result;
    result.identity = {"show", miyoofin::ImageType::Primary, "tag", 64, 96};
    result.context = miyoofin::ArtworkContext::HomeShows;
    result.showsWorkingSetGeneration = 4;
    const std::string key = "show:Primary:tag:64x96";
    CHECK(!HomeScreen::acceptsShowsArtworkResult(result, 5, {key}, {}));
    CHECK(HomeScreen::acceptsShowsArtworkResult(result, 4, {key}, {}));
    CHECK(HomeScreen::acceptsShowsArtworkResult(result, 4, {}, {key}));
    result.context = miyoofin::ArtworkContext::HomeGrid;
    CHECK(HomeScreen::acceptsShowsArtworkResult(result, 999, {}, {}));
}

static void testStaleShowsResultsDoNotConsumeRetryBudget()
{
    const std::string item = "home-artwork-controller-stale-retry";
    const std::string key = item + ":Primary:tag:64x96";
    HomeArtworkController controller(miyoofin::Session{}, 1);

    for (std::uint64_t generation = 1; generation <= 2; ++generation) {
        controller.setShowsWorkingSet(generation, {key});
        auto request = decodeRequest(item.c_str(), miyoofin::ArtworkContext::HomeShows);
        request.showsWorkingSetGeneration = generation;
        controller.requestDecode(std::move(request));
        std::deque<HomeArtworkController::DecodeResult> results;
        CHECK(takeOne(controller, results));

        controller.setShowsWorkingSet(generation + 10, {});
        CHECK(!HomeScreen::acceptsShowsArtworkResult(results.front(), generation + 10, {}, {}));
        controller.completeDecodeResult(results.front(), false);
    }

    const std::uint64_t currentGeneration = 20;
    controller.setShowsWorkingSet(currentGeneration, {key});
    for (int attempt = 1; attempt <= HomeArtworkController::kMaxDecodeAttempts; ++attempt) {
        auto request = decodeRequest(item.c_str(), miyoofin::ArtworkContext::HomeShows);
        request.showsWorkingSetGeneration = currentGeneration;
        controller.requestDecode(std::move(request));
        std::deque<HomeArtworkController::DecodeResult> results;
        CHECK(takeOne(controller, results));
        CHECK(!results.front().cachePresent);
        controller.completeDecodeResult(results.front(), true);
        CHECK(results.front().terminalFailure ==
              (attempt == HomeArtworkController::kMaxDecodeAttempts));
    }
    controller.requestStopAllWorkers();
    controller.joinAllWorkers();
}

static void testCachedDecodeAndOfflineMiss()
{
    const std::string item = "home-artwork-controller-cache";
    const std::string bytes = miyoofin_test::readTestBytes("tests/fixtures/tiny.jpg");
    CHECK(!bytes.empty());
    CHECK(miyoofin::ImageCache::writeToCache(item, miyoofin::ImageType::Primary, "tag", 64, 96,
                                             reinterpret_cast<const unsigned char*>(bytes.data()),
                                             bytes.size()));

    HomeArtworkController controller(miyoofin::Session{}, 1);
    controller.requestDecode(decodeRequest(item.c_str()));
    std::deque<HomeArtworkController::DecodeResult> results;
    CHECK(takeOne(controller, results));
    CHECK(results.front().cachePresent);
    CHECK(!results.front().image.empty());
    CHECK(results.front().identity.itemId == item);
    controller.completeDecodeResult(results.front(), true);

    const std::string missItem = "home-artwork-controller-offline-miss";
    for (int attempt = 1; attempt <= HomeArtworkController::kMaxDecodeAttempts; ++attempt) {
        controller.requestDecode(decodeRequest(missItem.c_str()));
        results.clear();
        CHECK(takeOne(controller, results));
        CHECK(!results.front().cachePresent);
        CHECK(results.front().image.empty());
        controller.completeDecodeResult(results.front(), true);
        CHECK(results.front().terminalFailure ==
              (attempt == HomeArtworkController::kMaxDecodeAttempts));
    }
    CHECK(!miyoofin::ImageCache::isCached(missItem, miyoofin::ImageType::Primary, "tag", 64, 96));
    controller.requestStopAllWorkers();
    controller.joinAllWorkers();
    miyoofin::ImageCache::removeCached(item, miyoofin::ImageType::Primary, "tag", 64, 96);
}

static void testConcurrentStopClosesAdmission()
{
    HomeArtworkController controller(miyoofin::Session{}, 1);
    std::atomic<bool> running{true};
    std::thread producer([&] {
        int i = 0;
        while (running.load()) {
            auto request =
                decodeRequest(("home-artwork-controller-stop-" + std::to_string(i++)).c_str());
            controller.requestDecode(std::move(request));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    controller.requestStopAllWorkers();
    running.store(false);
    producer.join();
    controller.joinAllWorkers();
    CHECK(controller.admissionClosed());
    CHECK(controller.workersJoined());
    CHECK(controller.queuedDecodeJobs() <= HomeArtworkController::kDecodeQueueLimit);
}

static void testCorruptCacheRetryTombstone()
{
    const std::string item = "home-artwork-controller-corrupt";
    const unsigned char corrupt[] = {1, 2, 3, 4};
    HomeArtworkController controller(miyoofin::Session{}, 1);
    for (int attempt = 1; attempt <= HomeArtworkController::kMaxDecodeAttempts; ++attempt) {
        CHECK(miyoofin::ImageCache::writeToCache(item, miyoofin::ImageType::Primary, "tag", 64, 96,
                                                 corrupt, sizeof(corrupt)));
        controller.requestDecode(decodeRequest(item.c_str()));
        std::deque<HomeArtworkController::DecodeResult> results;
        CHECK(takeOne(controller, results));
        CHECK(results.front().cachePresent);
        CHECK(results.front().image.empty());
        controller.completeDecodeResult(results.front(), true);
        CHECK(results.front().terminalFailure ==
              (attempt == HomeArtworkController::kMaxDecodeAttempts));
    }
    controller.requestStopAllWorkers();
    controller.joinAllWorkers();
    miyoofin::ImageCache::removeCached(item, miyoofin::ImageType::Primary, "tag", 64, 96);
}

int main()
{
    testControllerHasNoHomeOrSdlDependency();
    testIdentityAndImmutableResultMetadata();
    testPosterDeferralPriorityCacheHitAndProgress();
    testLowThenHighPromotesQueuedPoster();
    testMixedHighBatchPreservesPromotionPositions();
    testDecodeQueueBoundAndShowsStaleRemoval();
    testStaleImmutableResultRejection();
    testStaleShowsResultsDoNotConsumeRetryBudget();
    testCachedDecodeAndOfflineMiss();
    testCorruptCacheRetryTombstone();
    testConcurrentStopClosesAdmission();
    return miyoofin_test::finish("home-artwork-controller");
}
