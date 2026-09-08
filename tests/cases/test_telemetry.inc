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

#if MIYOOFIN_ENABLE_PERF_TELEMETRY
static void testMftV1Codec()
{
    std::printf("[test] MFT v1 little-endian codec and golden bytes\n");
    MftFileHeader fileHeader{};
    fileHeader.flags = 0xaabbccddu;
    fileHeader.pid = 0x01020304u;
    fileHeader.session_nonce = 0x1122334455667788ull;
    fileHeader.start_monotonic_us = 0x0102030405060708ull;
    fileHeader.optional_wall_time_s = 0x1112131415161718ull;
    fileHeader.sample_interval_ms = 1000;
    fileHeader.free_space_interval_ms = 10000;
    fileHeader.rotation_index = 2;
    fileHeader.writer_buffer_bytes = 32768;
    fileHeader.app_version_packed = 0x00102030u;
    fileHeader.build_commit32 = 0xa1b2c3d4u;
    fileHeader.platform_id = static_cast<uint32_t>(PlatformId::Host);

    std::array<uint8_t, kMftFileHeaderSize> headerBytes{};
    CHECK(encodeFileHeader(fileHeader, headerBytes.data(), headerBytes.size()));
    const std::array<uint8_t, kMftFileHeaderSize> expectedHeader = {
        'M', 'F', 'T', '1', 0x01, 0x00, 0x50, 0x00,
        0xdd, 0xcc, 0xbb, 0xaa, 0x04, 0x03, 0x02, 0x01,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        0xe8, 0x03, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
        0x30, 0x20, 0x10, 0x00, 0xd4, 0xc3, 0xb2, 0xa1,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    CHECK(headerBytes == expectedHeader);
    CHECK(!encodeFileHeader(fileHeader, headerBytes.data(), kMftFileHeaderSize - 1));

    TelemetryRecord session{};
    session.header.record_type = RecordType::SessionEvent;
    session.header.sequence = 0x11223344u;
    session.header.monotonic_us = 0x0102030405060708ull;
    session.payload.session_event.kind = static_cast<uint8_t>(SessionEventKind::WriterError);
    session.payload.session_event.outcome = static_cast<uint8_t>(Outcome::Failure);
    session.payload.session_event.value0 = 0xaabbccddu;
    session.payload.session_event.value1 = 0x1122334455667788ull;

    std::array<uint8_t, kMftMaxRecordSize> recordBytes{};
    uint16_t written = 0;
    CHECK(encodeRecord(session, recordBytes.data(), recordBytes.size(), written));
    CHECK(written == 32);
    const std::array<uint8_t, 32> expectedSession = {
        0x0e, 0x00, 0x20, 0x00, 0x44, 0x33, 0x22, 0x11,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        0x06, 0x02, 0x00, 0x00, 0xdd, 0xcc, 0xbb, 0xaa,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    };
    CHECK(std::equal(expectedSession.begin(), expectedSession.end(), recordBytes.begin()));

    TelemetryRecord stall{};
    stall.header.record_type = RecordType::UiStall;
    stall.header.sequence = 0x01020304u;
    stall.header.monotonic_us = 0x1112131415161718ull;
    stall.payload.ui_stall.edge = static_cast<uint8_t>(StallEdge::SlowScope);
    stall.payload.ui_stall.screen = static_cast<uint16_t>(ScreenId::MovieDetails);
    stall.payload.ui_stall.tab = static_cast<uint16_t>(TabId::Downloads);
    stall.payload.ui_stall.action = static_cast<uint16_t>(ActionId::Confirm);
    stall.payload.ui_stall.phase = static_cast<uint8_t>(UiPhaseId::Render);
    stall.payload.ui_stall.duration_us = 0x2122232425262728ull;
    stall.payload.ui_stall.scope_id = static_cast<uint16_t>(UiScopeId::ScreenRender);
    stall.payload.ui_stall.worker_mask = 0x1234;
    written = 0;
    CHECK(encodeRecord(stall, recordBytes.data(), recordBytes.size(), written));
    CHECK(written == 48);
    CHECK(recordBytes[0] == 0x0c && recordBytes[1] == 0x00);
    CHECK(recordBytes[2] == 0x30 && recordBytes[3] == 0x00);
    CHECK(recordBytes[16] == 0x03 && recordBytes[17] == 0x00);
    CHECK(recordBytes[18] == 0x09 && recordBytes[19] == 0x00);
    CHECK(recordBytes[20] == 0x04 && recordBytes[21] == 0x00);
    CHECK(recordBytes[22] == 0x05 && recordBytes[23] == 0x00);
    CHECK(recordBytes[24] == 0x05);
    CHECK(recordBytes[28] == 0x28 && recordBytes[29] == 0x27);
    CHECK(recordBytes[36] == 0x04 && recordBytes[37] == 0x00);
    CHECK(recordBytes[38] == 0x34 && recordBytes[39] == 0x12);

    const RecordType recordTypes[] = {
        RecordType::SystemSample, RecordType::FrameTimingSummary,
        RecordType::StateTransition, RecordType::WorkerSample,
        RecordType::NetworkRequest, RecordType::ArtworkSummary,
        RecordType::ArtworkDecode, RecordType::LibrarySync,
        RecordType::DownloadSample, RecordType::DownloadSegmentAttempt,
        RecordType::PlaybackEvent, RecordType::UiStall,
        RecordType::TelemetryHealth, RecordType::SessionEvent,
    };
    const uint16_t expectedSizes[] = {88, 88, 28, 44, 56, 80, 48, 48,
                                      56, 64, 40, 48, 48, 32};
    TelemetryRecord zeroRecord{};
    zeroRecord.header.sequence = 1;
    for (size_t index = 0; index < sizeof(recordTypes) / sizeof(recordTypes[0]); ++index) {
        zeroRecord.header.record_type = recordTypes[index];
        written = 0;
        CHECK(encodedRecordSize(recordTypes[index]) == expectedSizes[index]);
        CHECK(encodeRecord(zeroRecord, recordBytes.data(), recordBytes.size(), written));
        CHECK(written == expectedSizes[index]);
    }
    CHECK(!encodeRecord(zeroRecord, recordBytes.data(), 31, written));
    CHECK(written == 0);
    zeroRecord.header.record_type = static_cast<RecordType>(0xffff);
    CHECK(encodedRecordSize(zeroRecord.header.record_type) == 0);
    CHECK(!encodeRecord(zeroRecord, recordBytes.data(), recordBytes.size(), written));
    CHECK(written == 0);
    std::printf("[test] MFT v1 little-endian codec and golden bytes OK\n");
}

static void testTelemetryWriter()
{
    std::printf("[test] buffered telemetry writer header, flush, close, and errors\n");
    const std::string root = "/tmp/miyoofin-writer-" + std::to_string(static_cast<long long>(getpid()));
    const std::string path = root + "/telemetry.mft";
    std::remove(path.c_str());
    ::rmdir(root.c_str());

    TelemetryConfig config;
    config.targetDirectory = root;
    config.writerBufferBytes = 256;
    config.flushIntervalMs = 5000;
    MftFileHeader header{};
    header.flags = 2;
    header.pid = static_cast<uint32_t>(getpid());
    header.session_nonce = 0x12345678ull;
    header.sample_interval_ms = config.sampleIntervalMs;
    header.free_space_interval_ms = config.freeSpaceIntervalMs;
    header.writer_buffer_bytes = config.writerBufferBytes;
    header.platform_id = static_cast<uint32_t>(PlatformId::Host);

    TelemetryWriter writer;
    CHECK(writer.open(config, header));
    CHECK(writer.isOpen());
    CHECK(writer.logicalBytesWritten() == kMftFileHeaderSize);
    CHECK(writer.bufferedBytes() == 0);
    FILE *file = std::fopen(path.c_str(), "rb");
    CHECK(file != nullptr);
    if (file != nullptr) {
        std::array<uint8_t, kMftFileHeaderSize> bytes{};
        CHECK(std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size());
        CHECK(bytes[0] == 'M' && bytes[1] == 'F' && bytes[2] == 'T' && bytes[3] == '1');
        CHECK(bytes[4] == 1 && bytes[5] == 0 && bytes[6] == 80 && bytes[7] == 0);
        std::fclose(file);
    }

    TelemetryRecord record{};
    record.header.record_type = RecordType::SessionEvent;
    record.header.sequence = 1;
    record.payload.session_event.kind = static_cast<uint8_t>(SessionEventKind::TelemetryStarted);
    record.payload.session_event.outcome = static_cast<uint8_t>(Outcome::Success);
    record.payload.session_event.value0 = config.sampleIntervalMs;
    CHECK(writer.append(record));
    CHECK(writer.logicalBytesWritten() == kMftFileHeaderSize + 32);
    CHECK(writer.bufferedBytes() == 32);
    file = std::fopen(path.c_str(), "rb");
    CHECK(file != nullptr);
    if (file != nullptr) {
        CHECK(std::fseek(file, 0, SEEK_END) == 0);
        CHECK(std::ftell(file) == static_cast<long>(kMftFileHeaderSize));
        std::fclose(file);
    }
    CHECK(writer.flushIfDue(4999999));
    CHECK(writer.bufferedBytes() == 32);
    CHECK(writer.flushIfDue(5000000));
    CHECK(writer.bufferedBytes() == 0);
    file = std::fopen(path.c_str(), "rb");
    CHECK(file != nullptr);
    if (file != nullptr) {
        CHECK(std::fseek(file, 0, SEEK_END) == 0);
        CHECK(std::ftell(file) == static_cast<long>(kMftFileHeaderSize + 32));
        std::fclose(file);
    }

    record.header.sequence = 2;
    CHECK(writer.append(record));
    CHECK(writer.bufferedBytes() == 32);
    CHECK(writer.close());
    CHECK(!writer.isOpen());
    CHECK(writer.bufferedBytes() == 0);
    file = std::fopen(path.c_str(), "rb");
    CHECK(file != nullptr);
    if (file != nullptr) {
        CHECK(std::fseek(file, 0, SEEK_END) == 0);
        CHECK(std::ftell(file) == static_cast<long>(kMftFileHeaderSize + 64));
        std::fclose(file);
    }

    TelemetryConfig invalid = config;
    invalid.targetDirectory = "/dev/null/miyoofin-writer";
    TelemetryWriter failed;
    CHECK(!failed.open(invalid, header));
    CHECK(!failed.isOpen());
    CHECK(failed.lastErrorKind() == WriterErrorKind::Open);
    CHECK(failed.lastErrorNumber() != 0);
    std::remove(path.c_str());
    ::rmdir(root.c_str());
    std::printf("[test] buffered telemetry writer header, flush, close, and errors OK\n");
}

static void testTelemetryServiceThread()
{
    std::printf("[test] telemetry service runtime-off, drain, drops, rotation, and join\n");
    PerformanceTelemetry &telemetry = performanceTelemetry();
    telemetry.stop();

    const std::string offRoot = "/tmp/miyoofin-telemetry-off-" + std::to_string(static_cast<long long>(getpid()));
    const std::string offPath = offRoot + "/telemetry.mft";
    std::remove(offPath.c_str());
    ::rmdir(offRoot.c_str());
    TelemetryConfig offConfig;
    offConfig.targetDirectory = offRoot;
    offConfig.runtimeEnabled = false;
    telemetry.start(offConfig);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(!telemetry.enabledFast());
    CHECK(!telemetry.serviceThreadActive());
    CHECK(::access(offPath.c_str(), F_OK) != 0);
    telemetry.stop();

    const std::string root = "/tmp/miyoofin-telemetry-service-" + std::to_string(static_cast<long long>(getpid()));
    const std::string activePath = root + "/telemetry.mft";
    std::remove(activePath.c_str());
    ::rmdir(root.c_str());
    TelemetryConfig config;
    config.runtimeEnabled = true;
    config.targetDirectory = root;
    config.writerBufferBytes = 256;
    config.flushIntervalMs = 1;
    config.rotateBytes = 4096;
    config.retainFiles = 4;
    telemetry.start(config);
    for (int attempt = 0; attempt < 100 && !telemetry.serviceThreadActive(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(telemetry.enabledFast());
    CHECK(telemetry.serviceThreadActive());

    for (uint32_t sequence = 0; sequence < 10000; ++sequence) {
        telemetry.emitSessionEvent(SessionEventKind::SamplingResumed,
                                    Outcome::Success, sequence, 0);
    }
    CHECK(telemetry.droppedRecordCount() > 0);
    telemetry.stop();
    CHECK(!telemetry.enabledFast());
    CHECK(!telemetry.serviceThreadActive());
    CHECK(::access(activePath.c_str(), F_OK) == 0);
    CHECK(telemetry.rotationCount() > 0);
    int retainedFiles = 1;
    for (uint32_t index = 0; index < 100; ++index) {
        const std::string rotated = root + "/telemetry-" + std::to_string(index) + ".mft";
        if (::access(rotated.c_str(), F_OK) == 0)
            ++retainedFiles;
    }
    CHECK(retainedFiles <= 4);
    std::remove(activePath.c_str());
    for (uint32_t index = 0; index < 100; ++index) {
        const std::string rotated = root + "/telemetry-" + std::to_string(index) + ".mft";
        std::remove(rotated.c_str());
    }
    ::rmdir(root.c_str());
    std::printf("[test] telemetry service runtime-off, drain, drops, rotation, and join OK\n");
}
#endif

static void testTelemetrySchemaTypes()
{
    std::printf("[test] telemetry schema IDs and record types\n");
    testTelemetryClocks();
    testTelemetryConfigAndFacade();
    testTelemetryContextAndGuards();
    testTelemetryRing();
#if MIYOOFIN_ENABLE_PERF_TELEMETRY
    testMftV1Codec();
    testTelemetryWriter();
    testTelemetryServiceThread();
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

    std::printf("[test] periodic system samples and fake service hooks\n");
    static std::atomic<uint64_t> fakeNowUs{1};
    static std::atomic<uint32_t> fakeMetricSamples{0};
    static std::atomic<uint32_t> fakeFreeSamples{0};
    const auto fakeClock = []() noexcept -> uint64_t {
        return fakeNowUs.load(std::memory_order_relaxed);
    };
    const auto fakeMetrics = [](bool includeFreeStorage) noexcept {
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
    for (int attempt = 0; attempt < 100 && fakeMetricSamples.load(std::memory_order_relaxed) < 2; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sampleTelemetry.setWorkerQueueDepth(WorkerId::HomeLibraryFetch, 1);
    sampleTelemetry.addWorkerCompleted(WorkerId::HomeLibraryFetch, 7);
    sampleTelemetry.recordArtworkCacheProbe(false);
    sampleTelemetry.recordArtworkDecode(false, 20);
    sampleTelemetry.addDownloadBytes(300);
    sampleTelemetry.addDownloadSegmentRetries();
    fakeNowUs.store(200001, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 100 && fakeMetricSamples.load(std::memory_order_relaxed) < 3; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    fakeNowUs.store(300001, std::memory_order_relaxed);
    for (int attempt = 0; attempt < 100 && fakeMetricSamples.load(std::memory_order_relaxed) < 4; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sampleTelemetry.stop();
    CHECK(fakeMetricSamples.load(std::memory_order_relaxed) == 4);
    CHECK(fakeFreeSamples.load(std::memory_order_relaxed) == 3);
    std::ifstream sampleFile(samplePath, std::ios::binary);
    sampleFile.seekg(0, std::ios::end);
    CHECK(sampleFile.good());
    constexpr std::streamoff aggregateTickBytes = 88 + 14 * 44 + 80 + 56 + 48;
    constexpr std::streamoff decodeRecordBytes = 2 * 48;
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
    CHECK(librarySyncRequests == 1);
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
    const auto lowStorageMetrics = [](bool includeFreeStorage) noexcept {
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
    const auto activeLowMetrics = [](bool includeFreeStorage) noexcept {
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
    const auto frameMetrics = [](bool) noexcept {
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
    const auto stateMetrics = [](bool) noexcept {
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

    CHECK(static_cast<uint32_t>(PlatformId::Unknown) == 0);
    CHECK(static_cast<uint32_t>(PlatformId::MiyooMiniPlusOnionOS) == 1);
    CHECK(static_cast<uint32_t>(PlatformId::Host) == 2);
    CHECK(static_cast<uint8_t>(Outcome::Unknown) == 0);
    CHECK(static_cast<uint8_t>(Outcome::Success) == 1);
    CHECK(static_cast<uint8_t>(Outcome::Failure) == 2);
    CHECK(static_cast<uint8_t>(Outcome::Cancelled) == 3);
    CHECK(static_cast<uint8_t>(Outcome::Dropped) == 4);
    CHECK(static_cast<uint8_t>(Outcome::Skipped) == 5);
    CHECK(static_cast<uint8_t>(Outcome::Unavailable) == 6);
    CHECK(static_cast<uint8_t>(RouteKind::Unknown) == 0);
    CHECK(static_cast<uint8_t>(RouteKind::Lan) == 1);
    CHECK(static_cast<uint8_t>(RouteKind::Public) == 2);
    CHECK(static_cast<uint8_t>(HttpMethod::Unknown) == 0);
    CHECK(static_cast<uint8_t>(HttpMethod::Get) == 1);
    CHECK(static_cast<uint8_t>(HttpMethod::Post) == 2);

    CHECK(static_cast<uint16_t>(ScreenId::None) == 0);
    CHECK(static_cast<uint16_t>(ScreenId::Startup) == 1);
    CHECK(static_cast<uint16_t>(ScreenId::ServerEntry) == 2);
    CHECK(static_cast<uint16_t>(ScreenId::Connect) == 3);
    CHECK(static_cast<uint16_t>(ScreenId::Login) == 4);
    CHECK(static_cast<uint16_t>(ScreenId::AuthCheck) == 5);
    CHECK(static_cast<uint16_t>(ScreenId::Home) == 6);
    CHECK(static_cast<uint16_t>(ScreenId::Series) == 7);
    CHECK(static_cast<uint16_t>(ScreenId::EpisodeBrowser) == 8);
    CHECK(static_cast<uint16_t>(ScreenId::MovieDetails) == 9);
    CHECK(static_cast<uint16_t>(ScreenId::InputDiagnostics) == 10);
    CHECK(static_cast<uint16_t>(ScreenId::Other) == 11);
    CHECK(static_cast<uint16_t>(TabId::NotApplicable) == 0);
    CHECK(static_cast<uint16_t>(TabId::Home) == 1);
    CHECK(static_cast<uint16_t>(TabId::Movies) == 2);
    CHECK(static_cast<uint16_t>(TabId::Shows) == 3);
    CHECK(static_cast<uint16_t>(TabId::Downloads) == 4);
    CHECK(static_cast<uint16_t>(TabId::Settings) == 5);
    CHECK(static_cast<uint16_t>(TabId::Other) == 6);
    CHECK(static_cast<uint16_t>(ActionId::None) == 0);
    CHECK(static_cast<uint16_t>(ActionId::Up) == 1);
    CHECK(static_cast<uint16_t>(ActionId::Down) == 2);
    CHECK(static_cast<uint16_t>(ActionId::Left) == 3);
    CHECK(static_cast<uint16_t>(ActionId::Right) == 4);
    CHECK(static_cast<uint16_t>(ActionId::Confirm) == 5);
    CHECK(static_cast<uint16_t>(ActionId::Back) == 6);
    CHECK(static_cast<uint16_t>(ActionId::Search) == 7);
    CHECK(static_cast<uint16_t>(ActionId::ActionsMenu) == 8);
    CHECK(static_cast<uint16_t>(ActionId::PrevTab) == 9);
    CHECK(static_cast<uint16_t>(ActionId::NextTab) == 10);
    CHECK(static_cast<uint16_t>(ActionId::PrevPage) == 11);
    CHECK(static_cast<uint16_t>(ActionId::NextPage) == 12);
    CHECK(static_cast<uint16_t>(ActionId::Settings) == 13);
    CHECK(static_cast<uint16_t>(ActionId::Menu) == 14);
    CHECK(static_cast<uint16_t>(ActionId::Exit) == 15);
    CHECK(static_cast<uint16_t>(ActionId::Raw) == 16);
    CHECK(static_cast<uint16_t>(ActionId::Other) == 17);

    CHECK(static_cast<uint8_t>(FramePhase::FullFrame) == 1);
    CHECK(static_cast<uint8_t>(FramePhase::Input) == 2);
    CHECK(static_cast<uint8_t>(FramePhase::Update) == 3);
    CHECK(static_cast<uint8_t>(FramePhase::Transition) == 4);
    CHECK(static_cast<uint8_t>(FramePhase::ScreenRender) == 5);
    CHECK(static_cast<uint8_t>(FramePhase::FramebufferUpload) == 6);
    CHECK(static_cast<uint8_t>(FramePhase::Present) == 7);
    CHECK(static_cast<uint8_t>(StateKind::Screen) == 1);
    CHECK(static_cast<uint8_t>(StateKind::Tab) == 2);
    CHECK(static_cast<uint8_t>(StateKind::Action) == 3);
    CHECK(static_cast<uint8_t>(StateKind::PlaybackState) == 4);
    CHECK(static_cast<uint16_t>(PlaybackState::Unknown) == 0);
    CHECK(static_cast<uint16_t>(PlaybackState::UiActive) == 1);
    CHECK(static_cast<uint16_t>(PlaybackState::StartingOverlay) == 2);
    CHECK(static_cast<uint16_t>(PlaybackState::ExternalPlayback) == 3);
    CHECK(static_cast<uint16_t>(PlaybackState::Resuming) == 4);

    CHECK(static_cast<uint8_t>(UiPhaseId::Unknown) == 0);
    CHECK(static_cast<uint8_t>(UiPhaseId::IdleFrameBoundary) == 1);
    CHECK(static_cast<uint8_t>(UiPhaseId::EventInput) == 2);
    CHECK(static_cast<uint8_t>(UiPhaseId::Update) == 3);
    CHECK(static_cast<uint8_t>(UiPhaseId::ScreenTransition) == 4);
    CHECK(static_cast<uint8_t>(UiPhaseId::Render) == 5);
    CHECK(static_cast<uint16_t>(UiScopeId::Unknown) == 0);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenHandleAction) == 1);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenUpdate) == 2);
    CHECK(static_cast<uint16_t>(UiScopeId::AppFinishSavedSessionValidation) == 3);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenRender) == 4);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackPushNull) == 5);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackPushMovieDetailsScreen) == 6);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackPushSeriesScreen) == 7);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackPushEpisodeBrowserScreen) == 8);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackPushHomeScreen) == 9);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackPushScreen) == 10);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackPop) == 11);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackSignalLeave) == 12);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackRetireScreen) == 13);
    CHECK(static_cast<uint16_t>(UiScopeId::ScreenStackEnterPrevious) == 14);
    CHECK(static_cast<uint16_t>(UiScopeId::HomeScreenPublishLibraryResult) == 15);
    CHECK(static_cast<uint16_t>(UiScopeId::HomeScreenPublishResumeResult) == 16);
    CHECK(static_cast<uint16_t>(UiScopeId::HomeScreenUpdateArtworkWorkingSet) == 17);
    CHECK(static_cast<uint16_t>(UiScopeId::HomeScreenPublishDecodedArtwork) == 18);
    CHECK(static_cast<uint16_t>(UiScopeId::HomeScreenQueueSelectedArtwork) == 19);
    CHECK(static_cast<uint16_t>(UiScopeId::HomeScreenQueueVisibleArtwork) == 20);
    CHECK(static_cast<uint16_t>(UiScopeId::HomeScreenPublishDownloadSnapshot) == 21);
    CHECK(static_cast<uint16_t>(UiScopeId::EpisodeBrowserScreenEnter) == 22);
    CHECK(static_cast<uint16_t>(UiScopeId::EpisodeBrowserScreenWorkerShutdown) == 23);
    CHECK(static_cast<uint16_t>(UiScopeId::EpisodeBrowserScreenPublishEpisodes) == 24);
    CHECK(static_cast<uint16_t>(UiScopeId::EpisodeBrowserScreenPublishArtworkResult) == 25);
    CHECK(static_cast<uint16_t>(UiScopeId::MovieDetailsScreenEnter) == 26);
    CHECK(static_cast<uint16_t>(UiScopeId::MovieDetailsScreenWorkerCancellation) == 27);
    CHECK(static_cast<uint16_t>(UiScopeId::MovieDetailsScreenOwnedWorkerCreation) == 28);
    CHECK(static_cast<uint16_t>(UiScopeId::MovieDetailsScreenPublishAsyncPreparation) == 29);
    CHECK(static_cast<uint16_t>(UiScopeId::MovieDetailsScreenPublishImageSurfacePreparation) == 30);
    CHECK(static_cast<uint16_t>(UiScopeId::MovieDetailsScreenPublishPlaybackDownloadState) == 31);
    CHECK(static_cast<uint16_t>(UiScopeId::DownloadManagerPlanSnapshotMutexWait) == 32);

    CHECK(static_cast<uint16_t>(WorkerId::HomeLibraryFetch) == 1);
    CHECK(static_cast<uint16_t>(WorkerId::HomeHierarchy) == 2);
    CHECK(static_cast<uint16_t>(WorkerId::HomePoster) == 3);
    CHECK(static_cast<uint16_t>(WorkerId::HomeDecode) == 4);
    CHECK(static_cast<uint16_t>(WorkerId::HomeResumeRefresh) == 5);
    CHECK(static_cast<uint16_t>(WorkerId::HomeDownloadRefresh) == 6);
    CHECK(static_cast<uint16_t>(WorkerId::EpisodeFetch) == 7);
    CHECK(static_cast<uint16_t>(WorkerId::EpisodeArtwork) == 8);
    CHECK(static_cast<uint16_t>(WorkerId::DownloadTransfer) == 9);
    CHECK(static_cast<uint16_t>(WorkerId::DownloadPlanner) == 10);
    CHECK(static_cast<uint16_t>(WorkerId::DownloadReconcile) == 11);
    CHECK(static_cast<uint16_t>(WorkerId::ScreenRetirement) == 12);
    CHECK(static_cast<uint16_t>(WorkerId::SavedSessionValidation) == 13);
    CHECK(static_cast<uint16_t>(WorkerId::PlaybackJournalSync) == 14);
    CHECK(kWorkerMaskHomeLibraryFetch == (uint16_t{1} << 0));
    CHECK(kWorkerMaskHomeHierarchy == (uint16_t{1} << 1));
    CHECK(kWorkerMaskHomePoster == (uint16_t{1} << 2));
    CHECK(kWorkerMaskHomeDecode == (uint16_t{1} << 3));
    CHECK(kWorkerMaskHomeResumeRefresh == (uint16_t{1} << 4));
    CHECK(kWorkerMaskHomeDownloadRefresh == (uint16_t{1} << 5));
    CHECK(kWorkerMaskEpisodeFetch == (uint16_t{1} << 6));
    CHECK(kWorkerMaskEpisodeArtwork == (uint16_t{1} << 7));
    CHECK(kWorkerMaskDownloadTransfer == (uint16_t{1} << 8));
    CHECK(kWorkerMaskDownloadPlanner == (uint16_t{1} << 9));
    CHECK(kWorkerMaskDownloadReconcile == (uint16_t{1} << 10));
    CHECK(kWorkerMaskScreenRetirement == (uint16_t{1} << 11));
    CHECK(kWorkerMaskSavedSessionValidation == (uint16_t{1} << 12));
    CHECK(kWorkerMaskPlaybackJournalSync == (uint16_t{1} << 13));
    CHECK(kWorkerMaskAll == 0x3fffu);

    CHECK(static_cast<uint16_t>(RequestKind::Unknown) == 0);
    CHECK(static_cast<uint16_t>(RequestKind::SystemInfo) == 1);
    CHECK(static_cast<uint16_t>(RequestKind::Authentication) == 2);
    CHECK(static_cast<uint16_t>(RequestKind::TokenValidation) == 3);
    CHECK(static_cast<uint16_t>(RequestKind::Views) == 4);
    CHECK(static_cast<uint16_t>(RequestKind::LibraryItems) == 5);
    CHECK(static_cast<uint16_t>(RequestKind::ResumeItems) == 6);
    CHECK(static_cast<uint16_t>(RequestKind::LatestItems) == 7);
    CHECK(static_cast<uint16_t>(RequestKind::ChangedHierarchy) == 8);
    CHECK(static_cast<uint16_t>(RequestKind::Seasons) == 9);
    CHECK(static_cast<uint16_t>(RequestKind::Episodes) == 10);
    CHECK(static_cast<uint16_t>(RequestKind::PlaybackPosition) == 11);
    CHECK(static_cast<uint16_t>(RequestKind::PlaybackStopped) == 12);
    CHECK(static_cast<uint16_t>(RequestKind::DownloadPlaybackInfo) == 13);
    CHECK(static_cast<uint16_t>(RequestKind::HlsMaster) == 14);
    CHECK(static_cast<uint16_t>(RequestKind::HlsVariant) == 15);
    CHECK(static_cast<uint16_t>(RequestKind::Artwork) == 16);
    CHECK(static_cast<uint8_t>(ArtworkContext::Unknown) == 0);
    CHECK(static_cast<uint8_t>(ArtworkContext::HomeSelected) == 1);
    CHECK(static_cast<uint8_t>(ArtworkContext::HomeGrid) == 2);
    CHECK(static_cast<uint8_t>(ArtworkContext::HomeShows) == 3);
    CHECK(static_cast<uint8_t>(ArtworkContext::HomePoster) == 4);
    CHECK(static_cast<uint8_t>(ArtworkContext::EpisodeSelected) == 5);
    CHECK(static_cast<uint8_t>(ArtworkContext::EpisodePrefetch) == 6);
    CHECK(static_cast<uint8_t>(ArtworkContext::MovieDetails) == 7);
    CHECK(static_cast<uint8_t>(ArtworkContext::CacheGeneric) == 8);
    CHECK(static_cast<uint8_t>(PlaybackStage::RequestToFinalPresent) == 1);
    CHECK(static_cast<uint8_t>(PlaybackStage::SuspendPlatform) == 2);
    CHECK(static_cast<uint8_t>(PlaybackStage::ChildWait) == 3);
    CHECK(static_cast<uint8_t>(PlaybackStage::ResumePlatform) == 4);
    CHECK(static_cast<uint8_t>(PlaybackStage::ReturnToFirstNormalFrame) == 5);
    CHECK(static_cast<uint8_t>(PlaybackSourceKind::Unknown) == 0);
    CHECK(static_cast<uint8_t>(PlaybackSourceKind::Jellyfin) == 1);
    CHECK(static_cast<uint8_t>(PlaybackSourceKind::Local) == 2);
    CHECK(static_cast<uint8_t>(StallEdge::Begin) == 1);
    CHECK(static_cast<uint8_t>(StallEdge::End) == 2);
    CHECK(static_cast<uint8_t>(StallEdge::SlowScope) == 3);
    CHECK(static_cast<uint8_t>(SessionEventKind::TelemetryStarted) == 1);
    CHECK(static_cast<uint8_t>(SessionEventKind::TelemetryStopped) == 2);
    CHECK(static_cast<uint8_t>(SessionEventKind::SamplingSuspended) == 3);
    CHECK(static_cast<uint8_t>(SessionEventKind::SamplingResumed) == 4);
    CHECK(static_cast<uint8_t>(SessionEventKind::WriterDisabledLowSpace) == 5);
    CHECK(static_cast<uint8_t>(SessionEventKind::WriterError) == 6);
    CHECK(static_cast<uint32_t>(WriterErrorKind::Unknown) == 0);
    CHECK(static_cast<uint32_t>(WriterErrorKind::Open) == 1);
    CHECK(static_cast<uint32_t>(WriterErrorKind::Write) == 2);
    CHECK(static_cast<uint32_t>(WriterErrorKind::Flush) == 3);
    CHECK(static_cast<uint32_t>(WriterErrorKind::Rotate) == 4);
    CHECK(static_cast<uint32_t>(WriterErrorKind::Close) == 5);
    CHECK(static_cast<uint32_t>(SamplingReason::Unknown) == 0);
    CHECK(static_cast<uint32_t>(SamplingReason::ExternalPlayback) == 1);
    CHECK(static_cast<uint16_t>(RecordType::SystemSample) == 1);
    CHECK(static_cast<uint16_t>(RecordType::FrameTimingSummary) == 2);
    CHECK(static_cast<uint16_t>(RecordType::StateTransition) == 3);
    CHECK(static_cast<uint16_t>(RecordType::WorkerSample) == 4);
    CHECK(static_cast<uint16_t>(RecordType::NetworkRequest) == 5);
    CHECK(static_cast<uint16_t>(RecordType::ArtworkSummary) == 6);
    CHECK(static_cast<uint16_t>(RecordType::ArtworkDecode) == 7);
    CHECK(static_cast<uint16_t>(RecordType::LibrarySync) == 8);
    CHECK(static_cast<uint16_t>(RecordType::DownloadSample) == 9);
    CHECK(static_cast<uint16_t>(RecordType::DownloadSegmentAttempt) == 10);
    CHECK(static_cast<uint16_t>(RecordType::PlaybackEvent) == 11);
    CHECK(static_cast<uint16_t>(RecordType::UiStall) == 12);
    CHECK(static_cast<uint16_t>(RecordType::TelemetryHealth) == 13);
    CHECK(static_cast<uint16_t>(RecordType::SessionEvent) == 14);

    CHECK(sizeof(SystemSample) == 72);
    CHECK(sizeof(FrameTimingSummary) == 72);
    CHECK(sizeof(StateTransition) == 12);
    CHECK(sizeof(WorkerSample) == 28);
    CHECK(sizeof(NetworkRequest) == 40);
    CHECK(sizeof(ArtworkSummary) == 64);
    CHECK(sizeof(ArtworkDecode) == 32);
    CHECK(sizeof(LibrarySync) == 32);
    CHECK(sizeof(DownloadSample) == 40);
    CHECK(sizeof(DownloadSegmentAttempt) == 48);
    CHECK(sizeof(PlaybackEvent) == 24);
    CHECK(sizeof(UiStall) == 32);
    CHECK(sizeof(TelemetryHealth) == 32);
    CHECK(sizeof(SessionEvent) == 16);
    CHECK(sizeof(TelemetryRecord) <= 96);
    CHECK(std::is_trivially_copyable<SystemSample>::value);
    CHECK(std::is_trivially_copyable<FrameTimingSummary>::value);
    CHECK(std::is_trivially_copyable<StateTransition>::value);
    CHECK(std::is_trivially_copyable<WorkerSample>::value);
    CHECK(std::is_trivially_copyable<NetworkRequest>::value);
    CHECK(std::is_trivially_copyable<ArtworkSummary>::value);
    CHECK(std::is_trivially_copyable<ArtworkDecode>::value);
    CHECK(std::is_trivially_copyable<LibrarySync>::value);
    CHECK(std::is_trivially_copyable<DownloadSample>::value);
    CHECK(std::is_trivially_copyable<DownloadSegmentAttempt>::value);
    CHECK(std::is_trivially_copyable<PlaybackEvent>::value);
    CHECK(std::is_trivially_copyable<UiStall>::value);
    CHECK(std::is_trivially_copyable<TelemetryHealth>::value);
    CHECK(std::is_trivially_copyable<SessionEvent>::value);
    CHECK(std::is_trivially_copyable<TelemetryRecord>::value);
    std::printf("[test] telemetry schema IDs and record types OK\n");
}
