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

static void testTelemetryClocks()
{
    std::printf("[test] telemetry monotonic and process CPU clocks\n");
    const uint64_t monotonic_before = TelemetryClock::monotonicUs();
    const uint64_t monotonic_after = TelemetryClock::monotonicUs();
    const uint64_t cpu_before = TelemetryClock::processCpuUs();
    const uint64_t cpu_after = TelemetryClock::processCpuUs();
    CHECK(monotonic_before != 0);
    CHECK(monotonic_after >= monotonic_before);
    CHECK(cpu_before != 0);
    CHECK(cpu_after >= cpu_before);
    std::printf("[test] telemetry monotonic and process CPU clocks OK\n");
}

static void testTelemetryConfigAndFacade()
{
    std::printf("[test] telemetry config and runtime facade\n");
    const char *savedTelemetry = std::getenv("MIYOOFIN_TELEMETRY");
    const char *savedMinFree = std::getenv("MIYOOFIN_TELEMETRY_MIN_FREE_MIB");
    const std::string telemetryValue = savedTelemetry ? savedTelemetry : "";
    const std::string minFreeValue = savedMinFree ? savedMinFree : "";
    const bool hadTelemetry = savedTelemetry != nullptr;
    const bool hadMinFree = savedMinFree != nullptr;

    unsetenv("MIYOOFIN_TELEMETRY");
    unsetenv("MIYOOFIN_TELEMETRY_MIN_FREE_MIB");
    const TelemetryConfig defaults = TelemetryConfig::fromEnvironment();
    CHECK(!defaults.runtimeEnabled);
    CHECK(defaults.sampleIntervalMs == 1000);
    CHECK(defaults.freeSpaceIntervalMs == 10000);
    CHECK(defaults.writerBufferBytes == 32768);
    CHECK(defaults.flushIntervalMs == 5000);
    CHECK(defaults.rotateBytes == 16ull * 1024ull * 1024ull);
    CHECK(defaults.retainFiles == 4);
    CHECK(defaults.minFreeStorageBytes == 128ull * 1024ull * 1024ull);
    CHECK(defaults.targetDirectory == "/tmp/miyoofin-telemetry-test");

#if MIYOOFIN_ENABLE_PERF_TELEMETRY
    setenv("MIYOOFIN_TELEMETRY", "01", 1);
    CHECK(!TelemetryConfig::fromEnvironment().runtimeEnabled);
    setenv("MIYOOFIN_TELEMETRY", "1", 1);
    CHECK(TelemetryConfig::fromEnvironment().runtimeEnabled);

    setenv("MIYOOFIN_TELEMETRY_MIN_FREE_MIB", "32", 1);
    CHECK(TelemetryConfig::fromEnvironment().minFreeStorageBytes == 64ull * 1024ull * 1024ull);
    setenv("MIYOOFIN_TELEMETRY_MIN_FREE_MIB", "8192", 1);
    CHECK(TelemetryConfig::fromEnvironment().minFreeStorageBytes == 4096ull * 1024ull * 1024ull);
    setenv("MIYOOFIN_TELEMETRY_MIN_FREE_MIB", "not-a-number", 1);
    CHECK(TelemetryConfig::fromEnvironment().minFreeStorageBytes == 128ull * 1024ull * 1024ull);
#endif

    PerformanceTelemetry &telemetry = performanceTelemetry();
    telemetry.stop();
    CHECK(!telemetry.enabledFast());
    TelemetryConfig enabled = defaults;
    enabled.runtimeEnabled = true;
    telemetry.start(enabled);
#if MIYOOFIN_ENABLE_PERF_TELEMETRY
    CHECK(telemetry.enabledFast());
    CHECK(telemetry.nextEphemeralId() != 0);
    CHECK(telemetry.nextEphemeralId() != 0);
    telemetry.suspendSampling(true, SamplingReason::ExternalPlayback);
    CHECK(telemetry.enabledFast());
#else
    CHECK(!telemetry.enabledFast());
    CHECK(telemetry.nextEphemeralId() == 0);
#endif
    telemetry.stop();
    CHECK(!telemetry.enabledFast());

    if (hadTelemetry)
        setenv("MIYOOFIN_TELEMETRY", telemetryValue.c_str(), 1);
    else
        unsetenv("MIYOOFIN_TELEMETRY");
    if (hadMinFree)
        setenv("MIYOOFIN_TELEMETRY_MIN_FREE_MIB", minFreeValue.c_str(), 1);
    else
        unsetenv("MIYOOFIN_TELEMETRY_MIN_FREE_MIB");
    std::printf("[test] telemetry config and runtime facade OK\n");
}

static void testTelemetryContextAndGuards()
{
    std::printf("[test] telemetry context and hot-path guards\n");
    PerformanceTelemetry &telemetry = performanceTelemetry();
    telemetry.stop();
    CHECK(currentRequestKind() == RequestKind::Unknown);
    CHECK(currentRouteKind() == RouteKind::Unknown);
    CHECK(currentRouteAttempt() == 0);
    CHECK(!currentRouteFallback());
    CHECK(currentArtworkContext() == ArtworkContext::Unknown);
    {
        TelemetryRequestScope request(RequestKind::Artwork);
        TelemetryRouteScope route(RouteKind::Public, 2, true);
        TelemetryArtworkScope artwork(ArtworkContext::EpisodePrefetch);
        CHECK(currentRequestKind() == RequestKind::Unknown);
        CHECK(currentRouteKind() == RouteKind::Unknown);
        CHECK(currentRouteAttempt() == 0);
        CHECK(!currentRouteFallback());
        CHECK(currentArtworkContext() == ArtworkContext::Unknown);
    }

#if MIYOOFIN_ENABLE_PERF_TELEMETRY
    TelemetryConfig config;
    config.runtimeEnabled = true;
    telemetry.start(config);
    CHECK(currentRequestKind() == RequestKind::Unknown);
    CHECK(currentRouteKind() == RouteKind::Unknown);
    CHECK(currentArtworkContext() == ArtworkContext::Unknown);
    {
        TelemetryRequestScope outerRequest(RequestKind::Artwork);
        TelemetryRouteScope outerRoute(RouteKind::Lan, 1, false);
        TelemetryArtworkScope outerArtwork(ArtworkContext::MovieDetails);
        CHECK(currentRequestKind() == RequestKind::Artwork);
        CHECK(currentRouteKind() == RouteKind::Lan);
        CHECK(currentRouteAttempt() == 1);
        CHECK(!currentRouteFallback());
        CHECK(currentArtworkContext() == ArtworkContext::MovieDetails);
        {
            TelemetryRequestScope innerRequest(RequestKind::PlaybackPosition);
            TelemetryRouteScope innerRoute(RouteKind::Public, 2, true);
            TelemetryArtworkScope innerArtwork(ArtworkContext::EpisodeSelected);
            CHECK(currentRequestKind() == RequestKind::PlaybackPosition);
            CHECK(currentRouteKind() == RouteKind::Public);
            CHECK(currentRouteAttempt() == 2);
            CHECK(currentRouteFallback());
            CHECK(currentArtworkContext() == ArtworkContext::EpisodeSelected);
        }
        CHECK(currentRequestKind() == RequestKind::Artwork);
        CHECK(currentRouteKind() == RouteKind::Lan);
        CHECK(currentRouteAttempt() == 1);
        CHECK(!currentRouteFallback());
        CHECK(currentArtworkContext() == ArtworkContext::MovieDetails);
    }
    CHECK(currentRequestKind() == RequestKind::Unknown);
    CHECK(currentRouteKind() == RouteKind::Unknown);
    CHECK(currentRouteAttempt() == 0);
    CHECK(!currentRouteFallback());
    CHECK(currentArtworkContext() == ArtworkContext::Unknown);
    {
        TelemetryRequestScope outerRequest(RequestKind::SystemInfo);
        CHECK(currentRequestKind() == RequestKind::SystemInfo);
        {
            TelemetryRequestScope innerRequest(RequestKind::Authentication);
            CHECK(currentRequestKind() == RequestKind::Authentication);
        }
        CHECK(currentRequestKind() == RequestKind::SystemInfo);
    }
    CHECK(currentRequestKind() == RequestKind::Unknown);
    Session routeSession;
    routeSession.serverUrl = "https://public.example";
    std::string routeError;
    {
        int routeCalls = 0;
        CHECK(RouteRequest(routeSession).run([&](const std::string &base) {
            ++routeCalls;
            CHECK(base == "https://public.example");
            CHECK(currentRouteKind() == RouteKind::Public);
            CHECK(currentRouteAttempt() == 1);
            CHECK(!currentRouteFallback());
            return true;
        }, routeError));
        CHECK(routeCalls == 1);
        CHECK(currentRouteKind() == RouteKind::Unknown);
        CHECK(currentRouteAttempt() == 0);
        CHECK(!currentRouteFallback());
    }
    routeSession.localServerUrl = "http://192.168.1.10";
    routeSession.publicServerUrl = "https://public.example";
    {
        int routeCalls = 0;
        CHECK(RouteRequest(routeSession).run([&](const std::string &base) {
            ++routeCalls;
            const bool lan = base == routeSession.localServerUrl;
            CHECK(currentRouteKind() == (lan ? RouteKind::Lan : RouteKind::Public));
            CHECK(currentRouteAttempt() == (lan ? 1 : 2));
            CHECK(currentRouteFallback() == !lan);
            if (lan) {
                routeError = "Transport: connect failed";
                return false;
            }
            CHECK(base == routeSession.publicServerUrl);
            return true;
        }, routeError));
        CHECK(routeCalls == 2);
        CHECK(currentRouteKind() == RouteKind::Unknown);
        CHECK(currentRouteAttempt() == 0);
        CHECK(!currentRouteFallback());
    }
#endif

#if MIYOOFIN_ENABLE_PERF_TELEMETRY
    TelemetryTimer activeTimer;
    CHECK(activeTimer.active());
    telemetry.stop();
    TelemetryTimer inactiveTimer;
    CHECK(!inactiveTimer.active());
    CHECK(inactiveTimer.elapsedUs() == 0);
#else
    telemetry.stop();
    TelemetryTimer inactiveTimer;
    CHECK(!inactiveTimer.active());
    CHECK(inactiveTimer.elapsedUs() == 0);
#endif
    std::printf("[test] telemetry context and hot-path guards OK\n");
}

static TelemetryRecord makeTelemetryRingRecord(uint32_t sequence)
{
    TelemetryRecord record{};
    record.header.record_type = RecordType::SessionEvent;
    record.header.record_size = sizeof(SessionEvent);
    record.header.sequence = sequence;
    record.header.monotonic_us = sequence;
    record.payload.session_event.kind = static_cast<uint8_t>(SessionEventKind::TelemetryStarted);
    return record;
}

static void testTelemetryRing()
{
    std::printf("[test] bounded telemetry ring FIFO, recovery, wrap, and MPSC stress\n");
    TelemetryRing<4> ring;
    TelemetryRecord record;
    CHECK(ring.approximateDepth() == 0);
    for (uint32_t sequence = 1; sequence <= 4; ++sequence)
        CHECK(ring.tryEnqueue(makeTelemetryRingRecord(sequence)));
    CHECK(ring.approximateDepth() == 4);
    CHECK(!ring.tryEnqueue(makeTelemetryRingRecord(5)));
    CHECK(ring.approximateHighWatermark() >= 4);
    for (uint32_t sequence = 1; sequence <= 4; ++sequence) {
        CHECK(ring.tryDequeue(record));
        CHECK(record.header.sequence == sequence);
    }
    CHECK(!ring.tryDequeue(record));
    CHECK(ring.approximateDepth() == 0);
    CHECK(ring.tryEnqueue(makeTelemetryRingRecord(6)));
    CHECK(ring.tryDequeue(record));
    CHECK(record.header.sequence == 6);

#if defined(MIYOOFIN_TELEMETRY_RING_TEST)
    TelemetryRing<4> wrap;
    wrap.seedPositionsForTest(UINT32_MAX - 1u);
    for (uint32_t sequence = 1; sequence <= 4; ++sequence)
        CHECK(wrap.tryEnqueue(makeTelemetryRingRecord(sequence)));
    for (uint32_t sequence = 1; sequence <= 4; ++sequence) {
        CHECK(wrap.tryDequeue(record));
        CHECK(record.header.sequence == sequence);
    }
#endif

    constexpr int producerCount = 4;
    constexpr int recordsPerProducer = 128;
    constexpr int totalRecords = producerCount * recordsPerProducer;
    TelemetryRing<512> stressRing;
    std::atomic<int> completedProducers{0};
    std::atomic<int> consumedRecords{0};
    std::vector<bool> seen(totalRecords, false);
    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (int producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&stressRing, &completedProducers, producer]() {
            for (int index = 0; index < recordsPerProducer; ++index) {
                const uint32_t sequence = static_cast<uint32_t>(producer * recordsPerProducer + index);
                const TelemetryRecord record = makeTelemetryRingRecord(sequence);
                while (!stressRing.tryEnqueue(record))
                    std::this_thread::yield();
            }
            completedProducers.fetch_add(1, std::memory_order_release);
        });
    }
    while (completedProducers.load(std::memory_order_acquire) != producerCount
        || stressRing.approximateDepth() != 0) {
        if (stressRing.tryDequeue(record)) {
            CHECK(record.header.sequence < static_cast<uint32_t>(totalRecords));
            if (record.header.sequence < static_cast<uint32_t>(totalRecords)) {
                CHECK(!seen[record.header.sequence]);
                seen[record.header.sequence] = true;
            }
            consumedRecords.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::this_thread::yield();
        }
    }
    for (std::thread &producer : producers)
        producer.join();
    CHECK(consumedRecords.load(std::memory_order_relaxed) == totalRecords);
    for (bool wasSeen : seen)
        CHECK(wasSeen);
    CHECK(stressRing.approximateDepth() == 0);
    std::printf("[test] bounded telemetry ring FIFO, recovery, wrap, and MPSC stress OK\n");
}


