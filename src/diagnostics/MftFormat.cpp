#include "MftFormat.hpp"

namespace miyoofin {
namespace {

class LittleEndianWriter {
public:
    LittleEndianWriter(uint8_t *destination, std::size_t capacity)
        : destination_(destination), capacity_(capacity), offset_(0) {}

    bool putU8(uint8_t value) noexcept
    {
        if (offset_ >= capacity_)
            return false;
        destination_[offset_++] = value;
        return true;
    }

    bool putU16(uint16_t value) noexcept
    {
        return putU8(static_cast<uint8_t>(value))
            && putU8(static_cast<uint8_t>(value >> 8));
    }

    bool putU32(uint32_t value) noexcept
    {
        return putU8(static_cast<uint8_t>(value))
            && putU8(static_cast<uint8_t>(value >> 8))
            && putU8(static_cast<uint8_t>(value >> 16))
            && putU8(static_cast<uint8_t>(value >> 24));
    }

    bool putI32(int32_t value) noexcept
    {
        return putU32(static_cast<uint32_t>(value));
    }

    bool putU64(uint64_t value) noexcept
    {
        return putU32(static_cast<uint32_t>(value))
            && putU32(static_cast<uint32_t>(value >> 32));
    }

    std::size_t offset() const noexcept
    {
        return offset_;
    }

private:
    uint8_t *destination_;
    std::size_t capacity_;
    std::size_t offset_;
};

bool putCommonHeader(LittleEndianWriter &writer,
                     const TelemetryRecord &record,
                     uint16_t size) noexcept
{
    return writer.putU16(static_cast<uint16_t>(record.header.record_type))
        && writer.putU16(size)
        && writer.putU32(record.header.sequence)
        && writer.putU64(record.header.monotonic_us);
}

} // namespace

bool encodeFileHeader(const MftFileHeader &header,
                      uint8_t *destination,
                      std::size_t capacity) noexcept
{
    if (destination == nullptr || capacity < kMftFileHeaderSize)
        return false;

    LittleEndianWriter writer(destination, capacity);
    return writer.putU8('M')
        && writer.putU8('F')
        && writer.putU8('T')
        && writer.putU8('1')
        && writer.putU16(1)
        && writer.putU16(static_cast<uint16_t>(kMftFileHeaderSize))
        && writer.putU32(header.flags)
        && writer.putU32(header.pid)
        && writer.putU64(header.session_nonce)
        && writer.putU64(header.start_monotonic_us)
        && writer.putU64(header.optional_wall_time_s)
        && writer.putU32(header.sample_interval_ms)
        && writer.putU32(header.free_space_interval_ms)
        && writer.putU32(header.rotation_index)
        && writer.putU32(header.writer_buffer_bytes)
        && writer.putU32(header.app_version_packed)
        && writer.putU32(header.build_commit32)
        && writer.putU32(header.platform_id)
        && writer.putU32(0)
        && writer.putU64(0)
        && writer.offset() == kMftFileHeaderSize;
}

uint16_t encodedRecordSize(RecordType type) noexcept
{
    switch (type) {
    case RecordType::SystemSample:
        return 88;
    case RecordType::FrameTimingSummary:
        return 88;
    case RecordType::StateTransition:
        return 28;
    case RecordType::WorkerSample:
        return 44;
    case RecordType::NetworkRequest:
        return 56;
    case RecordType::ArtworkSummary:
        return 80;
    case RecordType::ArtworkDecode:
        return 48;
    case RecordType::LibrarySync:
        return 48;
    case RecordType::DownloadSample:
        return 56;
    case RecordType::DownloadSegmentAttempt:
        return 64;
    case RecordType::PlaybackEvent:
        return 40;
    case RecordType::UiStall:
        return 48;
    case RecordType::TelemetryHealth:
        return 48;
    case RecordType::SessionEvent:
        return 32;
    }
    return 0;
}

bool encodeRecord(const TelemetryRecord &record,
                  uint8_t *destination,
                  std::size_t capacity,
                  uint16_t &written) noexcept
{
    written = 0;
    const uint16_t size = encodedRecordSize(record.header.record_type);
    if (size == 0 || size > kMftMaxRecordSize || destination == nullptr || capacity < size)
        return false;

    LittleEndianWriter writer(destination, capacity);
    if (!putCommonHeader(writer, record, size))
        return false;

    bool success = false;
    switch (record.header.record_type) {
    case RecordType::SystemSample: {
        const SystemSample &value = record.payload.system_sample;
        success = writer.putU64(value.process_cpu_us_cumulative)
            && writer.putU64(value.rss_kib)
            && writer.putU64(value.peak_rss_kib)
            && writer.putU64(value.process_read_bytes_cumulative)
            && writer.putU64(value.process_write_bytes_cumulative)
            && writer.putU64(value.free_storage_bytes)
            && writer.putU64(value.telemetry_logical_bytes_written)
            && writer.putU32(value.dropped_records_cumulative)
            && writer.putU32(value.free_storage_sample_age_ms)
            && writer.putU32(value.validity_flags)
            && writer.putU32(value.reserved);
        break;
    }
    case RecordType::FrameTimingSummary: {
        const FrameTimingSummary &value = record.payload.frame_timing_summary;
        success = writer.putU8(value.phase)
            && writer.putU8(value.reserved0[0])
            && writer.putU8(value.reserved0[1])
            && writer.putU8(value.reserved0[2])
            && writer.putU32(value.interval_us)
            && writer.putU32(value.sample_count)
            && writer.putU64(value.total_us)
            && writer.putU32(value.max_us)
            && writer.putU32(value.over_50ms_count)
            && writer.putU32(value.over_100ms_count);
        for (uint32_t count : value.histogram)
            success = success && writer.putU32(count);
        success = success && writer.putU32(value.reserved1);
        break;
    }
    case RecordType::StateTransition: {
        const StateTransition &value = record.payload.state_transition;
        success = writer.putU8(value.state_kind)
            && writer.putU8(value.reserved0)
            && writer.putU16(value.previous_id)
            && writer.putU16(value.current_id)
            && writer.putU16(value.reserved1)
            && writer.putU32(value.transition_seq);
        break;
    }
    case RecordType::WorkerSample: {
        const WorkerSample &value = record.payload.worker_sample;
        success = writer.putU16(value.worker_id)
            && writer.putU8(value.active)
            && writer.putU8(value.reserved0)
            && writer.putU32(value.queue_depth)
            && writer.putU32(value.queue_highwater)
            && writer.putU32(value.completed_delta)
            && writer.putU32(value.failed_delta)
            && writer.putU32(value.cancelled_delta)
            && writer.putU32(value.reserved1);
        break;
    }
    case RecordType::NetworkRequest: {
        const NetworkRequest &value = record.payload.network_request;
        success = writer.putU16(value.request_kind)
            && writer.putU8(value.route_kind)
            && writer.putU8(value.method)
            && writer.putU64(value.duration_us)
            && writer.putU32(value.http_status)
            && writer.putU32(value.curl_code)
            && writer.putU64(value.rx_payload_bytes)
            && writer.putU64(value.tx_body_bytes)
            && writer.putU8(value.attempt)
            && writer.putU8(value.cancelled)
            && writer.putU8(value.truncated)
            && writer.putU8(value.fallback_attempt);
        break;
    }
    case RecordType::ArtworkSummary: {
        const ArtworkSummary &value = record.payload.artwork_summary;
        success = writer.putU32(value.cache_probe_hits)
            && writer.putU32(value.cache_probe_misses)
            && writer.putU32(value.cache_read_success)
            && writer.putU32(value.cache_read_failure)
            && writer.putU32(value.cache_write_success)
            && writer.putU32(value.cache_write_failure)
            && writer.putU64(value.compressed_read_bytes)
            && writer.putU64(value.compressed_written_bytes)
            && writer.putU32(value.decode_count)
            && writer.putU32(value.decode_failures)
            && writer.putU64(value.decode_total_us)
            && writer.putU32(value.decode_max_us)
            && writer.putU32(value.reserved);
        break;
    }
    case RecordType::ArtworkDecode: {
        const ArtworkDecode &value = record.payload.artwork_decode;
        success = writer.putU8(value.context)
            && writer.putU8(value.outcome)
            && writer.putU16(value.reserved0)
            && writer.putU64(value.duration_us)
            && writer.putU64(value.compressed_input_bytes)
            && writer.putU64(value.decoded_rgba_bytes)
            && writer.putU32(value.reserved1);
        break;
    }
    case RecordType::LibrarySync: {
        const LibrarySync &value = record.payload.library_sync;
        success = writer.putU64(value.duration_us)
            && writer.putU8(value.outcome)
            && writer.putU8(value.cache_saved)
            && writer.putU16(value.reserved0)
            && writer.putU32(value.views_count)
            && writer.putU32(value.media_count)
            && writer.putU32(value.changed_hierarchy_count)
            && writer.putU32(value.request_count)
            && writer.putU32(value.reserved1);
        break;
    }
    case RecordType::DownloadSample: {
        const DownloadSample &value = record.payload.download_sample;
        success = writer.putU32(value.active_downloads)
            && writer.putU32(value.queued_downloads)
            && writer.putU64(value.bytes_delta)
            && writer.putU64(value.bytes_per_sec)
            && writer.putU32(value.segments_completed_delta)
            && writer.putU32(value.segment_retries_delta)
            && writer.putU32(value.planner_queue_depth)
            && writer.putU32(value.reserved);
        break;
    }
    case RecordType::DownloadSegmentAttempt: {
        const DownloadSegmentAttempt &value = record.payload.download_segment_attempt;
        success = writer.putU32(value.telemetry_job_seq)
            && writer.putU32(value.segment_ordinal)
            && writer.putU16(value.attempt_number)
            && writer.putU16(value.retry_delay_ms)
            && writer.putU8(value.route_kind)
            && writer.putU8(value.outcome)
            && writer.putU8(value.retry_planned)
            && writer.putU8(value.reserved0)
            && writer.putU64(value.duration_us)
            && writer.putU64(value.payload_bytes)
            && writer.putU32(value.http_status)
            && writer.putU32(value.curl_code)
            && writer.putU64(value.reserved1);
        break;
    }
    case RecordType::PlaybackEvent: {
        const PlaybackEvent &value = record.payload.playback_event;
        success = writer.putU8(value.stage)
            && writer.putU8(value.source)
            && writer.putU8(value.child_exit_kind)
            && writer.putU8(value.reserved0)
            && writer.putU64(value.duration_us)
            && writer.putI32(value.child_exit_code)
            && writer.putU32(value.reserved1)
            && writer.putU32(value.playback_seq);
        break;
    }
    case RecordType::UiStall: {
        const UiStall &value = record.payload.ui_stall;
        success = writer.putU8(value.edge)
            && writer.putU8(value.reserved0)
            && writer.putU16(value.screen)
            && writer.putU16(value.tab)
            && writer.putU16(value.action)
            && writer.putU8(value.phase)
            && writer.putU8(value.reserved1[0])
            && writer.putU8(value.reserved1[1])
            && writer.putU8(value.reserved1[2])
            && writer.putU64(value.duration_us)
            && writer.putU16(value.scope_id)
            && writer.putU16(value.worker_mask)
            && writer.putU64(value.reserved2);
        break;
    }
    case RecordType::TelemetryHealth: {
        const TelemetryHealth &value = record.payload.telemetry_health;
        success = writer.putU32(value.queue_depth)
            && writer.putU32(value.queue_highwater)
            && writer.putU32(value.dropped_records_cumulative)
            && writer.putU32(value.writer_errors_cumulative)
            && writer.putU32(value.rotation_count)
            && writer.putU32(value.sampling_late_count)
            && writer.putU32(value.buffered_bytes)
            && writer.putU32(value.reserved);
        break;
    }
    case RecordType::SessionEvent: {
        const SessionEvent &value = record.payload.session_event;
        success = writer.putU8(value.kind)
            && writer.putU8(value.outcome)
            && writer.putU16(value.reserved)
            && writer.putU32(value.value0)
            && writer.putU64(value.value1);
        break;
    }
    }

    if (!success || writer.offset() != size)
        return false;
    written = size;
    return true;
}

} // namespace miyoofin
