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
#include <string>

// -------------------------------------------------------------------
// Seconds → Jellyfin PositionTicks conversion.
// Jellyfin ticks are 100 ns units: 1 second = 10,000,000 ticks.
// Negative or non-finite inputs clamp to 0.
// -------------------------------------------------------------------
inline int64_t seconds_to_ticks(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0) return 0;
    return static_cast<int64_t>(seconds * 10000000.0 + 0.5);
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

#endif // PLAYBACK_CLOCK_PARSER_HPP