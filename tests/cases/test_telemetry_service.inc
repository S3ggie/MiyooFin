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

#if MIYOOFIN_ENABLE_PERF_TELEMETRY
static void testHttpClientTelemetry()
{
    std::printf("[test] http client transport telemetry\n");
    PerformanceTelemetry &telemetry = performanceTelemetry();
    telemetry.stop();
    const std::string root = "/tmp/miyoofin-http-telemetry-"
        + std::to_string(static_cast<long long>(getpid()));
    const std::string payloadPath = root + "-payload";
    const std::string payloadUrl = "file://" + payloadPath;
    const std::string missingUrl = "file://" + root + "-missing";
    std::remove(payloadPath.c_str());
    std::remove((root + "/telemetry.mft").c_str());
    ::rmdir(root.c_str());
    {
        std::ofstream payload(payloadPath, std::ios::binary);
        payload << "hello";
    }

    TelemetryConfig config;
    config.runtimeEnabled = true;
    config.targetDirectory = root;
    config.sampleIntervalMs = 600000;
    config.freeSpaceIntervalMs = 600000;
    config.flushIntervalMs = 1;
    telemetry.start(config);
    for (int attempt = 0; attempt < 100 && !telemetry.serviceThreadActive(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(telemetry.enabledFast());
    CHECK(telemetry.serviceThreadActive());

    HttpClient client;
    HttpResponse textResponse;
    std::string error;
    {
        TelemetryRequestScope request(RequestKind::Views);
        TelemetryRouteScope route(RouteKind::Public, 1, false);
        CHECK(client.perform("GET", payloadUrl, {}, {}, textResponse, error));
    }
    CHECK(textResponse.body == "hello");
    {
        TelemetryRequestScope request(RequestKind::LibraryItems);
        TelemetryRouteScope route(RouteKind::Public, 2, true);
        CHECK(!client.perform("GET", missingUrl, {}, {}, textResponse, error));
    }
    BinaryHttpResponse binaryResponse;
    {
        TelemetryRequestScope request(RequestKind::Artwork);
        TelemetryRouteScope route(RouteKind::Lan, 1, false);
        CHECK(client.getBinary(payloadUrl, {}, binaryResponse, error));
    }
    CHECK(binaryResponse.data.size() == 5);
    BinaryHttpResponse missingBinary;
    CHECK(!client.getBinary(missingUrl, {}, missingBinary, error));
    telemetry.stop();

    std::ifstream sample(root + "/telemetry.mft", std::ios::binary);
    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(sample)), std::istreambuf_iterator<char>());
    const auto readU16 = [&bytes](std::size_t offset) {
        return static_cast<uint16_t>(bytes[offset])
            | (static_cast<uint16_t>(bytes[offset + 1]) << 8);
    };
    const auto readU32 = [&bytes](std::size_t offset) {
        return static_cast<uint32_t>(bytes[offset])
            | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    };
    const auto readU64 = [&bytes](std::size_t offset) {
        uint64_t value = 0;
        for (unsigned int byte = 0; byte < 8; ++byte)
            value |= static_cast<uint64_t>(bytes[offset + byte]) << (byte * 8);
        return value;
    };
    std::size_t networkRecords = 0;
    bool sawTextSuccess = false;
    bool sawTextFailure = false;
    bool sawBinarySuccess = false;
    bool sawBinaryFailure = false;
    for (std::size_t offset = 80; offset + 16 <= bytes.size();) {
        const uint16_t size = readU16(offset + 2);
        if (size < 16 || offset + size > bytes.size())
            break;
        if (readU16(offset) == static_cast<uint16_t>(RecordType::NetworkRequest)
            && size >= 56) {
            ++networkRecords;
            const uint16_t requestKind = readU16(offset + 16);
            const uint8_t routeKind = bytes[offset + 18];
            const uint8_t method = bytes[offset + 19];
            const uint32_t curlCode = readU32(offset + 32);
            const uint64_t rxBytes = readU64(offset + 36);
            const uint64_t txBytes = readU64(offset + 44);
            const uint8_t attempt = bytes[offset + 52];
            const uint8_t cancelled = bytes[offset + 53];
            const uint8_t truncated = bytes[offset + 54];
            const uint8_t fallback = bytes[offset + 55];
            CHECK(method == 1);
            CHECK(cancelled == 0);
            CHECK(truncated == 0);
            if (requestKind == static_cast<uint16_t>(RequestKind::Views)) {
                CHECK(routeKind == static_cast<uint8_t>(RouteKind::Public));
                CHECK(attempt == 1);
                CHECK(fallback == 0);
                CHECK(curlCode == 0);
                CHECK(rxBytes == 5);
                CHECK(txBytes == 0);
                sawTextSuccess = true;
            } else if (requestKind == static_cast<uint16_t>(RequestKind::LibraryItems)) {
                CHECK(routeKind == static_cast<uint8_t>(RouteKind::Public));
                CHECK(attempt == 2);
                CHECK(fallback == 1);
                CHECK(curlCode != 0);
                sawTextFailure = true;
            } else if (requestKind == static_cast<uint16_t>(RequestKind::Artwork)) {
                CHECK(routeKind == static_cast<uint8_t>(RouteKind::Lan));
                CHECK(attempt == 1);
                CHECK(fallback == 0);
                CHECK(curlCode == 0);
                CHECK(rxBytes == 5);
                sawBinarySuccess = true;
            } else {
                CHECK(requestKind == static_cast<uint16_t>(RequestKind::Unknown));
                CHECK(routeKind == static_cast<uint8_t>(RouteKind::Unknown));
                CHECK(attempt == 0);
                CHECK(fallback == 0);
                CHECK(curlCode != 0);
                sawBinaryFailure = true;
            }
        }
        offset += size;
    }
    CHECK(networkRecords == 4);
    CHECK(sawTextSuccess);
    CHECK(sawTextFailure);
    CHECK(sawBinarySuccess);
    CHECK(sawBinaryFailure);
    std::remove(payloadPath.c_str());
    std::remove((root + "/telemetry.mft").c_str());
    ::rmdir(root.c_str());
    std::printf("[test] http client transport telemetry OK\n");
}
#endif

