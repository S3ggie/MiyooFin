// Pure helpers for the player's subtitle/audio track list.
//
// The reporter asks the server for the item's MediaStreams and writes
// playback-tracks.txt (no secrets) for the player; subtitle text for each
// selectable text track is saved next to it as subs/<index>.srt. Header-only
// and free of I/O so it can be unit tested without a network.
#ifndef MIYOOFIN_PLAYBACK_TRACKS_HPP
#define MIYOOFIN_PLAYBACK_TRACKS_HPP

#include <cstddef>
#include <cstdlib>
#include <string>
#include <vector>

struct PlaybackTrack
{
    int index = -1; // Jellyfin's global stream index (used verbatim in URLs)
    char type = 0;  // 'a' audio, 's' subtitle
    std::string codec;
    std::string lang;  // ISO code, may be empty
    std::string title; // display title, may be empty
    bool isDefault = false;
    bool forced = false;
    bool text = false; // subtitle only: fetchable as SRT (not PGS/DVD bitmaps)
};

struct PlaybackTracks
{
    std::string mediaSourceId;
    std::vector<PlaybackTrack> tracks;
};

namespace playback_tracks_detail {

// Index just past the value that starts at body[pos] (string, object, array or
// scalar), honouring string escapes and nesting.
inline std::size_t skipValue(const std::string& body, std::size_t pos)
{
    if (pos >= body.size())
        return pos;
    const char open = body[pos];
    if (open == '"') {
        for (++pos; pos < body.size(); ++pos) {
            if (body[pos] == '\\')
                ++pos;
            else if (body[pos] == '"')
                return pos + 1;
        }
        return body.size();
    }
    if (open == '{' || open == '[') {
        int depth = 0;
        for (; pos < body.size(); ++pos) {
            const char c = body[pos];
            if (c == '"') {
                pos = skipValue(body, pos) - 1;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0)
                    return pos + 1;
            }
        }
        return body.size();
    }
    while (pos < body.size() && body[pos] != ',' && body[pos] != '}' && body[pos] != ']')
        ++pos;
    return pos;
}

inline std::string unescape(const std::string& raw)
{
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            const char n = raw[++i];
            out += n == 'n' || n == 't' ? ' ' : n;
        } else {
            out += raw[i];
        }
    }
    return out;
}

// Value of a TOP-LEVEL key of the object text `obj` ("{...}"), as raw JSON text
// (strings keep their quotes). Empty when absent.
inline std::string topLevelValue(const std::string& obj, const std::string& key)
{
    std::size_t pos = 1;
    while (pos < obj.size()) {
        while (pos < obj.size() && obj[pos] != '"' && obj[pos] != '}')
            ++pos;
        if (pos >= obj.size() || obj[pos] == '}')
            return {};
        const std::size_t keyEnd = skipValue(obj, pos);
        const std::string name = obj.substr(pos + 1, keyEnd - pos - 2);
        pos = keyEnd;
        while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == ':'))
            ++pos;
        const std::size_t valueEnd = skipValue(obj, pos);
        if (name == key)
            return obj.substr(pos, valueEnd - pos);
        pos = valueEnd;
    }
    return {};
}

inline std::string stringValue(const std::string& obj, const std::string& key)
{
    const std::string raw = topLevelValue(obj, key);
    if (raw.size() < 2 || raw.front() != '"')
        return {};
    return unescape(raw.substr(1, raw.size() - 2));
}

inline bool boolValue(const std::string& obj, const std::string& key)
{
    return topLevelValue(obj, key) == "true";
}

} // namespace playback_tracks_detail

// Parses an item JSON body (GET /Users/{u}/Items/{id}): the first media
// source's id and its audio/subtitle streams. False when there is no media source.
inline bool playback_parse_tracks(const std::string& body, PlaybackTracks& out)
{
    using namespace playback_tracks_detail;
    out = {};
    const std::size_t sources = body.find("\"MediaSources\"");
    if (sources == std::string::npos)
        return false;
    const std::size_t arrayStart = body.find('[', sources);
    if (arrayStart == std::string::npos)
        return false;
    const std::size_t objStart = body.find('{', arrayStart);
    if (objStart == std::string::npos)
        return false;
    const std::string source = body.substr(objStart, skipValue(body, objStart) - objStart);
    out.mediaSourceId = stringValue(source, "Id");
    const std::string streams = topLevelValue(source, "MediaStreams");
    std::size_t pos = streams.find('{');
    while (pos != std::string::npos && out.tracks.size() < 64) {
        const std::size_t end = skipValue(streams, pos);
        const std::string stream = streams.substr(pos, end - pos);
        const std::string type = stringValue(stream, "Type");
        if (type == "Audio" || type == "Subtitle") {
            PlaybackTrack track;
            track.type = type == "Audio" ? 'a' : 's';
            track.index = std::atoi(topLevelValue(stream, "Index").c_str());
            track.codec = stringValue(stream, "Codec");
            track.lang = stringValue(stream, "Language");
            track.title = stringValue(stream, "DisplayTitle");
            if (track.title.empty())
                track.title = stringValue(stream, "Title");
            track.isDefault = boolValue(stream, "IsDefault");
            track.forced = boolValue(stream, "IsForced");
            // Bitmap formats cannot be converted to text.
            track.text = track.type == 's' && track.codec != "PGSSUB" && track.codec != "DVDSUB" &&
                         track.codec != "dvd_subtitle" && track.codec != "hdmv_pgs_subtitle" &&
                         track.codec != "DVBSUB" && track.codec != "dvb_subtitle" &&
                         track.codec != "XSUB";
            out.tracks.push_back(std::move(track));
        }
        pos = streams.find('{', end);
    }
    return !out.mediaSourceId.empty();
}

// One line per track: type|index|text|default|forced|lang|title. Fields never
// contain '|' or newlines.
inline std::string playback_format_tracks(const PlaybackTracks& tracks)
{
    auto clean = [](std::string s) {
        for (char& c : s)
            if (c == '|' || c == '\n' || c == '\r')
                c = ' ';
        return s;
    };
    std::string out;
    for (const PlaybackTrack& t : tracks.tracks) {
        out += t.type;
        out += '|' + std::to_string(t.index) + '|' + (t.text ? '1' : '0') + '|' +
               (t.isDefault ? '1' : '0') + '|' + (t.forced ? '1' : '0') + '|' + clean(t.lang) +
               '|' + clean(t.title) + '\n';
    }
    return out;
}

// Subtitle tracks worth fetching, best first: default/forced, then source order;
// at most `limit`.
inline std::vector<PlaybackTrack> playback_subtitle_fetch_order(const PlaybackTracks& tracks,
                                                                std::size_t limit)
{
    std::vector<PlaybackTrack> first, rest;
    for (const PlaybackTrack& t : tracks.tracks) {
        if (t.type != 's' || !t.text)
            continue;
        (t.isDefault || t.forced ? first : rest).push_back(t);
    }
    first.insert(first.end(), rest.begin(), rest.end());
    if (first.size() > limit)
        first.resize(limit);
    return first;
}

#endif // MIYOOFIN_PLAYBACK_TRACKS_HPP
