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

static void testMftV2CatalogDbTelemetry()
{
    std::printf("[test] MFT v2 CatalogDb format and aggregation\n");
    MftFileHeader header{};
    header.schema_version = 2;
    std::array<uint8_t, kMftFileHeaderSize> headerBytes{};
    CHECK(encodeFileHeader(header, headerBytes.data(), headerBytes.size()));
    CHECK(headerBytes[4] == 2 && headerBytes[5] == 0);

    CatalogDbSummaryRecord summary{};
    summary.sequence = 7;
    summary.monotonic_us = 0x0102030405060708ull;
    summary.payload.query_count = 2;
    summary.payload.query_total_us = 300;
    summary.payload.query_max_us = 200;
    summary.payload.transaction_count = 1;
    summary.payload.transaction_total_us = 400;
    summary.payload.transaction_max_us = 400;
    summary.payload.commit_count = 1;
    summary.payload.commit_total_us = 25;
    summary.payload.commit_max_us = 25;
    summary.payload.queue_wait_total_us = 50;
    summary.payload.queue_wait_max_us = 40;
    summary.payload.enqueue_rejected_delta = 1;
    summary.payload.rows_inserted = 3;
    summary.payload.rows_updated = 4;
    summary.payload.rows_deleted = 5;
    summary.payload.sqlite_busy_family_delta = 6;
    summary.payload.sqlite_ioerr_family_delta = 7;
    summary.payload.sqlite_corrupt_notadb_delta = 8;
    std::array<uint8_t, kMftMaxRecordSize> summaryBytes{};
    uint16_t written = 0;
    CHECK(encodeCatalogDbSummaryRecord(summary, summaryBytes.data(),
                                       summaryBytes.size(), written));
    CHECK(written == kMftV2CatalogDbSummarySize);
    CHECK(summaryBytes[0] == 15 && summaryBytes[1] == 0);
    CHECK(summaryBytes[2] == 128 && summaryBytes[3] == 0);
    CHECK(summaryBytes[16] == 2 && summaryBytes[20] == 44);
    CHECK(std::all_of(summaryBytes.begin() + 104, summaryBytes.end(),
                      [](uint8_t value) { return value == 0; }));

    TelemetryRecord worker{};
    worker.header.record_type = RecordType::WorkerSample;
    worker.payload.worker_sample.worker_id = kMftV2CatalogDbWorkerId;
    CHECK(encodeRecord(worker, summaryBytes.data(), summaryBytes.size(), written));
    CHECK(written == 44);
    CHECK(summaryBytes[16] == kMftV2CatalogDbWorkerId);

    PerformanceTelemetry::setSchemaVersionForTest(2);
    PerformanceTelemetry &telemetry = performanceTelemetry();
    telemetry.stop();
    const std::string root = "/tmp/miyoofin-v2-catalog-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string path = root + "/telemetry.mft";
    std::remove(path.c_str());
    ::rmdir(root.c_str());
    TelemetryConfig config;
    config.runtimeEnabled = true;
    config.targetDirectory = root;
    config.sampleIntervalMs = 1;
    config.flushIntervalMs = 1;
    config.minFreeStorageBytes = 0;
    telemetry.start(config);
    for (int attempt = 0; attempt < 100 && !telemetry.serviceThreadActive(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(telemetry.serviceThreadActive());
    telemetry.setCatalogDbActive(true);
    telemetry.setCatalogDbQueueDepth(3);
    telemetry.addCatalogDbCompleted(2);
    telemetry.addCatalogDbFailed();
    telemetry.addCatalogDbCancelled();
    telemetry.recordCatalogDbQuery(100);
    telemetry.recordCatalogDbQuery(200);
    telemetry.recordCatalogDbTransaction(400);
    telemetry.recordCatalogDbCommit(25);
    telemetry.recordCatalogDbQueueWait(40);
    telemetry.addCatalogDbEnqueueRejected();
    telemetry.addCatalogDbRows(3, 4, 5);
    telemetry.recordCatalogDbSqliteError(5);
    telemetry.recordCatalogDbSqliteError(10);
    telemetry.recordCatalogDbSqliteError(11);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    telemetry.stop();
    std::ifstream input(path, std::ios::binary);
    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(bytes.size() >= kMftFileHeaderSize);
    if (bytes.size() >= kMftFileHeaderSize) {
        CHECK(bytes[4] == 2 && bytes[5] == 0);
        bool sawWorker = false;
        bool sawSummary = false;
        for (std::size_t offset = kMftFileHeaderSize;
             offset + kMftRecordHeaderSize <= bytes.size();) {
            const uint16_t type = static_cast<uint16_t>(bytes[offset])
                | (static_cast<uint16_t>(bytes[offset + 1]) << 8);
            const uint16_t size = static_cast<uint16_t>(bytes[offset + 2])
                | (static_cast<uint16_t>(bytes[offset + 3]) << 8);
            if (size < kMftRecordHeaderSize || offset + size > bytes.size())
                break;
            if (type == static_cast<uint16_t>(RecordType::WorkerSample)
                && bytes[offset + 16] == kMftV2CatalogDbWorkerId) {
                sawWorker = true;
                CHECK(bytes[offset + 18] == 1);
                CHECK(bytes[offset + 20] == 3);
            }
            if (type == static_cast<uint16_t>(MftV2RecordType::CatalogDbSummary)
                && size == kMftV2CatalogDbSummarySize) {
                const std::size_t payload = offset + 16;
                if (bytes[payload] == 2 && bytes[payload + 4] == 44) {
                    sawSummary = true;
                    CHECK(bytes[payload + 32] == 1);
                    CHECK(bytes[payload + 36] == 25);
                    CHECK(bytes[payload + 48] == 40);
                    CHECK(bytes[payload + 60] == 1);
                    CHECK(bytes[payload + 64] == 3);
                    CHECK(bytes[payload + 68] == 4);
                    CHECK(bytes[payload + 72] == 5);
                    CHECK(bytes[payload + 76] == 1);
                    CHECK(bytes[payload + 80] == 1);
                    CHECK(bytes[payload + 84] == 1);
                    CHECK(bytes[payload + 88] == 0);
                    CHECK(bytes[payload + 92] == 0);
                }
            }
            offset += size;
        }
        CHECK(sawWorker);
        CHECK(sawSummary);
    }
    std::remove(path.c_str());
    ::rmdir(root.c_str());
    PerformanceTelemetry::setSchemaVersionForTest(1);
    std::printf("[test] MFT v2 CatalogDb format and aggregation OK\n");
}

static DownloadSegmentAttempt makeTelemetrySegmentAttemptPayload(
    uint32_t jobSequence, uint32_t segmentOrdinal, uint16_t attemptNumber,
    uint16_t retryDelayMs, RouteKind route, Outcome outcome,
    bool retryPlanned, uint64_t durationUs, uint64_t payloadBytes,
    uint32_t httpStatus, uint32_t curlCode)
{
    DownloadSegmentAttempt payload{};
    payload.telemetry_job_seq = jobSequence;
    payload.segment_ordinal = segmentOrdinal;
    payload.attempt_number = attemptNumber;
    payload.retry_delay_ms = retryDelayMs;
    payload.route_kind = static_cast<uint8_t>(route);
    payload.outcome = static_cast<uint8_t>(outcome);
    payload.retry_planned = retryPlanned ? 1u : 0u;
    payload.duration_us = durationUs;
    payload.payload_bytes = payloadBytes;
    payload.http_status = httpStatus;
    payload.curl_code = curlCode;
    return payload;
}

static void testTelemetrySegmentAttemptPayload()
{
    std::printf("[test] HLS segment attempt payload builder\n");
    const DownloadSegmentAttempt payload = makeTelemetrySegmentAttemptPayload(
        17, 3, 4, 8000, RouteKind::Lan, Outcome::Failure, true,
        1234, 5678, 503, 28);
    CHECK(payload.telemetry_job_seq == 17);
    CHECK(payload.segment_ordinal == 3);
    CHECK(payload.attempt_number == 4);
    CHECK(payload.retry_delay_ms == 8000);
    CHECK(payload.route_kind == static_cast<uint8_t>(RouteKind::Lan));
    CHECK(payload.outcome == static_cast<uint8_t>(Outcome::Failure));
    CHECK(payload.retry_planned == 1);
    CHECK(payload.duration_us == 1234);
    CHECK(payload.payload_bytes == 5678);
    CHECK(payload.http_status == 503);
    CHECK(payload.curl_code == 28);
    CHECK(payload.reserved0 == 0);
    CHECK(payload.reserved1 == 0);
    std::printf("[test] HLS segment attempt payload builder OK\n");
}

static PlaybackEvent makeTelemetryPlaybackEventPayload(
    PlaybackStage stage, PlaybackSourceKind source, uint8_t childExitKind,
    int32_t childExitCode, uint64_t durationUs, uint32_t playbackSequence)
{
    PlaybackEvent payload{};
    payload.stage = static_cast<uint8_t>(stage);
    payload.source = static_cast<uint8_t>(source);
    payload.child_exit_kind = childExitKind;
    payload.duration_us = durationUs;
    payload.child_exit_code = childExitCode;
    payload.playback_seq = playbackSequence;
    return payload;
}

static void testTelemetryPlaybackEventPayload()
{
    std::printf("[test] playback milestone payload bookkeeping\n");
    const PlaybackStage stages[] = {
        PlaybackStage::RequestToFinalPresent,
        PlaybackStage::SuspendPlatform,
        PlaybackStage::ChildWait,
        PlaybackStage::ResumePlatform,
        PlaybackStage::ReturnToFirstNormalFrame,
    };
    for (std::size_t index = 0; index < sizeof(stages) / sizeof(stages[0]); ++index) {
        const PlaybackEvent payload = makeTelemetryPlaybackEventPayload(
            stages[index], PlaybackSourceKind::Unknown,
            index == 2 ? 1 : 0, index == 2 ? 17 : 0,
            100 + index, 23);
        CHECK(payload.stage == static_cast<uint8_t>(stages[index]));
        CHECK(payload.source == static_cast<uint8_t>(PlaybackSourceKind::Unknown));
        CHECK(payload.child_exit_kind == (index == 2 ? 1 : 0));
        CHECK(payload.duration_us == 100 + index);
        CHECK(payload.child_exit_code == (index == 2 ? 17 : 0));
        CHECK(payload.playback_seq == 23);
        CHECK(payload.reserved0 == 0);
        CHECK(payload.reserved1 == 0);
    }
    std::printf("[test] playback milestone payload bookkeeping OK\n");
}

#if MIYOOFIN_ENABLE_PERF_TELEMETRY
static void testTelemetrySecretLeakRegression()
{
    std::printf("[test] telemetry raw MFT secret redaction regression\n");
    const std::string accessToken = "SECRET_ACCESS_TOKEN_123";
    const std::string authenticatedUrl =
        "https://private.example/Items/SECRET_ITEM?api_key=SECRET_ACCESS_TOKEN_123";
    const std::string username = "SECRET_USERNAME_456";
    const std::string password = "SECRET_PASSWORD_789";
    const std::string serverId = "SECRET_SERVER_ID_ABC";
    const std::string userId = "SECRET_USER_ID_DEF";
    const std::string deviceId = "SECRET_DEVICE_ID_GHI";
    const std::string itemId = "SECRET_ITEM_ID_JKL";
    const std::string mediaSourceId = "SECRET_MEDIA_SOURCE_MNO";
    const std::string seriesId = "SECRET_SERIES_ID_PQR";
    const std::string seasonId = "SECRET_SEASON_ID_STU";
    const std::string episodeId = "SECRET_EPISODE_ID_VWX";
    const std::string title = "SECRET_TITLE_YZA";
    const std::string imageTag = "SECRET_IMAGE_TAG_BCD";
    const std::string cachePath = "SECRET_CACHE_PATH_EFG";
    const std::string downloadScope = "SECRET_DOWNLOAD_SCOPE_HIJ";
    const std::vector<std::string> fakeSecrets = {
        accessToken, authenticatedUrl, username, password, serverId, userId,
        deviceId, itemId, mediaSourceId, seriesId, seasonId, episodeId,
        title, imageTag, cachePath, downloadScope,
    };

    const std::string root = "/tmp/miyoofin-secret-regression-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string path = root + "/telemetry.mft";
    std::remove(path.c_str());
    ::rmdir(root.c_str());

    TelemetryConfig config;
    config.runtimeEnabled = true;
    config.targetDirectory = root;
    config.sampleIntervalMs = 1;
    config.freeSpaceIntervalMs = 600000;
    config.flushIntervalMs = 1;
    config.minFreeStorageBytes = 0;

    PerformanceTelemetry &telemetry = performanceTelemetry();
    telemetry.stop();
    telemetry.start(config);
    for (int attempt = 0; attempt < 100 && !telemetry.serviceThreadActive(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(telemetry.serviceThreadActive());
    CHECK(telemetry.enabledFast());

    telemetry.setScreen(ScreenId::Home);
    telemetry.setTab(TabId::Downloads);
    telemetry.setAction(ActionId::Confirm);
    telemetry.setPlaybackState(PlaybackState::UiActive);
    telemetry.setWorkerActive(WorkerId::DownloadTransfer, true);
    telemetry.setWorkerQueueDepth(WorkerId::DownloadTransfer, 2);
    telemetry.addWorkerCompleted(WorkerId::DownloadTransfer);
    telemetry.addWorkerFailed(WorkerId::DownloadTransfer);
    telemetry.addWorkerCancelled(WorkerId::DownloadTransfer);
    telemetry.recordArtworkCacheProbe(true);
    telemetry.recordArtworkCacheRead(true, 128);
    telemetry.recordArtworkCacheWrite(true, 256);
    telemetry.recordArtworkDecode(true, 512);
    telemetry.setDownloadGauges(1, 2, 3);
    telemetry.addDownloadBytes(4096);
    telemetry.addDownloadSegmentCompleted();
    telemetry.addDownloadSegmentRetries();

    TelemetryRecord network{};
    network.header.record_type = RecordType::NetworkRequest;
    network.payload.network_request.request_kind =
        static_cast<uint16_t>(RequestKind::PlaybackPosition);
    network.payload.network_request.route_kind = static_cast<uint8_t>(RouteKind::Public);
    network.payload.network_request.method = static_cast<uint8_t>(HttpMethod::Post);
    network.payload.network_request.duration_us = 1000;
    network.payload.network_request.http_status = 200;
    network.payload.network_request.rx_payload_bytes = 64;
    network.payload.network_request.tx_body_bytes = 32;
    network.payload.network_request.attempt = 1;
    CHECK(telemetry.emitRecord(network));

    TelemetryRecord playback{};
    playback.header.record_type = RecordType::PlaybackEvent;
    playback.payload.playback_event.stage =
        static_cast<uint8_t>(PlaybackStage::ChildWait);
    playback.payload.playback_event.source =
        static_cast<uint8_t>(PlaybackSourceKind::Jellyfin);
    playback.payload.playback_event.duration_us = 2000;
    playback.payload.playback_event.playback_seq = 7;
    CHECK(telemetry.emitRecord(playback));

    CHECK(telemetry.emitSessionEvent(SessionEventKind::SamplingSuspended,
                                     Outcome::Success,
                                     static_cast<uint32_t>(SamplingReason::ExternalPlayback),
                                     0));
    telemetry.suspendSampling(true, SamplingReason::ExternalPlayback);
    telemetry.suspendSampling(false, SamplingReason::ExternalPlayback);
    CHECK(telemetry.emitSessionEvent(SessionEventKind::SamplingResumed,
                                     Outcome::Success,
                                     static_cast<uint32_t>(SamplingReason::ExternalPlayback),
                                     0));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    telemetry.stop();

    std::ifstream stream(path, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
    CHECK(!raw.empty());
    for (const std::string &secret : fakeSecrets)
        CHECK(raw.find(secret) == std::string::npos);
    CHECK(raw.find("SECRET_") == std::string::npos);
    CHECK(raw.find("private.example") == std::string::npos);
    CHECK(raw.find("api_key=") == std::string::npos);

    std::remove(path.c_str());
    ::rmdir(root.c_str());
    std::printf("[test] telemetry raw MFT secret redaction regression OK\n");
}
#endif

#endif
