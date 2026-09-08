#pragma once

#include <cstdint>
#include <type_traits>

#include "TelemetryIds.hpp"

namespace miyoofin {

#pragma pack(push, 1)

struct SystemSample {
    uint64_t process_cpu_us_cumulative;
    uint64_t rss_kib;
    uint64_t peak_rss_kib;
    uint64_t process_read_bytes_cumulative;
    uint64_t process_write_bytes_cumulative;
    uint64_t free_storage_bytes;
    uint64_t telemetry_logical_bytes_written;
    uint32_t dropped_records_cumulative;
    uint32_t free_storage_sample_age_ms;
    uint32_t validity_flags;
    uint32_t reserved;
};

struct FrameTimingSummary {
    uint8_t phase;
    uint8_t reserved0[3];
    uint32_t interval_us;
    uint32_t sample_count;
    uint64_t total_us;
    uint32_t max_us;
    uint32_t over_50ms_count;
    uint32_t over_100ms_count;
    uint32_t histogram[9];
    uint32_t reserved1;
};

struct StateTransition {
    uint8_t state_kind;
    uint8_t reserved0;
    uint16_t previous_id;
    uint16_t current_id;
    uint16_t reserved1;
    uint32_t transition_seq;
};

struct WorkerSample {
    uint16_t worker_id;
    uint8_t active;
    uint8_t reserved0;
    uint32_t queue_depth;
    uint32_t queue_highwater;
    uint32_t completed_delta;
    uint32_t failed_delta;
    uint32_t cancelled_delta;
    uint32_t reserved1;
};

struct NetworkRequest {
    uint16_t request_kind;
    uint8_t route_kind;
    uint8_t method;
    uint64_t duration_us;
    uint32_t http_status;
    uint32_t curl_code;
    uint64_t rx_payload_bytes;
    uint64_t tx_body_bytes;
    uint8_t attempt;
    uint8_t cancelled;
    uint8_t truncated;
    uint8_t fallback_attempt;
};

struct ArtworkSummary {
    uint32_t cache_probe_hits;
    uint32_t cache_probe_misses;
    uint32_t cache_read_success;
    uint32_t cache_read_failure;
    uint32_t cache_write_success;
    uint32_t cache_write_failure;
    uint64_t compressed_read_bytes;
    uint64_t compressed_written_bytes;
    uint32_t decode_count;
    uint32_t decode_failures;
    uint64_t decode_total_us;
    uint32_t decode_max_us;
    uint32_t reserved;
};

struct ArtworkDecode {
    uint8_t context;
    uint8_t outcome;
    uint16_t reserved0;
    uint64_t duration_us;
    uint64_t compressed_input_bytes;
    uint64_t decoded_rgba_bytes;
    uint32_t reserved1;
};

struct LibrarySync {
    uint64_t duration_us;
    uint8_t outcome;
    uint8_t cache_saved;
    uint16_t reserved0;
    uint32_t views_count;
    uint32_t media_count;
    uint32_t changed_hierarchy_count;
    uint32_t request_count;
    uint32_t reserved1;
};

struct DownloadSample {
    uint32_t active_downloads;
    uint32_t queued_downloads;
    uint64_t bytes_delta;
    uint64_t bytes_per_sec;
    uint32_t segments_completed_delta;
    uint32_t segment_retries_delta;
    uint32_t planner_queue_depth;
    uint32_t reserved;
};

struct DownloadSegmentAttempt {
    uint32_t telemetry_job_seq;
    uint32_t segment_ordinal;
    uint16_t attempt_number;
    uint16_t retry_delay_ms;
    uint8_t route_kind;
    uint8_t outcome;
    uint8_t retry_planned;
    uint8_t reserved0;
    uint64_t duration_us;
    uint64_t payload_bytes;
    uint32_t http_status;
    uint32_t curl_code;
    uint64_t reserved1;
};

struct PlaybackEvent {
    uint8_t stage;
    uint8_t source;
    uint8_t child_exit_kind;
    uint8_t reserved0;
    uint64_t duration_us;
    int32_t child_exit_code;
    uint32_t reserved1;
    uint32_t playback_seq;
};

struct UiStall {
    uint8_t edge;
    uint8_t reserved0;
    uint16_t screen;
    uint16_t tab;
    uint16_t action;
    uint8_t phase;
    uint8_t reserved1[3];
    uint64_t duration_us;
    uint16_t scope_id;
    uint16_t worker_mask;
    uint64_t reserved2;
};

struct TelemetryHealth {
    uint32_t queue_depth;
    uint32_t queue_highwater;
    uint32_t dropped_records_cumulative;
    uint32_t writer_errors_cumulative;
    uint32_t rotation_count;
    uint32_t sampling_late_count;
    uint32_t buffered_bytes;
    uint32_t reserved;
};

struct SessionEvent {
    uint8_t kind;
    uint8_t outcome;
    uint16_t reserved;
    uint32_t value0;
    uint64_t value1;
};

struct TelemetryRecordHeader {
    RecordType record_type;
    uint16_t record_size;
    uint32_t sequence;
    uint64_t monotonic_us;
};

union TelemetryPayload {
    SystemSample system_sample;
    FrameTimingSummary frame_timing_summary;
    StateTransition state_transition;
    WorkerSample worker_sample;
    NetworkRequest network_request;
    ArtworkSummary artwork_summary;
    ArtworkDecode artwork_decode;
    LibrarySync library_sync;
    DownloadSample download_sample;
    DownloadSegmentAttempt download_segment_attempt;
    PlaybackEvent playback_event;
    UiStall ui_stall;
    TelemetryHealth telemetry_health;
    SessionEvent session_event;
};

struct TelemetryRecord {
    TelemetryRecordHeader header;
    TelemetryPayload payload;
};

#pragma pack(pop)

static_assert(sizeof(SystemSample) == 72, "SystemSample schema size");
static_assert(sizeof(FrameTimingSummary) == 72, "FrameTimingSummary schema size");
static_assert(sizeof(StateTransition) == 12, "StateTransition schema size");
static_assert(sizeof(WorkerSample) == 28, "WorkerSample schema size");
static_assert(sizeof(NetworkRequest) == 40, "NetworkRequest schema size");
static_assert(sizeof(ArtworkSummary) == 64, "ArtworkSummary schema size");
static_assert(sizeof(ArtworkDecode) == 32, "ArtworkDecode schema size");
static_assert(sizeof(LibrarySync) == 32, "LibrarySync schema size");
static_assert(sizeof(DownloadSample) == 40, "DownloadSample schema size");
static_assert(sizeof(DownloadSegmentAttempt) == 48, "DownloadSegmentAttempt schema size");
static_assert(sizeof(PlaybackEvent) == 24, "PlaybackEvent schema size");
static_assert(sizeof(UiStall) == 32, "UiStall schema size");
static_assert(sizeof(TelemetryHealth) == 32, "TelemetryHealth schema size");
static_assert(sizeof(SessionEvent) == 16, "SessionEvent schema size");
static_assert(sizeof(TelemetryRecordHeader) == 16, "TelemetryRecordHeader schema size");

#define MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(type) \
    static_assert(std::is_trivially_copyable<type>::value, #type " must be trivially copyable")

MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(SystemSample);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(FrameTimingSummary);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(StateTransition);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(WorkerSample);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(NetworkRequest);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(ArtworkSummary);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(ArtworkDecode);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(LibrarySync);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(DownloadSample);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(DownloadSegmentAttempt);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(PlaybackEvent);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(UiStall);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(TelemetryHealth);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(SessionEvent);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(TelemetryRecordHeader);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(TelemetryPayload);
MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL(TelemetryRecord);

static_assert(sizeof(TelemetryRecord) <= 96, "TelemetryRecord exceeds compile contract");

#undef MIYOOFIN_ASSERT_TELEMETRY_TRIVIAL

} // namespace miyoofin
