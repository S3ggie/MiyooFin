#pragma once

#include <cstddef>
#include <cstdint>

#include "TelemetryTypes.hpp"

namespace miyoofin {

constexpr std::size_t kMftFileHeaderSize = 80;
constexpr std::size_t kMftRecordHeaderSize = 16;
constexpr std::size_t kMftMaxRecordSize = 256;

struct MftFileHeader {
    uint16_t schema_version = 1;
    uint32_t flags;
    uint32_t pid;
    uint64_t session_nonce;
    uint64_t start_monotonic_us;
    uint64_t optional_wall_time_s;
    uint32_t sample_interval_ms;
    uint32_t free_space_interval_ms;
    uint32_t rotation_index;
    uint32_t writer_buffer_bytes;
    uint32_t app_version_packed;
    uint32_t build_commit32;
    uint32_t platform_id;
};

bool encodeFileHeader(const MftFileHeader &header,
                      uint8_t *destination,
                      std::size_t capacity) noexcept;

uint16_t encodedRecordSize(RecordType type) noexcept;

bool encodeRecord(const TelemetryRecord &record,
                  uint8_t *destination,
                  std::size_t capacity,
                  uint16_t &written) noexcept;

constexpr uint16_t kMftV2CatalogDbSummarySize = 128;

bool encodeCatalogDbSummaryRecord(const CatalogDbSummaryRecord &record,
                                  uint8_t *destination,
                                  std::size_t capacity,
                                  uint16_t &written) noexcept;

} // namespace miyoofin
