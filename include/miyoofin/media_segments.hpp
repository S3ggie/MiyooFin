#ifndef MIYOOFIN_MEDIA_SEGMENTS_HPP
#define MIYOOFIN_MEDIA_SEGMENTS_HPP

#include "playback_tracks.hpp"
#include <cstdlib>
#include <string>
#include <vector>

// Jellyfin media segments (GET /MediaSegments/{id}, server 10.10+): the intro and
// credits ranges the player offers to skip. Written to playback-segments.txt as
// "intro <start> <end>" / "outro <start> <end>" lines, in seconds.
struct MediaSegment
{
    std::string kind; // "intro" or "outro"
    double start = 0;
    double end = 0;
};

inline std::vector<MediaSegment> media_segments_parse(const std::string& body)
{
    using namespace playback_tracks_detail;
    std::vector<MediaSegment> out;
    const std::string items = topLevelValue(body, "Items");
    std::size_t pos = items.find('{');
    while (pos != std::string::npos && out.size() < 16) {
        const std::size_t end = skipValue(items, pos);
        const std::string obj = items.substr(pos, end - pos);
        const std::string type = stringValue(obj, "Type");
        MediaSegment s;
        s.kind = type == "Intro" ? "intro" : type == "Outro" ? "outro" : "";
        s.start = std::atof(topLevelValue(obj, "StartTicks").c_str()) / 1e7;
        s.end = std::atof(topLevelValue(obj, "EndTicks").c_str()) / 1e7;
        if (!s.kind.empty() && s.start >= 0 && s.end > s.start + 3)
            out.push_back(s);
        pos = items.find('{', end);
    }
    return out;
}

inline std::string media_segments_format(const std::vector<MediaSegment>& segments)
{
    std::string text;
    for (const MediaSegment& s : segments)
        text += s.kind + " " + std::to_string(s.start) + " " + std::to_string(s.end) + "\n";
    return text;
}

#endif
