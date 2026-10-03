#include "MpegTsValidator.hpp"
#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <map>
#include <unistd.h>
#include <vector>

namespace miyoofin {

namespace {

constexpr std::size_t kPacket = 188;
constexpr std::size_t kHeaderPackets = 16; // PAT and PMT must appear within these
constexpr std::size_t kMinPackets = 4;

std::uint32_t crcStep(std::uint32_t crc, unsigned char byte)
{
    crc ^= static_cast<std::uint32_t>(byte) << 24;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
    return crc;
}

TsVerdict fail(const std::string& reason)
{
    TsVerdict v;
    v.reason = reason;
    return v;
}

// Walks packets and keeps the PSI/continuity state. `full` also enforces continuity counters and
// PES framing, which only make sense when every packet is fed in order.
class Scanner
{
  public:
    explicit Scanner(bool full) : m_full(full) {}

    // Checks one packet; false (with reason()) when it is invalid.
    bool packet(const unsigned char* p, std::size_t index, bool sequential)
    {
        if (p[0] != 0x47)
            return bad("packet " + std::to_string(index) + " lacks the sync byte");
        if (p[1] & 0x80)
            return bad("packet " + std::to_string(index) + " has the transport error flag");
        if (!sequential)
            return true; // sampled packets: framing only
        const unsigned pid = ((p[1] & 0x1f) << 8) | p[2];
        const bool pusi = (p[1] & 0x40) != 0;
        const unsigned afc = (p[3] >> 4) & 3;
        const unsigned cc = p[3] & 15;
        if (afc == 0)
            return bad("packet " + std::to_string(index) + " has a reserved adaptation value");
        std::size_t payload = 4;
        bool discontinuity = false;
        if (afc & 2) {
            const unsigned len = p[4];
            if (len > 183 || (afc == 2 && len != 183))
                return bad("packet " + std::to_string(index) + " has a bad adaptation field");
            if (len > 0)
                discontinuity = (p[5] & 0x80) != 0;
            payload = 5 + len;
        }
        const bool hasPayload = (afc & 1) != 0;
        if (m_full && hasPayload && pid != 0x1fff) {
            auto it = m_cc.find(pid);
            if (it != m_cc.end() && !discontinuity) {
                const unsigned expected = (it->second + 1) & 15;
                if (cc != expected && cc != it->second) // a repeat of the last one is allowed
                    return bad("continuity error on PID " + std::to_string(pid) + " at packet " +
                               std::to_string(index));
            }
            m_cc[pid] = cc;
        }
        if (!hasPayload || payload >= kPacket)
            return true;
        const unsigned char* body = p + payload;
        const std::size_t bodySize = kPacket - payload;
        if (pid == 0 && pusi && !m_patOk)
            return parsePat(body, bodySize);
        if (m_patOk && pid == m_pmtPid && pusi && !m_pmtOk)
            return parsePmt(body, bodySize);
        if (m_full && m_pmtOk && pusi && m_esPids.count(pid)) {
            // A packet that starts a frame starts a PES packet: 00 00 01 + stream id.
            if (bodySize < 4 || body[0] != 0 || body[1] != 0 || body[2] != 1)
                return bad("PES header missing on PID " + std::to_string(pid) + " at packet " +
                           std::to_string(index));
            const unsigned id = body[3];
            const bool video = m_esPids[pid] == 0x1b;
            if (video ? (id < 0xe0 || id > 0xef) : (id < 0xc0 || id > 0xdf))
                return bad("unexpected PES stream id on PID " + std::to_string(pid));
            if (video)
                m_videoSeen = true;
        }
        return true;
    }

    // After the header packets: PAT and PMT must have been found.
    bool headerComplete() const
    {
        return m_patOk && m_pmtOk;
    }
    // After the whole file.
    bool finish(bool full)
    {
        if (!m_patOk)
            return bad("no valid PAT");
        if (!m_pmtOk)
            return bad("no valid PMT");
        if (full && !m_videoSeen)
            return bad("no video frame start");
        return true;
    }
    const std::string& reason() const
    {
        return m_reason;
    }

  private:
    bool bad(const std::string& why)
    {
        if (m_reason.empty())
            m_reason = why;
        return false;
    }
    // A PSI section: pointer_field, then table_id, section_length, ... CRC-32 over all of it.
    bool section(const unsigned char* body, std::size_t size, unsigned tableId,
                 const unsigned char*& sec, std::size_t& secLen)
    {
        if (size < 1)
            return false;
        const std::size_t pointer = body[0];
        if (1 + pointer + 3 > size)
            return false;
        sec = body + 1 + pointer;
        if (sec[0] != tableId || (sec[1] & 0x80) == 0) // section_syntax_indicator
            return false;
        const std::size_t length = ((sec[1] & 0x0f) << 8) | sec[2];
        secLen = 3 + length;
        if (length < 9 || 1 + pointer + secLen > size)
            return false; // sections longer than one packet are not produced for this profile
        std::uint32_t crc = 0xffffffffu;
        for (std::size_t i = 0; i < secLen; ++i)
            crc = crcStep(crc, sec[i]);
        return crc == 0; // the CRC covers itself, so a valid section leaves zero
    }
    bool parsePat(const unsigned char* body, std::size_t size)
    {
        const unsigned char* sec = nullptr;
        std::size_t len = 0;
        if (!section(body, size, 0x00, sec, len))
            return bad("PAT is missing or its CRC is wrong");
        for (std::size_t i = 8; i + 4 <= len - 4; i += 4) {
            const unsigned program = (sec[i] << 8) | sec[i + 1];
            if (program != 0) {
                m_pmtPid = ((sec[i + 2] & 0x1f) << 8) | sec[i + 3];
                m_patOk = true;
                return true;
            }
        }
        return bad("PAT lists no program");
    }
    bool parsePmt(const unsigned char* body, std::size_t size)
    {
        const unsigned char* sec = nullptr;
        std::size_t len = 0;
        if (!section(body, size, 0x02, sec, len))
            return bad("PMT is missing or its CRC is wrong");
        if (len < 16)
            return bad("PMT is too short");
        const std::size_t infoLength = ((sec[10] & 0x0f) << 8) | sec[11];
        std::size_t at = 12 + infoLength;
        bool video = false, audio = false;
        while (at + 5 <= len - 4) {
            const unsigned type = sec[at];
            const unsigned pid = ((sec[at + 1] & 0x1f) << 8) | sec[at + 2];
            const std::size_t es = ((sec[at + 3] & 0x0f) << 8) | sec[at + 4];
            m_esPids[pid] = type;
            video = video || type == 0x1b; // H.264
            audio = audio || type == 0x0f; // AAC (ADTS)
            at += 5 + es;
        }
        if (at != len - 4)
            return bad("PMT stream list is malformed");
        if (!video || !audio)
            return bad("PMT does not declare H.264 video and AAC audio");
        m_pmtOk = true;
        return true;
    }

    bool m_full;
    bool m_patOk = false, m_pmtOk = false, m_videoSeen = false;
    unsigned m_pmtPid = 0;
    std::map<unsigned, unsigned> m_esPids; // elementary PID -> stream_type
    std::map<unsigned, unsigned> m_cc;     // PID -> last continuity counter
    std::string m_reason;
};

} // namespace

std::uint32_t mpegCrc32(const unsigned char* data, std::size_t size)
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i)
        crc = crcStep(crc, data[i]);
    return crc;
}

TsVerdict validateMpegTs(const unsigned char* data, std::size_t size, TsDepth depth)
{
    if (!data || size < kMinPackets * kPacket)
        return fail("too small to be a transport stream segment");
    if (size % kPacket != 0)
        return fail("size is not a whole number of 188-byte packets");
    const std::size_t packets = size / kPacket;
    const bool full = depth == TsDepth::Full;
    Scanner scanner(full);
    const std::size_t head = std::min(packets, kHeaderPackets);
    for (std::size_t i = 0; i < head; ++i)
        if (!scanner.packet(data + i * kPacket, i, true))
            return fail(scanner.reason());
    if (!scanner.headerComplete())
        return fail("PAT and PMT are not at the start");
    if (full) {
        for (std::size_t i = head; i < packets; ++i)
            if (!scanner.packet(data + i * kPacket, i, true))
                return fail(scanner.reason());
    } else {
        // Sampled framing checks across the file, and the last packet.
        const std::size_t samples = 8;
        for (std::size_t s = 1; s <= samples; ++s) {
            const std::size_t i = head + (packets - head) * s / (samples + 1);
            if (i < packets && !scanner.packet(data + i * kPacket, i, false))
                return fail(scanner.reason());
        }
        if (!scanner.packet(data + (packets - 1) * kPacket, packets - 1, false))
            return fail(scanner.reason());
    }
    if (!scanner.finish(full))
        return fail(scanner.reason());
    TsVerdict ok;
    ok.ok = true;
    return ok;
}

TsVerdict validateMpegTsFile(const std::string& path, std::uint64_t size, TsDepth depth)
{
    if (size < kMinPackets * kPacket)
        return fail("too small to be a transport stream segment");
    if (size % kPacket != 0)
        return fail("size is not a whole number of 188-byte packets");
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return fail("cannot open the segment");
    auto readAt = [&](std::uint64_t offset, unsigned char* out, std::size_t n) {
        std::size_t done = 0;
        while (done < n) {
            const ssize_t got =
                ::pread(fd, out + done, n - done, static_cast<off_t>(offset + done));
            if (got <= 0)
                return false;
            done += static_cast<std::size_t>(got);
        }
        return true;
    };
    const std::size_t packets = static_cast<std::size_t>(size / kPacket);
    TsVerdict verdict;
    if (depth == TsDepth::Full) {
        // Stream the whole file through the same checks, in 64 KiB-ish chunks.
        std::vector<unsigned char> chunk(kPacket * 256);
        Scanner scanner(true);
        std::size_t index = 0;
        bool ok = true;
        while (ok && index < packets) {
            const std::size_t n = std::min<std::size_t>(256, packets - index);
            if (!readAt(static_cast<std::uint64_t>(index) * kPacket, chunk.data(), n * kPacket)) {
                ::close(fd);
                return fail("cannot read the segment");
            }
            for (std::size_t k = 0; ok && k < n; ++k) {
                ok = scanner.packet(chunk.data() + k * kPacket, index + k, true);
                if (ok && index + k + 1 == kHeaderPackets && !scanner.headerComplete()) {
                    ::close(fd);
                    return fail("PAT and PMT are not at the start");
                }
            }
            index += n;
        }
        ::close(fd);
        if (!ok)
            return fail(scanner.reason());
        if (packets < kHeaderPackets && !scanner.headerComplete())
            return fail("PAT and PMT are not at the start");
        if (!scanner.finish(true))
            return fail(scanner.reason());
        verdict.ok = true;
        return verdict;
    }
    // Quick: the opening packets as one read, then a handful of single packets.
    const std::size_t head = std::min(packets, kHeaderPackets);
    std::vector<unsigned char> buffer(head * kPacket);
    if (!readAt(0, buffer.data(), buffer.size())) {
        ::close(fd);
        return fail("cannot read the segment");
    }
    Scanner scanner(false);
    for (std::size_t i = 0; i < head; ++i)
        if (!scanner.packet(buffer.data() + i * kPacket, i, true)) {
            ::close(fd);
            return fail(scanner.reason());
        }
    if (!scanner.headerComplete()) {
        ::close(fd);
        return fail("PAT and PMT are not at the start");
    }
    unsigned char one[kPacket];
    const std::size_t samples = 8;
    for (std::size_t s = 1; s <= samples + 1; ++s) {
        const std::size_t i =
            s == samples + 1 ? packets - 1 : head + (packets - head) * s / (samples + 1);
        if (i >= packets)
            continue;
        if (!readAt(static_cast<std::uint64_t>(i) * kPacket, one, kPacket) ||
            !scanner.packet(one, i, false)) {
            ::close(fd);
            return fail(scanner.reason().empty() ? "cannot read the segment" : scanner.reason());
        }
    }
    ::close(fd);
    if (!scanner.finish(false))
        return fail(scanner.reason());
    verdict.ok = true;
    return verdict;
}

} // namespace miyoofin
