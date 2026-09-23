#include <cstdint>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#define MIYOOFIN_TELEMETRY_RING_TEST 1
#include "../../src/diagnostics/TelemetryRing.hpp"
#include "../../src/diagnostics/TelemetryContext.hpp"
#include "../../src/diagnostics/TelemetryGuards.hpp"
#include "../../src/diagnostics/PerformanceTelemetry.hpp"
#include "../../src/diagnostics/TelemetryConfig.hpp"
#include "../../src/diagnostics/TelemetryIds.hpp"
#include "../../src/diagnostics/TelemetryTypes.hpp"
#include "../../src/data/CatalogPrimitives.hpp"
#include "../../src/diagnostics/TelemetryClock.hpp"
#include "../../src/diagnostics/MftFormat.hpp"
#include "../../src/diagnostics/TelemetryWriter.hpp"
#include "../../src/cache/ImageCache.hpp"
#include "../../src/image/ImageDecoder.hpp"
#include "../../src/net/RouteRequest.hpp"
#include "../../src/net/HttpClient.hpp"
#include "../../src/input/Action.hpp"
#if MIYOOFIN_ENABLE_PERF_TELEMETRY
#include "../../src/diagnostics/LinuxProcessMetrics.hpp"
#endif

#include "test_telemetry_core.inc"
#include "test_telemetry_format.inc"
#include "test_telemetry_service.inc"

static void testLibraryStartupTimelineMarkers();

static void testTelemetrySchemaTypes()
{
    std::printf("[test] telemetry schema IDs and record types\n");
    testTelemetryClocks();
    testTelemetryConfigAndFacade();
    testTelemetryContextAndGuards();
    testTelemetryRing();
#if MIYOOFIN_ENABLE_PERF_TELEMETRY
    testMftV1Codec();
    testMftV2CatalogDbTelemetry();
    testTelemetryWriter();
    testTelemetrySegmentAttemptPayload();
    testTelemetryPlaybackEventPayload();
    testTelemetrySecretLeakRegression();
    testTelemetryServiceThread();
    testHttpClientTelemetry();
    std::printf("[test] linux process metrics parsing and validity\n");
    const std::string ioFixture = [] {
        std::ifstream stream("tests/fixtures/telemetry/proc_io_sample.txt");
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }();
    const std::string statmFixture = [] {
        std::ifstream stream("tests/fixtures/telemetry/proc_statm_sample.txt");
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }();
    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;
    uint64_t rssKib = 0;
    CHECK(LinuxProcessMetrics::parseProcIo(ioFixture, readBytes, writeBytes));
    CHECK(readBytes == 345678);
    CHECK(writeBytes == 456789);
    CHECK(LinuxProcessMetrics::parseProcStatm(statmFixture, 4096, rssKib));
    CHECK(rssKib == 8192);
    CHECK(!LinuxProcessMetrics::parseProcIo("read_bytes: malformed\n", readBytes, writeBytes));
    CHECK(!LinuxProcessMetrics::parseProcStatm("4096 malformed\n", 4096, rssKib));

    LinuxProcessMetrics metrics;
    const LinuxProcessMetricsSnapshot first = metrics.sampleFromProcText(
        statmFixture, ioFixture, 100, 64, 1024);
    CHECK((first.validity_flags & (ProcessCpuValid | RssValid | PeakRssValid
                                   | ReadBytesValid | WriteBytesValid | FreeStorageValid))
          == (ProcessCpuValid | RssValid | PeakRssValid
              | ReadBytesValid | WriteBytesValid | FreeStorageValid));
    const std::string regressedIo =
        "read_bytes: 3\nwrite_bytes: 456790\n";
    const LinuxProcessMetricsSnapshot regressed = metrics.sampleFromProcText(
        statmFixture, regressedIo, 101, 64, 1024);
    CHECK((regressed.validity_flags & ReadBytesValid) == 0);
    CHECK((regressed.validity_flags & WriteBytesValid) != 0);
    const LinuxProcessMetricsSnapshot recovered = metrics.sampleFromProcText(
        statmFixture, "read_bytes: 4\nwrite_bytes: 456791\n", 102, 64, 1024);
    CHECK((recovered.validity_flags & (ReadBytesValid | WriteBytesValid))
          == (ReadBytesValid | WriteBytesValid));
    std::printf("[test] linux process metrics parsing and validity OK\n");

    std::printf("[test] telemetry storage path follows configured output\n");
    static const std::string configuredStoragePath = "/tmp/miyoofin-storage-path-"
        + std::to_string(static_cast<long long>(getpid()));
    static std::atomic<bool> configuredPathObserved{false};
    static std::atomic<bool> rootPathObserved{false};
    const auto storagePathMetrics = [](const LinuxProcessMetrics &sampled,
                                        bool includeFreeStorage) noexcept {
        configuredPathObserved.store(sampled.storagePath() == configuredStoragePath,
                                      std::memory_order_relaxed);
        rootPathObserved.store(sampled.storagePath() == "/", std::memory_order_relaxed);
        LinuxProcessMetricsSnapshot snapshot{};
        snapshot.validity_flags = ProcessCpuValid | RssValid | PeakRssValid
            | ReadBytesValid | WriteBytesValid;
        if (includeFreeStorage) {
            snapshot.free_storage_bytes = 256ull * 1024ull * 1024ull;
            snapshot.validity_flags |= FreeStorageValid;
        }
        return snapshot;
    };
    PerformanceTelemetry::setTestHooks({nullptr, storagePathMetrics});
    TelemetryConfig storagePathConfig;
    storagePathConfig.runtimeEnabled = true;
    storagePathConfig.targetDirectory = configuredStoragePath;
    storagePathConfig.sampleIntervalMs = 100;
    storagePathConfig.freeSpaceIntervalMs = 600000;
    storagePathConfig.flushIntervalMs = 1;
    storagePathConfig.minFreeStorageBytes = 128ull * 1024ull * 1024ull;
    PerformanceTelemetry &storagePathTelemetry = performanceTelemetry();
    storagePathTelemetry.stop();
    storagePathTelemetry.start(storagePathConfig);
    for (int attempt = 0; attempt < 100 && !configuredPathObserved.load(std::memory_order_relaxed);
         ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(configuredPathObserved.load(std::memory_order_relaxed));
    CHECK(!rootPathObserved.load(std::memory_order_relaxed));
    storagePathTelemetry.stop();
    std::remove((configuredStoragePath + "/telemetry.mft").c_str());
    ::rmdir(configuredStoragePath.c_str());
    PerformanceTelemetry::clearTestHooks();
    std::printf("[test] telemetry storage path follows configured output OK\n");

    std::printf("[test] periodic system samples and fake service hooks\n");
    static std::atomic<uint64_t> fakeNowUs{1};
    static std::atomic<uint32_t> fakeMetricSamples{0};
    static std::atomic<uint32_t> fakeFreeSamples{0};
    const auto fakeClock = []() noexcept -> uint64_t {
        return fakeNowUs.load(std::memory_order_relaxed);
    };
    const auto fakeMetrics = [](const LinuxProcessMetrics &, bool includeFreeStorage) noexcept {
        LinuxProcessMetricsSnapshot snapshot{};
        snapshot.process_cpu_us_cumulative = 1000;
        snapshot.rss_kib = 2000;
        snapshot.peak_rss_kib = 3000;
        snapshot.process_read_bytes_cumulative = 4000;
        snapshot.process_write_bytes_cumulative = 5000;
        snapshot.validity_flags = ProcessCpuValid | RssValid | PeakRssValid
            | ReadBytesValid | WriteBytesValid;
        fakeMetricSamples.fetch_add(1, std::memory_order_relaxed);
        if (includeFreeStorage) {
            snapshot.free_storage_bytes = 256ull * 1024ull * 1024ull;
            snapshot.validity_flags |= FreeStorageValid;
            fakeFreeSamples.fetch_add(1, std::memory_order_relaxed);
        }
        return snapshot;
    };
    PerformanceTelemetry::setTestHooks({fakeClock, fakeMetrics});
    const std::string sampleRoot = "/tmp/miyoofin-system-sample-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string samplePath = sampleRoot + "/telemetry.mft";
    std::remove(samplePath.c_str());
    ::rmdir(sampleRoot.c_str());
    constexpr std::streamoff aggregateTickBytes = 88 + 14 * 44 + 80 + 56 + 48;
    constexpr std::streamoff decodeRecordBytes = 2 * 48;
    const auto waitForSampleBytes = [&samplePath](std::streamoff expected) {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            struct stat status{};
            if (::stat(samplePath.c_str(), &status) == 0 && status.st_size >= expected)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    TelemetryConfig sampleConfig;
    sampleConfig.runtimeEnabled = true;
    sampleConfig.targetDirectory = sampleRoot;
    sampleConfig.sampleIntervalMs = 100;
    sampleConfig.freeSpaceIntervalMs = 200;
    sampleConfig.flushIntervalMs = 1;
    PerformanceTelemetry &sampleTelemetry = performanceTelemetry();
    sampleTelemetry.stop();
    fakeMetricSamples.store(0, std::memory_order_relaxed);
    fakeFreeSamples.store(0, std::memory_order_relaxed);
    sampleTelemetry.start(sampleConfig);
    for (int attempt = 0; attempt < 100 && !sampleTelemetry.serviceThreadActive(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sampleTelemetry.setWorkerActive(WorkerId::HomeLibraryFetch, true);
    sampleTelemetry.setWorkerQueueDepth(WorkerId::HomeLibraryFetch, 3);
    sampleTelemetry.addWorkerCompleted(WorkerId::HomeLibraryFetch, 2);
    sampleTelemetry.addWorkerFailed(WorkerId::HomeLibraryFetch);
    sampleTelemetry.addWorkerCancelled(WorkerId::HomeLibraryFetch);
    sampleTelemetry.recordArtworkCacheProbe(true);
    sampleTelemetry.recordArtworkCacheRead(true, 100);
    sampleTelemetry.recordArtworkCacheWrite(true, 200);
    sampleTelemetry.recordArtworkDecode(true, 50);
    const std::vector<unsigned char> telemetryValidJpeg = [] {
        std::ifstream stream("tests/fixtures/tiny.jpg", std::ios::binary);
        return std::vector<unsigned char>(
            (std::istreambuf_iterator<char>(stream)),
            std::istreambuf_iterator<char>());
    }();
    const unsigned char telemetryInvalidJpeg[] = {0, 1, 2, 3};
    const DecodedImage telemetryDecoded = ImageDecoder::decodeJpeg(
        telemetryValidJpeg.data(), telemetryValidJpeg.size());
    CHECK(!telemetryDecoded.empty());
    CHECK(ImageDecoder::decodeJpeg(telemetryInvalidJpeg,
                                   sizeof(telemetryInvalidJpeg)).empty());
    const std::string oldImageCacheDir = ImageCache::cacheDir();
    const std::string imageCacheRoot = "/tmp/miyoofin-image-cache-metrics-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string imageCacheItem = "telemetry-cache-item";
    const std::string imageCacheTag = "telemetry-cache-tag";
    ImageCache::setCacheDir(imageCacheRoot + "/");
    const std::string imageCachePath = ImageCache::cachePath(
        imageCacheItem, ImageType::Primary, imageCacheTag, 64, 96);
    std::remove(imageCachePath.c_str());
    const unsigned char imageCacheBytes[] = {1, 2, 3};
    CHECK(!ImageCache::isCached(imageCacheItem, ImageType::Primary,
                                imageCacheTag, 64, 96));
    CHECK(ImageCache::writeToCache(imageCacheItem, ImageType::Primary,
                                   imageCacheTag, 64, 96,
                                   imageCacheBytes, sizeof(imageCacheBytes)));
    CHECK(ImageCache::isCached(imageCacheItem, ImageType::Primary,
                               imageCacheTag, 64, 96));
    CHECK(ImageCache::readCached(imageCacheItem, ImageType::Primary,
                                 imageCacheTag, 64, 96).size()
          == sizeof(imageCacheBytes));
    CHECK(ImageCache::readCached("missing-telemetry-cache-item",
                                 ImageType::Primary, imageCacheTag,
                                 64, 96).empty());
    CHECK(!ImageCache::writeToCache(imageCacheItem, ImageType::Primary,
                                    imageCacheTag, 64, 96, nullptr, 0));
    std::remove(imageCachePath.c_str());
    ::rmdir(imageCacheRoot.c_str());
    ImageCache::setCacheDir(oldImageCacheDir);
    sampleTelemetry.setDownloadGauges(4, 5, 6);
    sampleTelemetry.addDownloadBytes(1000);
    sampleTelemetry.addDownloadSegmentCompleted(2);
    sampleTelemetry.addDownloadSegmentRetries();
    fakeNowUs.store(100001, std::memory_order_relaxed);
    CHECK(waitForSampleBytes(80 + 32 + decodeRecordBytes + aggregateTickBytes));
    sampleTelemetry.setWorkerQueueDepth(WorkerId::HomeLibraryFetch, 1);
    sampleTelemetry.addWorkerCompleted(WorkerId::HomeLibraryFetch, 7);
    sampleTelemetry.recordArtworkCacheProbe(false);
    sampleTelemetry.recordArtworkDecode(false, 20);
    sampleTelemetry.addDownloadBytes(300);
    sampleTelemetry.addDownloadSegmentRetries();
    fakeNowUs.store(200001, std::memory_order_relaxed);
    CHECK(waitForSampleBytes(80 + 32 + decodeRecordBytes + aggregateTickBytes * 2));
    fakeNowUs.store(300001, std::memory_order_relaxed);
    CHECK(waitForSampleBytes(80 + 32 + decodeRecordBytes + aggregateTickBytes * 3));
    sampleTelemetry.stop();
    CHECK(fakeMetricSamples.load(std::memory_order_relaxed) == 4);
    CHECK(fakeFreeSamples.load(std::memory_order_relaxed) == 3);
    std::ifstream sampleFile(samplePath, std::ios::binary);
    sampleFile.seekg(0, std::ios::end);
    CHECK(sampleFile.good());
    CHECK(sampleFile.tellg() == static_cast<std::streamoff>(80 + 32
        + decodeRecordBytes + aggregateTickBytes * 3 + 32));
    sampleFile.close();
    std::ifstream sampleBytesFile(samplePath, std::ios::binary);
    const std::vector<uint8_t> sampleBytes(
        (std::istreambuf_iterator<char>(sampleBytesFile)), std::istreambuf_iterator<char>());
    const auto readU16 = [&sampleBytes](std::size_t offset) {
        return static_cast<uint16_t>(sampleBytes[offset])
            | (static_cast<uint16_t>(sampleBytes[offset + 1]) << 8);
    };
    const auto readU32 = [&sampleBytes](std::size_t offset) {
        return static_cast<uint32_t>(sampleBytes[offset])
            | (static_cast<uint32_t>(sampleBytes[offset + 1]) << 8)
            | (static_cast<uint32_t>(sampleBytes[offset + 2]) << 16)
            | (static_cast<uint32_t>(sampleBytes[offset + 3]) << 24);
    };
    const auto readU64 = [&sampleBytes](std::size_t offset) {
        uint64_t value = 0;
        for (unsigned int byte = 0; byte < 8; ++byte)
            value |= static_cast<uint64_t>(sampleBytes[offset + byte]) << (byte * 8);
        return value;
    };
    const std::size_t tickStart = 80 + 32 + static_cast<std::size_t>(decodeRecordBytes);
    const std::size_t firstWorker = tickStart + 88;
    const std::size_t firstArtwork = firstWorker + 14 * 44;
    const std::size_t firstDownload = firstArtwork + 80;
    const std::size_t secondTick = tickStart + static_cast<std::size_t>(aggregateTickBytes);
    const std::size_t secondWorker = secondTick + 88;
    const std::size_t secondArtwork = secondWorker + 14 * 44;
    const std::size_t secondDownload = secondArtwork + 80;
    CHECK(readU16(firstWorker) == static_cast<uint16_t>(RecordType::WorkerSample));
    CHECK(readU32(firstWorker + 16 + 4) == 3);
    CHECK(readU32(firstWorker + 16 + 8) == 3);
    CHECK(readU32(firstWorker + 16 + 12) == 2);
    CHECK(readU32(firstWorker + 16 + 16) == 1);
    CHECK(readU32(firstWorker + 16 + 20) == 1);
    CHECK(readU32(firstArtwork + 16) == 2);
    CHECK(readU32(firstArtwork + 16 + 4) == 1);
    CHECK(readU32(firstArtwork + 16 + 8) == 2);
    CHECK(readU32(firstArtwork + 16 + 12) == 1);
    CHECK(readU32(firstArtwork + 16 + 16) == 2);
    CHECK(readU32(firstArtwork + 16 + 20) == 1);
    CHECK(readU64(firstArtwork + 16 + 24) == 103);
    CHECK(readU64(firstArtwork + 16 + 32) == 203);
    CHECK(readU32(firstArtwork + 16 + 40) == 3);
    CHECK(readU32(firstArtwork + 16 + 44) == 1);
    CHECK(readU64(firstArtwork + 16 + 48) >= 50);
    CHECK(readU32(firstArtwork + 16 + 56) >= 50);
    CHECK(readU16(firstDownload) == static_cast<uint16_t>(RecordType::DownloadSample));
    CHECK(readU32(firstDownload + 16) == 4);
    CHECK(readU32(firstDownload + 16 + 4) == 5);
    CHECK(readU64(firstDownload + 16 + 8) == 1000);
    CHECK(readU64(firstDownload + 16 + 16) == 10000);
    CHECK(readU32(firstDownload + 16 + 24) == 2);
    CHECK(readU32(firstDownload + 16 + 28) == 1);
    CHECK(readU32(firstDownload + 16 + 32) == 6);
    CHECK(readU32(secondWorker + 16 + 4) == 1);
    CHECK(readU32(secondWorker + 16 + 8) == 1);
    CHECK(readU32(secondWorker + 16 + 12) == 7);
    CHECK(readU32(secondWorker + 16 + 16) == 0);
    CHECK(readU32(secondArtwork + 16) == 0);
    CHECK(readU32(secondArtwork + 16 + 4) == 1);
    CHECK(readU32(secondArtwork + 16 + 40) == 1);
    CHECK(readU32(secondArtwork + 16 + 44) == 1);
    CHECK(readU64(secondArtwork + 16 + 48) == 20);
    CHECK(readU32(secondArtwork + 16 + 56) == 20);
    std::size_t artworkDecodeRecords = 0;
    bool sawValidArtworkDecode = false;
    bool sawInvalidArtworkDecode = false;
    for (std::size_t offset = 80; offset + 16 <= sampleBytes.size();) {
        const uint16_t recordSize = readU16(offset + 2);
        if (recordSize < 16 || offset + recordSize > sampleBytes.size())
            break;
        if (readU16(offset) == static_cast<uint16_t>(RecordType::ArtworkDecode)
            && recordSize >= 48) {
            ++artworkDecodeRecords;
            CHECK(sampleBytes[offset + 16]
                  == static_cast<uint8_t>(ArtworkContext::Unknown));
            const uint8_t outcome = sampleBytes[offset + 17];
            const uint64_t inputBytes = readU64(offset + 28);
            const uint64_t outputBytes = readU64(offset + 36);
            if (inputBytes == telemetryValidJpeg.size()) {
                CHECK(outcome == static_cast<uint8_t>(Outcome::Success));
                CHECK(outputBytes == 4u * 4u * 4u);
                sawValidArtworkDecode = true;
            }
            if (inputBytes == sizeof(telemetryInvalidJpeg)) {
                CHECK(outcome == static_cast<uint8_t>(Outcome::Failure));
                CHECK(outputBytes == 0);
                sawInvalidArtworkDecode = true;
            }
        }
        offset += recordSize;
    }
    CHECK(artworkDecodeRecords == 2);
    CHECK(sawValidArtworkDecode);
    CHECK(sawInvalidArtworkDecode);
    CHECK(readU64(secondDownload + 16 + 8) == 300);
    CHECK(readU64(secondDownload + 16 + 16) == 3000);
    CHECK(readU32(secondDownload + 16 + 24) == 0);
    CHECK(readU32(secondDownload + 16 + 28) == 1);
    CHECK(readU32(secondDownload + 16 + 32) == 6);
    std::remove(samplePath.c_str());
    ::rmdir(sampleRoot.c_str());
    PerformanceTelemetry::clearTestHooks();
    std::printf("[test] periodic system samples and fake service hooks OK\n");

    std::printf("[test] home library sync telemetry completion\n");
    const std::string syncRoot = "/tmp/miyoofin-home-library-sync-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string syncPath = syncRoot + "/telemetry.mft";
    std::remove(syncPath.c_str());
    ::rmdir(syncRoot.c_str());
    TelemetryConfig syncConfig = sampleConfig;
    syncConfig.targetDirectory = syncRoot;
    fakeNowUs.store(1, std::memory_order_relaxed);
    fakeMetricSamples.store(0, std::memory_order_relaxed);
    PerformanceTelemetry &syncTelemetry = performanceTelemetry();
    syncTelemetry.start(syncConfig);
    Session syncSession;
    syncSession.serverUrl = "http://127.0.0.1:1";
    syncSession.accessToken = "test-token";
    syncSession.userId = "test-user";
    syncSession.deviceId = "test-device";
    {
        HomeScreen home(syncSession);
        home.enter();
        for (int attempt = 0; attempt < 200; ++attempt) {
            home.update(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    fakeNowUs.store(100001, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 100
        && fakeMetricSamples.load(std::memory_order_relaxed) < 2; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    syncTelemetry.stop();
    std::ifstream syncFile(syncPath, std::ios::binary);
    const std::vector<uint8_t> syncBytes(
        (std::istreambuf_iterator<char>(syncFile)), std::istreambuf_iterator<char>());
    std::size_t librarySyncCount = 0;
    bool librarySyncFailure = false;
    uint32_t librarySyncRequests = 0;
    bool libraryWorkerSawInactive = false;
    for (std::size_t offset = 80; offset + 16 <= syncBytes.size();) {
        const uint16_t recordSize = static_cast<uint16_t>(syncBytes[offset + 2])
            | (static_cast<uint16_t>(syncBytes[offset + 3]) << 8);
        if (recordSize < 16 || offset + recordSize > syncBytes.size())
            break;
        const uint16_t recordType = static_cast<uint16_t>(syncBytes[offset])
            | (static_cast<uint16_t>(syncBytes[offset + 1]) << 8);
        if (recordType == static_cast<uint16_t>(RecordType::LibrarySync)
            && recordSize >= 48) {
            ++librarySyncCount;
            librarySyncFailure = syncBytes[offset + 24]
                == static_cast<uint8_t>(Outcome::Failure);
            librarySyncRequests = static_cast<uint32_t>(syncBytes[offset + 40])
                | (static_cast<uint32_t>(syncBytes[offset + 41]) << 8)
                | (static_cast<uint32_t>(syncBytes[offset + 42]) << 16)
                | (static_cast<uint32_t>(syncBytes[offset + 43]) << 24);
        } else if (recordType == static_cast<uint16_t>(RecordType::WorkerSample)
                   && recordSize >= 44 && offset + 20 <= syncBytes.size()) {
            const std::size_t payload = offset + 16;
            const uint16_t workerId = static_cast<uint16_t>(syncBytes[payload])
                | (static_cast<uint16_t>(syncBytes[payload + 1]) << 8);
            if (workerId == static_cast<uint16_t>(WorkerId::HomeLibraryFetch))
                libraryWorkerSawInactive = syncBytes[payload + 2] == 0;
        }
        offset += recordSize;
    }
    CHECK(librarySyncCount == 1);
    CHECK(librarySyncFailure);
    CHECK(librarySyncRequests == 0);
    CHECK(libraryWorkerSawInactive);
    std::remove(syncPath.c_str());
    ::rmdir(syncRoot.c_str());
    PerformanceTelemetry::clearTestHooks();
    std::printf("[test] home library sync telemetry completion OK\n");

    std::printf("[test] screen retirement gauges and nonblocking pop\n");
    const std::string retirementRoot = "/tmp/miyoofin-screen-retirement-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string retirementPath = retirementRoot + "/telemetry.mft";
    std::remove(retirementPath.c_str());
    ::rmdir(retirementRoot.c_str());
    TelemetryConfig retirementConfig = sampleConfig;
    retirementConfig.targetDirectory = retirementRoot;
    fakeNowUs.store(1, std::memory_order_relaxed);
    fakeMetricSamples.store(0, std::memory_order_relaxed);
    PerformanceTelemetry &retirementTelemetry = performanceTelemetry();
    retirementTelemetry.start(retirementConfig);
    for (int attempt = 0; attempt < 100 && !retirementTelemetry.serviceThreadActive(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    class RetirementGaugeScreen final : public Screen {
    public:
        explicit RetirementGaugeScreen(bool deferred) : m_deferred(deferred) {}
        ~RetirementGaugeScreen() override { if (m_deferred) usleep(150000); }
        bool handleAction(Action) override { return false; }
        void update(Uint32) override {}
        void render(SDL_Surface *) override {}
        bool deferDestruction() const override { return m_deferred; }
    private:
        bool m_deferred;
    };
    {
        ScreenStack stack;
        stack.push(std::unique_ptr<Screen>(new RetirementGaugeScreen(false)));
        stack.push(std::unique_ptr<Screen>(new RetirementGaugeScreen(true)));
        stack.push(std::unique_ptr<Screen>(new RetirementGaugeScreen(true)));
        const auto retirementPopStart = std::chrono::steady_clock::now();
        CHECK(stack.pop());
        CHECK(stack.pop());
        const auto retirementPopElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - retirementPopStart).count();
        CHECK(retirementPopElapsed < 100);
        CHECK(stack.size() == 1);
        fakeNowUs.store(100001, std::memory_order_relaxed);
        for (int attempt = 0; attempt < 100
            && fakeMetricSamples.load(std::memory_order_relaxed) < 2; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    fakeNowUs.store(200001, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 100
        && fakeMetricSamples.load(std::memory_order_relaxed) < 3; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    retirementTelemetry.stop();
    std::ifstream retirementFile(retirementPath, std::ios::binary);
    const std::vector<uint8_t> retirementBytes(
        (std::istreambuf_iterator<char>(retirementFile)), std::istreambuf_iterator<char>());
    bool retirementSawActive = false;
    bool retirementSawQueued = false;
    uint32_t retirementCompletedTotal = 0;
    for (std::size_t offset = 80; offset + 16 <= retirementBytes.size();) {
        const uint16_t recordSize = static_cast<uint16_t>(retirementBytes[offset + 2])
            | (static_cast<uint16_t>(retirementBytes[offset + 3]) << 8);
        if (recordSize < 16 || offset + recordSize > retirementBytes.size())
            break;
        if ((static_cast<uint16_t>(retirementBytes[offset])
                | (static_cast<uint16_t>(retirementBytes[offset + 1]) << 8))
                == static_cast<uint16_t>(RecordType::WorkerSample)
            && recordSize >= 44) {
            const std::size_t payload = offset + 16;
            const uint16_t workerId = static_cast<uint16_t>(retirementBytes[payload])
                | (static_cast<uint16_t>(retirementBytes[payload + 1]) << 8);
            if (workerId == static_cast<uint16_t>(WorkerId::ScreenRetirement)) {
                retirementSawActive = retirementSawActive || retirementBytes[payload + 2] != 0;
                retirementSawQueued = retirementSawQueued
                    || (static_cast<uint32_t>(retirementBytes[payload + 4])
                        | (static_cast<uint32_t>(retirementBytes[payload + 5]) << 8)
                        | (static_cast<uint32_t>(retirementBytes[payload + 6]) << 16)
                        | (static_cast<uint32_t>(retirementBytes[payload + 7]) << 24)) != 0;
                retirementCompletedTotal += static_cast<uint32_t>(retirementBytes[payload + 12])
                    | (static_cast<uint32_t>(retirementBytes[payload + 13]) << 8)
                    | (static_cast<uint32_t>(retirementBytes[payload + 14]) << 16)
                    | (static_cast<uint32_t>(retirementBytes[payload + 15]) << 24);
            }
        }
        offset += recordSize;
    }
    CHECK(retirementSawActive);
    CHECK(retirementSawQueued);
    CHECK(retirementCompletedTotal >= 2);
    std::remove(retirementPath.c_str());
    ::rmdir(retirementRoot.c_str());
    PerformanceTelemetry::clearTestHooks();
    std::printf("[test] screen retirement gauges and nonblocking pop OK\n");

    std::printf("[test] low-storage startup and active shutdown\n");
    static std::atomic<uint64_t> lowStorageNowUs{1};
    static std::atomic<uint32_t> lowStorageSamples{0};
    const auto lowStorageClock = []() noexcept -> uint64_t {
        return lowStorageNowUs.load(std::memory_order_relaxed);
    };
    const auto lowStorageMetrics = [](const LinuxProcessMetrics &, bool includeFreeStorage) noexcept {
        LinuxProcessMetricsSnapshot snapshot{};
        snapshot.process_cpu_us_cumulative = 1000;
        snapshot.rss_kib = 2000;
        snapshot.peak_rss_kib = 3000;
        snapshot.process_read_bytes_cumulative = 4000;
        snapshot.process_write_bytes_cumulative = 5000;
        snapshot.validity_flags = ProcessCpuValid | RssValid | PeakRssValid
            | ReadBytesValid | WriteBytesValid;
        lowStorageSamples.fetch_add(1, std::memory_order_relaxed);
        if (includeFreeStorage) {
            snapshot.free_storage_bytes = 64ull * 1024ull * 1024ull;
            snapshot.validity_flags |= FreeStorageValid;
        }
        return snapshot;
    };
    PerformanceTelemetry::setTestHooks({lowStorageClock, lowStorageMetrics});
    const std::string startupLowRoot = "/tmp/miyoofin-low-storage-startup-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string startupLowPath = startupLowRoot + "/telemetry.mft";
    std::remove(startupLowPath.c_str());
    ::rmdir(startupLowRoot.c_str());
    TelemetryConfig startupLowConfig;
    startupLowConfig.runtimeEnabled = true;
    startupLowConfig.targetDirectory = startupLowRoot;
    startupLowConfig.sampleIntervalMs = 100;
    startupLowConfig.freeSpaceIntervalMs = 200;
    startupLowConfig.flushIntervalMs = 1;
    PerformanceTelemetry &lowStorageTelemetry = performanceTelemetry();
    lowStorageTelemetry.stop();
    lowStorageTelemetry.start(startupLowConfig);
    for (int attempt = 0; attempt < 100
        && (lowStorageTelemetry.enabledFast() || lowStorageTelemetry.serviceThreadActive()); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!lowStorageTelemetry.enabledFast());
    CHECK(!lowStorageTelemetry.serviceThreadActive());
    CHECK(lowStorageSamples.load(std::memory_order_relaxed) == 1);
    CHECK(::access(startupLowPath.c_str(), F_OK) != 0);
    CHECK(::access(startupLowRoot.c_str(), F_OK) != 0);
    lowStorageTelemetry.stop();
    PerformanceTelemetry::clearTestHooks();

    static std::atomic<uint64_t> activeLowNowUs{1};
    static std::atomic<uint32_t> activeLowSamples{0};
    const auto activeLowClock = []() noexcept -> uint64_t {
        return activeLowNowUs.load(std::memory_order_relaxed);
    };
    const auto activeLowMetrics = [](const LinuxProcessMetrics &, bool includeFreeStorage) noexcept {
        LinuxProcessMetricsSnapshot snapshot{};
        snapshot.process_cpu_us_cumulative = 1000;
        snapshot.rss_kib = 2000;
        snapshot.peak_rss_kib = 3000;
        snapshot.process_read_bytes_cumulative = 4000;
        snapshot.process_write_bytes_cumulative = 5000;
        snapshot.validity_flags = ProcessCpuValid | RssValid | PeakRssValid
            | ReadBytesValid | WriteBytesValid;
        const uint32_t sample = activeLowSamples.fetch_add(1, std::memory_order_relaxed) + 1;
        if (includeFreeStorage) {
            snapshot.free_storage_bytes = sample >= 3
                ? 64ull * 1024ull * 1024ull : 256ull * 1024ull * 1024ull;
            snapshot.validity_flags |= FreeStorageValid;
        }
        return snapshot;
    };
    PerformanceTelemetry::setTestHooks({activeLowClock, activeLowMetrics});
    const std::string activeLowRoot = "/tmp/miyoofin-low-storage-active-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string activeLowPath = activeLowRoot + "/telemetry.mft";
    std::remove(activeLowPath.c_str());
    ::rmdir(activeLowRoot.c_str());
    TelemetryConfig activeLowConfig = startupLowConfig;
    activeLowConfig.targetDirectory = activeLowRoot;
    activeLowConfig.freeSpaceIntervalMs = 100;
    lowStorageTelemetry.start(activeLowConfig);
    for (int attempt = 0; attempt < 100 && !lowStorageTelemetry.serviceThreadActive(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    activeLowNowUs.store(100001, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 100
        && activeLowSamples.load(std::memory_order_relaxed) < 2; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    activeLowNowUs.store(200001, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 100
        && (lowStorageTelemetry.enabledFast() || lowStorageTelemetry.serviceThreadActive()); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lowStorageTelemetry.stop();
    CHECK(activeLowSamples.load(std::memory_order_relaxed) == 3);
    CHECK(!lowStorageTelemetry.enabledFast());
    CHECK(!lowStorageTelemetry.serviceThreadActive());
    std::ifstream activeLowFile(activeLowPath, std::ios::binary);
    const std::vector<uint8_t> activeLowBytes(
        (std::istreambuf_iterator<char>(activeLowFile)), std::istreambuf_iterator<char>());
    constexpr std::size_t lowStorageAggregateTickBytes = 88 + 14 * 44 + 80 + 56 + 48;
    constexpr std::size_t lowEventOffset = 80 + 32 + lowStorageAggregateTickBytes;
    constexpr std::size_t expectedLowSize = lowEventOffset + 32 + 48;
    CHECK(activeLowBytes.size() == expectedLowSize);
    if (activeLowBytes.size() >= expectedLowSize) {
        const auto readU16 = [&activeLowBytes](std::size_t offset) {
            return static_cast<uint16_t>(activeLowBytes[offset])
                | (static_cast<uint16_t>(activeLowBytes[offset + 1]) << 8);
        };
        const auto readU32 = [&activeLowBytes](std::size_t offset) {
            return static_cast<uint32_t>(activeLowBytes[offset])
                | (static_cast<uint32_t>(activeLowBytes[offset + 1]) << 8)
                | (static_cast<uint32_t>(activeLowBytes[offset + 2]) << 16)
                | (static_cast<uint32_t>(activeLowBytes[offset + 3]) << 24);
        };
        const auto readU64 = [&activeLowBytes](std::size_t offset) {
            uint64_t value = 0;
            for (unsigned int byte = 0; byte < 8; ++byte)
                value |= static_cast<uint64_t>(activeLowBytes[offset + byte]) << (byte * 8);
            return value;
        };
        CHECK(readU16(lowEventOffset) == static_cast<uint16_t>(RecordType::SessionEvent));
        CHECK(activeLowBytes[lowEventOffset + 16]
              == static_cast<uint8_t>(SessionEventKind::WriterDisabledLowSpace));
        CHECK(activeLowBytes[lowEventOffset + 17] == static_cast<uint8_t>(Outcome::Skipped));
        CHECK(readU32(lowEventOffset + 20) == 128);
        CHECK(readU64(lowEventOffset + 24) == 64ull * 1024ull * 1024ull);
        CHECK(readU16(lowEventOffset + 32) == static_cast<uint16_t>(RecordType::TelemetryHealth));
    }
    std::ifstream activeLowFileAgain(activeLowPath, std::ios::binary);
    activeLowFileAgain.seekg(0, std::ios::end);
    CHECK(activeLowFileAgain.tellg() == static_cast<std::streamoff>(expectedLowSize));
    std::remove(activeLowPath.c_str());
    ::rmdir(activeLowRoot.c_str());
    PerformanceTelemetry::clearTestHooks();
    std::printf("[test] low-storage startup and active shutdown OK\n");

    std::printf("[test] frame timing aggregation boundaries and interval reset\n");
    static std::atomic<uint64_t> frameNowUs{1};
    static std::atomic<uint32_t> frameMetricSamples{0};
    const auto frameClock = []() noexcept -> uint64_t {
        return frameNowUs.load(std::memory_order_relaxed);
    };
    const auto frameMetrics = [](const LinuxProcessMetrics &, bool) noexcept {
        LinuxProcessMetricsSnapshot snapshot{};
        snapshot.validity_flags = ProcessCpuValid | RssValid | PeakRssValid
            | ReadBytesValid | WriteBytesValid;
        frameMetricSamples.fetch_add(1, std::memory_order_relaxed);
        return snapshot;
    };
    PerformanceTelemetry::setTestHooks({frameClock, frameMetrics});
    const std::string frameRoot = "/tmp/miyoofin-frame-aggregation-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string framePath = frameRoot + "/telemetry.mft";
    std::remove(framePath.c_str());
    ::rmdir(frameRoot.c_str());
    TelemetryConfig frameConfig;
    frameConfig.runtimeEnabled = true;
    frameConfig.targetDirectory = frameRoot;
    frameConfig.sampleIntervalMs = 100;
    frameConfig.flushIntervalMs = 1;
    PerformanceTelemetry &frameTelemetry = performanceTelemetry();
    frameTelemetry.stop();
    frameMetricSamples.store(0, std::memory_order_relaxed);
    frameTelemetry.start(frameConfig);
    for (int attempt = 0; attempt < 100 && !frameTelemetry.serviceThreadActive(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const uint64_t boundaryDurations[] = {
        8333, 16667, 25000, 33333, 50000, 100000, 250000, 500000, 500001
    };
    for (uint64_t durationUs : boundaryDurations)
        frameTelemetry.recordFramePhase(FramePhase::FullFrame, durationUs);
    frameNowUs.store(100001, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 100
        && frameMetricSamples.load(std::memory_order_relaxed) < 2; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    frameTelemetry.recordFramePhase(FramePhase::Input, 1234);
    frameNowUs.store(200001, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 100
        && frameMetricSamples.load(std::memory_order_relaxed) < 3; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    frameTelemetry.stop();

    std::ifstream frameFile(framePath, std::ios::binary);
    const std::vector<uint8_t> frameBytes(
        (std::istreambuf_iterator<char>(frameFile)), std::istreambuf_iterator<char>());
    const auto frameReadU16 = [&frameBytes](std::size_t offset) {
        return static_cast<uint16_t>(frameBytes[offset])
            | (static_cast<uint16_t>(frameBytes[offset + 1]) << 8);
    };
    const auto frameReadU32 = [&frameBytes](std::size_t offset) {
        return static_cast<uint32_t>(frameBytes[offset])
            | (static_cast<uint32_t>(frameBytes[offset + 1]) << 8)
            | (static_cast<uint32_t>(frameBytes[offset + 2]) << 16)
            | (static_cast<uint32_t>(frameBytes[offset + 3]) << 24);
    };
    const auto frameReadU64 = [&frameBytes](std::size_t offset) {
        uint64_t value = 0;
        for (unsigned int byte = 0; byte < 8; ++byte)
            value |= static_cast<uint64_t>(frameBytes[offset + byte]) << (byte * 8);
        return value;
    };
    std::vector<std::size_t> frameSummaryOffsets;
    for (std::size_t offset = 80; offset + 16 <= frameBytes.size();) {
        const uint16_t recordType = frameReadU16(offset);
        const uint16_t recordSize = frameReadU16(offset + 2);
        CHECK(recordSize >= 16);
        if (recordSize < 16 || offset + recordSize > frameBytes.size())
            break;
        if (recordType == static_cast<uint16_t>(RecordType::FrameTimingSummary))
            frameSummaryOffsets.push_back(offset);
        offset += recordSize;
    }
    CHECK(frameSummaryOffsets.size() == 2);
    if (frameSummaryOffsets.size() == 2) {
        const std::size_t first = frameSummaryOffsets[0] + 16;
        CHECK(frameBytes[first] == static_cast<uint8_t>(FramePhase::FullFrame));
        CHECK(frameReadU32(first + 4) == 100000);
        CHECK(frameReadU32(first + 8) == 9);
        CHECK(frameReadU64(first + 12)
              == 8333ull + 16667ull + 25000ull + 33333ull + 50000ull
                 + 100000ull + 250000ull + 500000ull + 500001ull);
        CHECK(frameReadU32(first + 20) == 500001);
        CHECK(frameReadU32(first + 24) == 4);
        CHECK(frameReadU32(first + 28) == 3);
        for (uint32_t bin = 0; bin < 9; ++bin)
            CHECK(frameReadU32(first + 32 + bin * 4) == 1);

        const std::size_t second = frameSummaryOffsets[1] + 16;
        CHECK(frameBytes[second] == static_cast<uint8_t>(FramePhase::Input));
        CHECK(frameReadU32(second + 4) == 100000);
        CHECK(frameReadU32(second + 8) == 1);
        CHECK(frameReadU64(second + 12) == 1234);
        CHECK(frameReadU32(second + 20) == 1234);
        CHECK(frameReadU32(second + 24) == 0);
        CHECK(frameReadU32(second + 28) == 0);
        CHECK(frameReadU32(second + 32) == 1);
        for (uint32_t bin = 1; bin < 9; ++bin)
            CHECK(frameReadU32(second + 32 + bin * 4) == 0);
    }
    std::remove(framePath.c_str());
    ::rmdir(frameRoot.c_str());
    PerformanceTelemetry::clearTestHooks();
    std::printf("[test] frame timing aggregation boundaries and interval reset OK\n");

    std::printf("[test] telemetry state mapping and duplicate suppression\n");
    CHECK(PerformanceTelemetry::screenIdFromDiagnosticName("HomeScreen") == ScreenId::Home);
    CHECK(PerformanceTelemetry::screenIdFromDiagnosticName("unrecognized") == ScreenId::Other);
    CHECK(PerformanceTelemetry::tabIdFromDiagnosticName("Movies") == TabId::Movies);
    CHECK(PerformanceTelemetry::tabIdFromDiagnosticName("unrecognized") == TabId::Other);
    CHECK(PerformanceTelemetry::actionIdFromAction(Action::Confirm) == ActionId::Confirm);
    CHECK(PerformanceTelemetry::actionIdFromAction(static_cast<Action>(255)) == ActionId::Other);

    static std::atomic<uint64_t> stateNowUs{1};
    static std::atomic<uint32_t> stateMetricSamples{0};
    const auto stateClock = []() noexcept -> uint64_t {
        return stateNowUs.load(std::memory_order_relaxed);
    };
    const auto stateMetrics = [](const LinuxProcessMetrics &, bool) noexcept {
        LinuxProcessMetricsSnapshot snapshot{};
        snapshot.validity_flags = ProcessCpuValid | RssValid | PeakRssValid
            | ReadBytesValid | WriteBytesValid;
        stateMetricSamples.fetch_add(1, std::memory_order_relaxed);
        return snapshot;
    };
    PerformanceTelemetry::setTestHooks({stateClock, stateMetrics});
    const std::string stateRoot = "/tmp/miyoofin-state-transitions-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string statePath = stateRoot + "/telemetry.mft";
    std::remove(statePath.c_str());
    ::rmdir(stateRoot.c_str());
    TelemetryConfig stateConfig;
    stateConfig.runtimeEnabled = true;
    stateConfig.targetDirectory = stateRoot;
    stateConfig.sampleIntervalMs = 100;
    stateConfig.flushIntervalMs = 1;
    PerformanceTelemetry &stateTelemetry = performanceTelemetry();
    stateTelemetry.stop();
    stateMetricSamples.store(0, std::memory_order_relaxed);
    stateTelemetry.start(stateConfig);
    for (int attempt = 0; attempt < 100 && !stateTelemetry.serviceThreadActive(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    stateTelemetry.setScreen(ScreenId::Home);
    stateTelemetry.setScreen(ScreenId::Home);
    stateTelemetry.setScreen(ScreenId::Series);
    stateTelemetry.setTab(TabId::Movies);
    stateTelemetry.setTab(TabId::Movies);
    stateTelemetry.setAction(ActionId::Confirm);
    stateTelemetry.setAction(ActionId::Confirm);
    stateTelemetry.setPlaybackState(PlaybackState::UiActive);
    stateTelemetry.setPlaybackState(PlaybackState::UiActive);
    stateTelemetry.stop();

    std::ifstream stateFile(statePath, std::ios::binary);
    const std::vector<uint8_t> stateBytes(
        (std::istreambuf_iterator<char>(stateFile)), std::istreambuf_iterator<char>());
    const auto stateReadU16 = [&stateBytes](std::size_t offset) {
        return static_cast<uint16_t>(stateBytes[offset])
            | (static_cast<uint16_t>(stateBytes[offset + 1]) << 8);
    };
    const auto stateReadU32 = [&stateBytes](std::size_t offset) {
        return static_cast<uint32_t>(stateBytes[offset])
            | (static_cast<uint32_t>(stateBytes[offset + 1]) << 8)
            | (static_cast<uint32_t>(stateBytes[offset + 2]) << 16)
            | (static_cast<uint32_t>(stateBytes[offset + 3]) << 24);
    };
    std::vector<std::size_t> stateOffsets;
    for (std::size_t offset = 80; offset + 16 <= stateBytes.size();) {
        const uint16_t recordSize = stateReadU16(offset + 2);
        CHECK(recordSize >= 16);
        if (recordSize < 16 || offset + recordSize > stateBytes.size())
            break;
        if (stateReadU16(offset) == static_cast<uint16_t>(RecordType::StateTransition))
            stateOffsets.push_back(offset);
        offset += recordSize;
    }
    CHECK(stateOffsets.size() == 5);
    if (stateOffsets.size() == 5) {
        CHECK(stateBytes[stateOffsets[0] + 16] == static_cast<uint8_t>(StateKind::Screen));
        CHECK(stateReadU16(stateOffsets[0] + 18) == static_cast<uint16_t>(ScreenId::None));
        CHECK(stateReadU16(stateOffsets[0] + 20) == static_cast<uint16_t>(ScreenId::Home));
        CHECK(stateReadU32(stateOffsets[0] + 24) == 1);
        CHECK(stateReadU16(stateOffsets[1] + 18) == static_cast<uint16_t>(ScreenId::Home));
        CHECK(stateReadU16(stateOffsets[1] + 20) == static_cast<uint16_t>(ScreenId::Series));
        CHECK(stateBytes[stateOffsets[2] + 16] == static_cast<uint8_t>(StateKind::Tab));
        CHECK(stateReadU16(stateOffsets[2] + 18) == static_cast<uint16_t>(TabId::NotApplicable));
        CHECK(stateReadU16(stateOffsets[2] + 20) == static_cast<uint16_t>(TabId::Movies));
        CHECK(stateBytes[stateOffsets[3] + 16] == static_cast<uint8_t>(StateKind::Action));
        CHECK(stateReadU16(stateOffsets[3] + 18) == static_cast<uint16_t>(ActionId::None));
        CHECK(stateReadU16(stateOffsets[3] + 20) == static_cast<uint16_t>(ActionId::Confirm));
        CHECK(stateBytes[stateOffsets[4] + 16]
              == static_cast<uint8_t>(StateKind::PlaybackState));
        CHECK(stateReadU16(stateOffsets[4] + 18) == static_cast<uint16_t>(PlaybackState::Unknown));
        CHECK(stateReadU16(stateOffsets[4] + 20) == static_cast<uint16_t>(PlaybackState::UiActive));
        for (std::size_t index = 0; index < stateOffsets.size(); ++index)
            CHECK(stateReadU32(stateOffsets[index] + 24) == index + 1);
    }
    std::remove(statePath.c_str());
    ::rmdir(stateRoot.c_str());
    PerformanceTelemetry::clearTestHooks();
    std::printf("[test] telemetry state mapping and duplicate suppression OK\n");

    std::printf("[test] UI diagnostics telemetry mappings and worker mask\n");
    CHECK(UiDiagnostics::phaseIdFromDiagnosticName("idle/frame boundary")
          == UiPhaseId::IdleFrameBoundary);
    CHECK(UiDiagnostics::phaseIdFromDiagnosticName("event/input") == UiPhaseId::EventInput);
    CHECK(UiDiagnostics::phaseIdFromDiagnosticName("update") == UiPhaseId::Update);
    CHECK(UiDiagnostics::phaseIdFromDiagnosticName("screen transition")
          == UiPhaseId::ScreenTransition);
    CHECK(UiDiagnostics::phaseIdFromDiagnosticName("render") == UiPhaseId::Render);
    CHECK(UiDiagnostics::phaseIdFromDiagnosticName("unknown") == UiPhaseId::Unknown);
    CHECK(UiDiagnostics::scopeIdFromDiagnosticName("Screen::handleAction")
          == UiScopeId::ScreenHandleAction);
    CHECK(UiDiagnostics::scopeIdFromDiagnosticName("Screen::update") == UiScopeId::ScreenUpdate);
    CHECK(UiDiagnostics::scopeIdFromDiagnosticName("ScreenStack::retireScreen")
          == UiScopeId::ScreenStackRetireScreen);
    CHECK(UiDiagnostics::scopeIdFromDiagnosticName("DownloadManager::planSnapshot mutex wait")
          == UiScopeId::DownloadManagerPlanSnapshotMutexWait);
    CHECK(UiDiagnostics::scopeIdFromDiagnosticName("unknown") == UiScopeId::Unknown);
    UiDiagnostics diagnostics;
    diagnostics.setWorker("library", "active");
    diagnostics.setWorker("hierarchy", "active");
    diagnostics.setWorker("artwork", "active");
    diagnostics.setWorker("download", "active");
    CHECK(diagnostics.activeWorkerMask()
          == (kWorkerMaskHomeLibraryFetch | kWorkerMaskHomeHierarchy
              | kWorkerMaskHomePoster | kWorkerMaskDownloadTransfer));
    diagnostics.setWorker("hierarchy", "idle");
    CHECK(diagnostics.activeWorkerMask()
          == (kWorkerMaskHomeLibraryFetch | kWorkerMaskHomePoster
              | kWorkerMaskDownloadTransfer));
    diagnostics.setWorker("library", "idle");
    diagnostics.setWorker("artwork", "idle");
    diagnostics.setWorker("download", "idle");
    CHECK(diagnostics.activeWorkerMask() == 0);
    std::printf("[test] UI diagnostics telemetry mappings and worker mask OK\n");
#endif
#include "test_telemetry_schema_tail.inc"
