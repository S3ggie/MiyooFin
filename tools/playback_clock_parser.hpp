// tools/playback_clock_parser.hpp — Pure parsing helpers for FFplay showinfo
// pts_time extraction and Jellyfin PositionTicks conversion.
// Extracted so they can be unit-tested independently of the reporter.
//
// B5f3b: Uses sampled showinfo pts_time as the real playback clock.
// The old A-V/M-V master-clock parsing has been removed because Onion
// FFplay does not emit usable A-V/M-V status lines.
#ifndef PLAYBACK_CLOCK_PARSER_HPP
#define PLAYBACK_CLOCK_PARSER_HPP

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

// -------------------------------------------------------------------
// Seconds → Jellyfin PositionTicks conversion.
// Jellyfin ticks are 100 ns units: 1 second = 10,000,000 ticks.
// Negative or non-finite inputs clamp to 0.
// -------------------------------------------------------------------
inline int64_t seconds_to_ticks(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) return 0;
    const double maxTicks =
        static_cast<double>(std::numeric_limits<int64_t>::max());
    if (seconds * 10000000.0 >= maxTicks)
        return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(seconds * 10000000.0 + 0.5);
}

// Parse playback-request.txt's resume_ticks value.  Only a complete signed
// decimal int64 is accepted; missing, malformed, negative, and overflowing
// values normalize to zero.
inline int64_t parse_resume_ticks(const std::string &value)
{
    if (value.empty()) return 0;
    size_t consumed = 0;
    try {
        long long parsed = std::stoll(value, &consumed, 10);
        if (consumed != value.size() || parsed < 0) return 0;
        return static_cast<int64_t>(parsed);
    } catch (...) {
        return 0;
    }
}

// Convert a local playback clock to Jellyfin's absolute clock.  Negative
// inputs normalize to zero and overflow saturates deterministically.
inline int64_t add_resume_ticks(int64_t resumeTicks, int64_t localTicks)
{
    if (resumeTicks < 0) resumeTicks = 0;
    if (localTicks < 0) localTicks = 0;
    const int64_t maxTicks = std::numeric_limits<int64_t>::max();
    if (localTicks > maxTicks - resumeTicks) return maxTicks;
    return resumeTicks + localTicks;
}

inline int64_t absolute_position_ticks(int64_t resumeTicks,
                                       double localSeconds)
{
    return add_resume_ticks(resumeTicks, seconds_to_ticks(localSeconds));
}

// -------------------------------------------------------------------
// Try to extract a playback clock from a single FFplay log record by
// parsing a showinfo pts_time value.
//
// The filtergraph:
//   split=2[main][tap];[tap]select=...,showinfo,nullsink
//
// causes FFplay's showinfo filter to emit lines such as:
//
//   [Parsed_showinfo_3 @ 0x...] n:   0 pts:  47107 pts_time:1.46673 ...
//   [Parsed_showinfo_3 @ 0x...] n:   1 pts: 208057 pts_time:6.47173 ...
//
// We look for the literal substring "pts_time:" and parse the decimal
// value immediately following it.  We reject negative, NaN, Inf values.
//
// Returns true on success and writes the PTS in seconds to `outSeconds`.
// Returns false if the line does not contain a valid pts_time value.
// -------------------------------------------------------------------
inline bool parse_showinfo_pts(const std::string &record, double &outSeconds)
{
    if (record.empty()) return false;

    // Find "pts_time:" in the record
    const char needle[] = "pts_time:";
    const size_t needleLen = sizeof(needle) - 1; // 10

    size_t pos = record.find(needle);
    if (pos == std::string::npos) return false;

    pos += needleLen;

    // Must not be at end of string
    if (pos >= record.size()) return false;

    // The next character must be a digit, '.', or '-' (for negative)
    char first = record[pos];
    if (!((first >= '0' && first <= '9') || first == '.' || first == '-'))
        return false;

    // Use strtod to parse the numeric value properly
    const char *start = record.c_str() + pos;
    char *endp = nullptr;
    double val = std::strtod(start, &endp);

    // Must have consumed at least one digit
    if (endp == start) return false;
    // Must have consumed at least one digit (not just a dot or sign)
    if (endp - start == 1 && (first == '.' || first == '-')) return false;

    // Reject NaN, Inf, negative
    if (!std::isfinite(val) || val < 0.0) return false;

    outSeconds = val;
    return true;
}

// -------------------------------------------------------------------
// Decide whether a new sampled PTS should trigger PlaybackStart or
// PlaybackProgress.  Call once per valid PTS event.
//
// Returns false on the FIRST call (caller should send Start).
// Returns true on every subsequent call (caller should send Progress).
//
// startAttempted is set to true on the first call and left there.
// This ensures PlaybackStart is attempted exactly once, regardless
// of whether the HTTP request succeeded.
// -------------------------------------------------------------------
inline bool pts_event(bool &startAttempted)
{
    bool isProgress = startAttempted;
    startAttempted = true;
    return isProgress;
}

// -------------------------------------------------------------------
// Extract a complete record from a growing buffer that uses \r or \n
// as record delimiters (FFplay frequently uses \r).
//
// On success, sets `record` to the next complete record (without the
// delimiter) and returns true.  Returns false if no complete record
// is available yet.
//
// Advances `pos` past the consumed bytes.
// -------------------------------------------------------------------
inline bool extract_record(const std::string &buf, size_t &pos,
                           std::string &record)
{
    if (pos >= buf.size()) return false;

    // Look for \n or \r
    size_t nl = buf.find('\n', pos);
    size_t cr = buf.find('\r', pos);

    size_t end = std::string::npos;
    if (nl != std::string::npos && cr != std::string::npos)
        end = (nl < cr) ? nl : cr;
    else if (nl != std::string::npos)
        end = nl;
    else if (cr != std::string::npos)
        end = cr;

    if (end == std::string::npos) return false;  // no delimiter yet

    record = buf.substr(pos, end - pos);
    pos = end + 1;

    // Skip the other delimiter if it's a \r\n or \n\r pair
    if (pos < buf.size()) {
        char next = buf[pos];
        if ((buf[end] == '\r' && next == '\n') ||
            (buf[end] == '\n' && next == '\r'))
            ++pos;
    }

    return true;
}

// -------------------------------------------------------------------
// pos.cfg binary-record parsing — OnionOS FFplay saved-position lookup.
//
// /mnt/SDCARD/.tmp_update/pos.cfg contains fixed-size 264-byte records:
//   bytes 0..255   — key/name/URL storage (NUL-terminated string)
//   bytes 256..259 — saved playback position, little-endian uint32 (seconds)
//   bytes 260..263 — another field (unused by this parser)
//
// This function scans the raw file data, finds the record whose first
// NUL-terminated field exactly matches `streamKey`, and returns the
// decoded position in whole seconds.
//
// Robustness:
//   - requires at least one complete 264-byte record
//   - iterates complete records safely; trailing partial bytes ignored
//   - iterates ALL records (does not assume ordering or count)
//   - rejects records with no NUL in the first 256 bytes
//   - compares only up to the first NUL (stale bytes after NUL ignored)
//   - decodes little-endian without assuming host endianness
//   - if multiple matching records exist, returns the LAST one (newest slot)
//   - does NOT modify the file data
//
// Returns true on success and writes decoded seconds to `outSeconds`.
// Returns false on any failure (caller falls back to showinfo PTS).
// -------------------------------------------------------------------

static const size_t POS_CFG_RECORD_SIZE = 264;
static const size_t POS_CFG_KEY_SIZE    = 256;
static const size_t POS_CFG_POS_OFFSET  = 256;

inline bool parse_pos_cfg_position(const std::string &fileData,
                                   const char *streamKey,
                                   uint32_t &outSeconds)
{
    if (!streamKey || streamKey[0] == '\0') return false;

    const size_t dataSize = fileData.size();
    if (dataSize < POS_CFG_RECORD_SIZE) return false;

    // Iterate only complete 264-byte records; trailing partial bytes
    // beyond the last full record are safely ignored.
    const size_t recordCount = dataSize / POS_CFG_RECORD_SIZE;
    const unsigned char *data =
        reinterpret_cast<const unsigned char *>(fileData.data());

    bool found = false;
    uint32_t bestSeconds = 0;

    const size_t keyLen = std::strlen(streamKey);

    for (size_t i = 0; i < recordCount; ++i) {
        const size_t base = i * POS_CFG_RECORD_SIZE;

        // Scan first 256 bytes for a NUL terminator
        size_t nulPos = 0;
        bool hasNul = false;
        for (size_t j = 0; j < POS_CFG_KEY_SIZE; ++j) {
            if (data[base + j] == 0) {
                nulPos = j;
                hasNul = true;
                break;
            }
        }

        // Reject record if no NUL found in the key field
        if (!hasNul) continue;

        // Reject if NUL-terminated length doesn't match key length
        if (nulPos != keyLen) continue;

        // Compare the NUL-terminated value against the stream key
        if (std::memcmp(data + base, streamKey, keyLen) != 0) continue;

        // Decode bytes +256..+259 as little-endian uint32
        const uint32_t pos =
            static_cast<uint32_t>(data[base + POS_CFG_POS_OFFSET + 0])
          | (static_cast<uint32_t>(data[base + POS_CFG_POS_OFFSET + 1]) << 8)
          | (static_cast<uint32_t>(data[base + POS_CFG_POS_OFFSET + 2]) << 16)
          | (static_cast<uint32_t>(data[base + POS_CFG_POS_OFFSET + 3]) << 24);

        // Prefer the last matching record in file order
        bestSeconds = pos;
        found = true;
    }

    if (found) outSeconds = bestSeconds;
    return found;
}

#endif // PLAYBACK_CLOCK_PARSER_HPP
