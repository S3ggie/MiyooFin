// Turns an ASS/SSA subtitle file into a compact SRT of just the readable lines.
//
// Fansub releases routinely ship 20+ MB ASS tracks: tens of thousands of karaoke and
// typesetting events (effect lines, vector drawings, per-syllable frames) around a few
// hundred real dialogue lines. Jellyfin's own SRT conversion keeps all of them, which is
// far too much for the Miyoo. The filter keeps only events a viewer would read:
//   - no Effect field (karaoke/banner/scroll effects are skipped),
//   - no vector drawing ({\p1...}),
//   - non-empty text after override tags are removed,
//   - at least 0.15 s long.
// Line-by-line streaming, bounded memory, header-only so the reporter and the app share it.
#ifndef MIYOOFIN_SUBTITLE_TEXT_HPP
#define MIYOOFIN_SUBTITLE_TEXT_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace subtitle_text_detail {

inline std::vector<std::string> splitCommas(const std::string& line, std::size_t maxFields)
{
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (out.size() + 1 < maxFields) {
        const std::size_t comma = line.find(',', pos);
        if (comma == std::string::npos)
            break;
        out.push_back(line.substr(pos, comma - pos));
        pos = comma + 1;
    }
    out.push_back(line.substr(pos));
    return out;
}

inline std::string trim(const std::string& s)
{
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

// "h:mm:ss.cc" -> milliseconds, -1 on error.
inline long long parseTime(const std::string& t)
{
    int h = 0, m = 0;
    double s = 0;
    if (std::sscanf(t.c_str(), "%d:%d:%lf", &h, &m, &s) != 3)
        return -1;
    return static_cast<long long>(h) * 3600000LL + m * 60000LL +
           static_cast<long long>(s * 1000.0 + 0.5);
}

inline void writeTime(std::string& out, long long ms)
{
    char b[64];
    std::snprintf(b, sizeof(b), "%02lld:%02lld:%02lld,%03lld", ms / 3600000, (ms / 60000) % 60,
                  (ms / 1000) % 60, ms % 1000);
    out += b;
}

// Removes {...} override blocks; reports whether any block starts vector drawing mode.
inline std::string stripOverrides(const std::string& text, bool& drawing)
{
    std::string out;
    drawing = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            const std::size_t end = text.find('}', i);
            const std::size_t stop = end == std::string::npos ? text.size() : end;
            for (std::size_t k = i; k + 2 < stop; ++k)
                if (text[k] == '\\' && text[k + 1] == 'p' && text[k + 2] >= '1' &&
                    text[k + 2] <= '9')
                    drawing = true;
            i = stop;
        } else if (text[i] == '\\' && i + 1 < text.size() &&
                   (text[i + 1] == 'N' || text[i + 1] == 'n')) {
            out += '\n';
            ++i;
        } else if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'h') {
            out += ' ';
            ++i;
        } else {
            out += text[i];
        }
    }
    return out;
}

} // namespace subtitle_text_detail

// Streams `inPath` (ASS) to `outPath` (SRT). Returns the number of cues written, or -1 when
// the input cannot be read / the output cannot be written. `maxCues` bounds the output.
inline int subtitle_ass_file_to_srt(const std::string& inPath, const std::string& outPath,
                                    int maxCues = 30000)
{
    using namespace subtitle_text_detail;
    FILE* in = std::fopen(inPath.c_str(), "rb");
    if (!in)
        return -1;
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return -1;
    }
    // Default ASS v4+ column order; replaced by the file's own Format line.
    std::vector<std::string> cols = {"Layer",   "Start",   "End",     "Style",  "Name",
                                     "MarginL", "MarginR", "MarginV", "Effect", "Text"};
    int cues = 0;
    bool ok = true;
    std::string line;
    char buf[8192];
    bool tooLong = false;
    std::string lastKey;
    while (ok && std::fgets(buf, sizeof(buf), in)) {
        const std::size_t len = std::strlen(buf);
        const bool complete = len > 0 && buf[len - 1] == '\n';
        if (!complete && !std::feof(in)) { // over-long line: skip its remainder
            tooLong = true;
            continue;
        }
        if (tooLong) {
            tooLong = false;
            continue;
        }
        line.assign(buf, len);
        if (line.compare(0, 7, "Format:") == 0 && line.find("Text") != std::string::npos) {
            cols.clear();
            for (const std::string& c : splitCommas(line.substr(7), 64))
                cols.push_back(trim(c));
            continue;
        }
        if (line.compare(0, 9, "Dialogue:") != 0)
            continue;
        const std::vector<std::string> f = splitCommas(trim(line.substr(9)), cols.size());
        if (f.size() != cols.size())
            continue;
        std::string start, end, effect, text;
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (cols[i] == "Start")
                start = trim(f[i]);
            else if (cols[i] == "End")
                end = trim(f[i]);
            else if (cols[i] == "Effect")
                effect = trim(f[i]);
            else if (cols[i] == "Text")
                text = f[i];
        }
        if (!effect.empty())
            continue;
        bool drawing = false;
        std::string clean = trim(stripOverrides(text, drawing));
        const long long a = parseTime(start), b = parseTime(end);
        if (drawing || clean.empty() || a < 0 || b - a < 150)
            continue;
        const std::string key = start + "|" + clean;
        if (key == lastKey) // exact repeats (layered duplicates) add nothing
            continue;
        lastKey = key;
        std::string cue = std::to_string(++cues) + "\n";
        writeTime(cue, a);
        cue += " --> ";
        writeTime(cue, b);
        cue += "\n" + clean + "\n\n";
        ok = std::fwrite(cue.data(), 1, cue.size(), out) == cue.size();
        if (cues >= maxCues)
            break;
    }
    std::fclose(in);
    ok = (std::fclose(out) == 0) && ok;
    return ok ? cues : -1;
}

#endif // MIYOOFIN_SUBTITLE_TEXT_HPP
