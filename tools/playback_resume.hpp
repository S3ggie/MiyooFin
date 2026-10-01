// Pure helpers for refreshing the resume position before remote playback.
//
// The screens write playback-request.txt from their CACHED item, which goes
// stale when the item is watched on another device. The reporter asks the
// server for the current position just before playback and rewrites
// resume_ticks, so playback resumes (and later reports) from the right place.
// Header-only and free of I/O so it can be unit tested without a network.
#ifndef MIYOOFIN_PLAYBACK_RESUME_HPP
#define MIYOOFIN_PLAYBACK_RESUME_HPP

#include <cstdint>
#include <cstdlib>
#include <string>

// Extracts UserData.PlaybackPositionTicks from a Jellyfin item JSON body.
// Returns false when the body has no UserData object or the value is not a
// non-negative integer. A UserData object with no (or a null) position means
// "not started": ticks = 0 and the function returns true.
inline bool playback_parse_user_position_ticks(const std::string& body, std::int64_t& ticks)
{
    ticks = 0;
    const std::string userKey = "\"UserData\"";
    const std::size_t user = body.find(userKey);
    if (user == std::string::npos)
        return false;
    const std::size_t objectStart = body.find('{', user + userKey.size());
    if (objectStart == std::string::npos)
        return false;
    // Bound the search to the UserData object (it contains no nested objects).
    const std::size_t objectEnd = body.find('}', objectStart);
    if (objectEnd == std::string::npos)
        return false;
    const std::string userData = body.substr(objectStart, objectEnd - objectStart);
    const std::string key = "\"PlaybackPositionTicks\"";
    const std::size_t at = userData.find(key);
    if (at == std::string::npos)
        return true;
    std::size_t pos = userData.find(':', at + key.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < userData.size() && (userData[pos] == ' ' || userData[pos] == '\t'))
        ++pos;
    if (userData.compare(pos, 4, "null") == 0)
        return true;
    std::int64_t value = 0;
    std::size_t digits = 0;
    while (pos < userData.size() && userData[pos] >= '0' && userData[pos] <= '9') {
        const int digit = userData[pos] - '0';
        if (value > (INT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
        ++pos;
        ++digits;
    }
    if (digits == 0)
        return false;
    ticks = value;
    return true;
}

// Returns `content` with the resume_ticks line set to `ticks`, keeping every
// other line as is. Appends the line when it is missing.
inline std::string playback_replace_resume_ticks(const std::string& content, std::int64_t ticks)
{
    const std::string key = "resume_ticks=";
    const std::string line = key + std::to_string(ticks);
    std::string out;
    bool replaced = false;
    std::size_t pos = 0;
    while (pos < content.size()) {
        std::size_t end = content.find('\n', pos);
        const bool hasNewline = end != std::string::npos;
        if (!hasNewline)
            end = content.size();
        const std::string current = content.substr(pos, end - pos);
        if (!replaced && current.compare(0, key.size(), key) == 0) {
            out += line;
            replaced = true;
        } else {
            out += current;
        }
        if (hasNewline)
            out += '\n';
        pos = hasNewline ? end + 1 : end;
    }
    if (!replaced) {
        if (!out.empty() && out.back() != '\n')
            out += '\n';
        out += line + "\n";
    }
    return out;
}

#endif // MIYOOFIN_PLAYBACK_RESUME_HPP
