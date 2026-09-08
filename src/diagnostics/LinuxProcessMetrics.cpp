#include "LinuxProcessMetrics.hpp"

#include <charconv>
#include <fstream>
#include <limits>
#include <iterator>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>

#include "TelemetryClock.hpp"

namespace miyoofin {
namespace {

bool parseUnsigned(const std::string &text, uint64_t &value) noexcept
{
    if (text.empty())
        return false;
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool multiplyToKib(uint64_t pages, uint64_t pageSize, uint64_t &kib) noexcept
{
    if (pageSize == 0 || pages > std::numeric_limits<uint64_t>::max() / pageSize)
        return false;
    const uint64_t bytes = pages * pageSize;
    kib = bytes / 1024ull;
    return true;
}

bool multiplyFreeBytes(unsigned long long blocks,
                       unsigned long long blockSize,
                       uint64_t &freeBytes) noexcept
{
    if (blockSize == 0 || blocks > std::numeric_limits<uint64_t>::max() / blockSize)
        return false;
    freeBytes = static_cast<uint64_t>(blocks * blockSize);
    return true;
}

std::string readFile(const std::string &path)
{
    std::ifstream stream(path);
    if (!stream)
        return {};
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

} // namespace

LinuxProcessMetrics::LinuxProcessMetrics(std::string procRoot,
                                         std::string storagePath)
    : m_procRoot(std::move(procRoot)),
      m_storagePath(std::move(storagePath))
{
}

bool LinuxProcessMetrics::parseProcIo(const std::string &contents,
                                      uint64_t &readBytes,
                                      uint64_t &writeBytes) noexcept
{
    bool foundRead = false;
    bool foundWrite = false;
    std::size_t lineStart = 0;
    while (lineStart <= contents.size()) {
        const std::size_t lineEnd = contents.find('\n', lineStart);
        const std::size_t length = lineEnd == std::string::npos
            ? contents.size() - lineStart : lineEnd - lineStart;
        const std::string line = contents.substr(lineStart, length);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string key = line.substr(0, colon);
            std::size_t valueStart = colon + 1;
            while (valueStart < line.size() && line[valueStart] == ' ')
                ++valueStart;
            std::size_t valueEnd = valueStart;
            while (valueEnd < line.size() && line[valueEnd] != ' ' && line[valueEnd] != '\t')
                ++valueEnd;
            uint64_t value = 0;
            if ((key == "read_bytes" || key == "write_bytes")
                && parseUnsigned(line.substr(valueStart, valueEnd - valueStart), value)) {
                if (key == "read_bytes") {
                    readBytes = value;
                    foundRead = true;
                } else {
                    writeBytes = value;
                    foundWrite = true;
                }
            }
        }
        if (lineEnd == std::string::npos)
            break;
        lineStart = lineEnd + 1;
    }
    return foundRead && foundWrite;
}

bool LinuxProcessMetrics::parseProcStatm(const std::string &contents,
                                         uint64_t pageSize,
                                         uint64_t &rssKib) noexcept
{
    std::size_t tokenStart = 0;
    uint64_t residentPages = 0;
    for (int token = 0; token < 2; ++token) {
        while (tokenStart < contents.size()
            && (contents[tokenStart] == ' ' || contents[tokenStart] == '\t'
                || contents[tokenStart] == '\n'))
            ++tokenStart;
        if (tokenStart == contents.size())
            return false;
        std::size_t tokenEnd = tokenStart;
        while (tokenEnd < contents.size()
            && contents[tokenEnd] != ' ' && contents[tokenEnd] != '\t'
            && contents[tokenEnd] != '\n')
            ++tokenEnd;
        uint64_t value = 0;
        if (!parseUnsigned(contents.substr(tokenStart, tokenEnd - tokenStart), value))
            return false;
        if (token == 1)
            residentPages = value;
        tokenStart = tokenEnd;
    }
    return multiplyToKib(residentPages, pageSize, rssKib);
}

LinuxProcessMetricsSnapshot LinuxProcessMetrics::makeSnapshot(
    uint64_t processCpuUs,
    bool peakValid,
    uint64_t peakRssKib,
    bool freeValid,
    uint64_t freeStorageBytes,
    const std::string &statm,
    const std::string &io) noexcept
{
    LinuxProcessMetricsSnapshot snapshot{};
    snapshot.process_cpu_us_cumulative = processCpuUs;
    if (processCpuUs != 0)
        snapshot.validity_flags |= ProcessCpuValid;

    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize > 0 && parseProcStatm(statm, static_cast<uint64_t>(pageSize), snapshot.rss_kib))
        snapshot.validity_flags |= RssValid;

    snapshot.peak_rss_kib = peakRssKib;
    if (peakValid)
        snapshot.validity_flags |= PeakRssValid;

    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;
    if (parseProcIo(io, readBytes, writeBytes)) {
        snapshot.process_read_bytes_cumulative = readBytes;
        snapshot.process_write_bytes_cumulative = writeBytes;

        if (!m_haveLastReadBytes || readBytes >= m_lastReadBytes) {
            snapshot.validity_flags |= ReadBytesValid;
        }
        if (!m_haveLastWriteBytes || writeBytes >= m_lastWriteBytes) {
            snapshot.validity_flags |= WriteBytesValid;
        }
        m_haveLastReadBytes = true;
        m_haveLastWriteBytes = true;
        m_lastReadBytes = readBytes;
        m_lastWriteBytes = writeBytes;
    } else {
        m_haveLastReadBytes = false;
        m_haveLastWriteBytes = false;
    }

    snapshot.free_storage_bytes = freeStorageBytes;
    if (freeValid)
        snapshot.validity_flags |= FreeStorageValid;
    return snapshot;
}

LinuxProcessMetricsSnapshot LinuxProcessMetrics::sampleFromProcText(
    const std::string &statm,
    const std::string &io,
    uint64_t processCpuUs,
    uint64_t peakRssKib,
    uint64_t freeStorageBytes) noexcept
{
    return makeSnapshot(processCpuUs, true, peakRssKib, true, freeStorageBytes, statm, io);
}

LinuxProcessMetricsSnapshot LinuxProcessMetrics::sample(bool includeFreeStorage) noexcept
{
    const uint64_t processCpuUs = TelemetryClock::processCpuUs();
    uint64_t peakRssKib = 0;
    struct rusage usage{};
    const bool peakValid = ::getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss >= 0;
    if (peakValid)
        peakRssKib = static_cast<uint64_t>(usage.ru_maxrss);

    uint64_t freeStorageBytes = 0;
    bool freeValid = false;
    if (includeFreeStorage) {
        struct statvfs storage{};
        freeValid = ::statvfs(m_storagePath.c_str(), &storage) == 0
            && multiplyFreeBytes(storage.f_bavail, storage.f_frsize, freeStorageBytes);
    }

    return makeSnapshot(processCpuUs, peakValid, peakRssKib, freeValid,
                        freeStorageBytes,
                        readFile(m_procRoot + "/self/statm"),
                        readFile(m_procRoot + "/self/io"));
}

} // namespace miyoofin
