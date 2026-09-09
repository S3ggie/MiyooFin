#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "MftFormat.hpp"
#include "TelemetryConfig.hpp"
#include "TelemetryIds.hpp"

namespace miyoofin {

class TelemetryWriter
{
public:
    TelemetryWriter() = default;
    ~TelemetryWriter() noexcept;

    TelemetryWriter(const TelemetryWriter &) = delete;
    TelemetryWriter &operator=(const TelemetryWriter &) = delete;

    bool open(const TelemetryConfig &config, const MftFileHeader &header);
    bool append(const TelemetryRecord &record);
    bool appendCatalogDbSummary(const CatalogDbSummaryRecord &record);
    bool flushIfDue(uint64_t monotonicUs) noexcept;
    bool flush() noexcept;
    bool close() noexcept;

    bool isOpen() const noexcept;
    uint64_t logicalBytesWritten() const noexcept;
    std::size_t bufferedBytes() const noexcept;
    uint32_t rotationCount() const noexcept;
    WriterErrorKind lastErrorKind() const noexcept;
    int lastErrorNumber() const noexcept;

private:
    void setError(WriterErrorKind kind, int errorNumber) noexcept;

    FILE *file_ = nullptr;
    std::vector<uint8_t> buffer_;
    std::size_t buffered_ = 0;
    std::string path_;
    TelemetryConfig config_;
    MftFileHeader header_{};
    uint64_t logicalBytesWritten_ = 0;
    uint64_t lastFlushMonotonicUs_ = 0;
    uint32_t flushIntervalMs_ = 0;
    uint32_t rotationCount_ = 0;
    WriterErrorKind lastErrorKind_ = WriterErrorKind::Unknown;
    int lastErrorNumber_ = 0;
};

} // namespace miyoofin
