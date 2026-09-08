#pragma once

#include <cstdint>
#include <string>

namespace miyoofin {

enum ProcessMetricValidity : uint32_t {
    ProcessCpuValid = 1u << 0,
    RssValid = 1u << 1,
    PeakRssValid = 1u << 2,
    ReadBytesValid = 1u << 3,
    WriteBytesValid = 1u << 4,
    FreeStorageValid = 1u << 5,
};

struct LinuxProcessMetricsSnapshot {
    uint64_t process_cpu_us_cumulative = 0;
    uint64_t rss_kib = 0;
    uint64_t peak_rss_kib = 0;
    uint64_t process_read_bytes_cumulative = 0;
    uint64_t process_write_bytes_cumulative = 0;
    uint64_t free_storage_bytes = 0;
    uint32_t validity_flags = 0;
};

class LinuxProcessMetrics
{
public:
    explicit LinuxProcessMetrics(std::string procRoot = "/proc",
                                 std::string storagePath = "/");

    LinuxProcessMetricsSnapshot sample(bool includeFreeStorage = true) noexcept;

    // These helpers accept complete proc-file contents so desktop tests can
    // validate parsing without depending on a particular host's /proc data.
    static bool parseProcIo(const std::string &contents,
                            uint64_t &readBytes,
                            uint64_t &writeBytes) noexcept;
    static bool parseProcStatm(const std::string &contents,
                               uint64_t pageSize,
                               uint64_t &rssKib) noexcept;

    // Test/source hook for exercising the same snapshot and I/O-baseline
    // handling with deterministic metric inputs.
    LinuxProcessMetricsSnapshot sampleFromProcText(const std::string &statm,
                                                   const std::string &io,
                                                   uint64_t processCpuUs,
                                                   uint64_t peakRssKib,
                                                   uint64_t freeStorageBytes) noexcept;

private:
    LinuxProcessMetricsSnapshot makeSnapshot(uint64_t processCpuUs,
                                             bool peakValid,
                                             uint64_t peakRssKib,
                                             bool freeValid,
                                             uint64_t freeStorageBytes,
                                             const std::string &statm,
                                             const std::string &io) noexcept;

    std::string m_procRoot;
    std::string m_storagePath;
    bool m_haveLastReadBytes = false;
    bool m_haveLastWriteBytes = false;
    uint64_t m_lastReadBytes = 0;
    uint64_t m_lastWriteBytes = 0;
};

} // namespace miyoofin
