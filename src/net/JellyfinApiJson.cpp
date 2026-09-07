#include "JellyfinApi.hpp"
#include <cstdint>

namespace miyoofin {

// -------------------------------------------------------------------
static int decodeUnicodeEscape(const std::string &s, size_t pos,
                               std::string &out)
{
    // pos points at 'u'
    if (pos + 4 >= s.size()) return 1; // malformed – skip
    auto hex = [&](unsigned idx) -> int {
        char c = s[pos + 1 + idx];
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int h0 = hex(0), h1 = hex(1), h2 = hex(2), h3 = hex(3);
    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return 1; // bad hex
    uint32_t cp = (uint32_t(h0) << 12) | (uint32_t(h1) << 8)
                | (uint32_t(h2) << 4)  |  uint32_t(h3);
    int consumed = 5; // \uXXXX → 5 chars: \, u, x, x, x

    // UTF-16 surrogate pair?
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        // look for \uXXXX (low surrogate) immediately after
        if (pos + 10 < s.size() && s[pos + 5] == '\\' && s[pos + 6] == 'u') {
            auto lhex = [&](unsigned idx) -> int {
                char c = s[pos + 7 + idx];
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int lh0 = lhex(0), lh1 = lhex(1), lh2 = lhex(2), lh3 = lhex(3);
            if (lh0 >= 0 && lh1 >= 0 && lh2 >= 0 && lh3 >= 0) {
                uint32_t lo = (uint32_t(lh0)<<12)|(uint32_t(lh1)<<8)
                            |(uint32_t(lh2)<<4)|uint32_t(lh3);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    consumed = 11; // \uHHHH\uLLLL
                }
            }
        }
        // if no valid low surrogate, emit the replacement char for the
        // lone high surrogate
        if (consumed == 5) cp = 0xFFFD;
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        cp = 0xFFFD; // lone low surrogate
    }

    // Encode cp as UTF-8
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
    return consumed;
}

// -------------------------------------------------------------------
// Helper — decode JSON escape sequences in a string body.
// The input is the content between quotes (quotes already stripped).
// Handles \n, \r, \t, \\, \", \/, \b, \f, and \uXXXX (including
// surrogate pairs via decodeUnicodeEscape).
// -------------------------------------------------------------------
static std::string decodeJsonStringBody(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    size_t pos = 0;
    while (pos < s.size()) {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            pos++;
            switch (s[pos]) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': pos += decodeUnicodeEscape(s, pos, out) - 1; break;
                default: out += s[pos]; break;
            }
        } else {
            out += s[pos];
        }
        pos++;
    }
    return out;
}

// -------------------------------------------------------------------
// Helper — extract a top-level JSON string value.
// -------------------------------------------------------------------
std::string JellyfinApi::extractString(const std::string &json, const std::string &key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t'))
        pos++;
    if (pos >= json.size() || json[pos] != '\"') return {};
    pos++;
    std::string val;
    while (pos < json.size() && json[pos] != '\"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            if (json[pos] == 'n') val += '\n';
            else if (json[pos] == 'r') val += '\r';
            else if (json[pos] == 't') val += '\t';
            else if (json[pos] == '\\') val += '\\';
            else if (json[pos] == '\"') val += '\"';
            else if (json[pos] == 'u' && pos + 4 < json.size())
                pos += decodeUnicodeEscape(json, pos, val) - 1;
            else val += json[pos];
        } else {
            val += json[pos];
        }
        pos++;
    }
    return val;
}

// -------------------------------------------------------------------
// Helper — extract a nested JSON string value.
// -------------------------------------------------------------------
std::string JellyfinApi::extractNestedString(const std::string &json,
                                        const std::string &parent,
                                        const std::string &child)
{
    std::string parentSearch = "\"" + parent + "\"";
    auto parentPos = json.find(parentSearch);
    if (parentPos == std::string::npos) return {};

    auto afterParent = parentPos + parentSearch.size();
    auto childPos = json.find("\"" + child + "\"", afterParent);
    if (childPos == std::string::npos) return {};

    auto afterChild = childPos + child.size() + 2;
    while (afterChild < json.size() &&
           (json[afterChild] == ':' || json[afterChild] == ' ' || json[afterChild] == '\t'))
        afterChild++;
    if (afterChild >= json.size() || json[afterChild] != '\"') return {};
    afterChild++;
    std::string val;
    while (afterChild < json.size() && json[afterChild] != '\"') {
        if (json[afterChild] == '\\' && afterChild + 1 < json.size()) {
            afterChild++;
            if (json[afterChild] == 'n') val += '\n';
            else if (json[afterChild] == 'r') val += '\r';
            else if (json[afterChild] == 't') val += '\t';
            else if (json[afterChild] == '\\') val += '\\';
            else if (json[afterChild] == '\"') val += '\"';
            else if (json[afterChild] == 'u' && afterChild + 4 < json.size())
                afterChild += decodeUnicodeEscape(json, afterChild, val) - 1;
            else val += json[afterChild];
        } else {
            val += json[afterChild];
        }
        afterChild++;
    }
    return val;
}
// -------------------------------------------------------------------
// Helper — JSON escape for string values.
// -------------------------------------------------------------------
std::string JellyfinApi::jsonEscape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}


std::string JellyfinApi::jsonRawValue(const std::string &json,
                                      const std::string &key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' '
           || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        pos++;
    if (pos >= json.size()) return {};
    if (json[pos] == '"') {
        pos++;
        size_t start = pos;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == 0x5c) pos++; // skip escaped char
            pos++;
        }
        return decodeJsonStringBody(json.substr(start, pos - start));
    } else if (json[pos] == '{' || json[pos] == '[') {
        char open = json[pos], close = (open == '{') ? '}' : ']';
        int depth = 1; size_t start = pos; pos++;
        while (pos < json.size() && depth > 0) {
            if (json[pos] == '"') { pos++;
                while (pos < json.size() && json[pos] != '"') {
                    if (json[pos] == '\\') pos++;
                    pos++; }
            } else if (json[pos] == open) depth++;
            else if (json[pos] == close) depth--;
            pos++;
        }
        return json.substr(start, pos - start);
    } else {
        size_t start = pos;
        while (pos < json.size() && json[pos] != ',' && json[pos] != '}'
               && json[pos] != ']' && json[pos] != ' '
               && json[pos] != '\n' && json[pos] != '\r'
               && json[pos] != '\t') pos++;
        return json.substr(start, pos - start);
    }
}

std::string JellyfinApi::jsonStringField(const std::string &obj,
                                         const std::string &key)
{ return jsonRawValue(obj, key); }

int JellyfinApi::jsonIntField(const std::string &obj, const std::string &key)
{
    std::string v = jsonRawValue(obj, key);
    if (v.empty() || v == "null") return 0;
    try { return std::stoi(v); } catch (...) { return 0; }
}

float JellyfinApi::jsonFloatField(const std::string &obj,
                                  const std::string &key)
{
    std::string v = jsonRawValue(obj, key);
    if (v.empty() || v == "null") return 0.0f;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    try { return std::stof(v); } catch (...) { return 0.0f; }
}

bool JellyfinApi::jsonBoolField(const std::string &obj,
                                const std::string &key)
{ return jsonRawValue(obj, key) == "true"; }

std::vector<std::string> JellyfinApi::splitJsonArrayContent(
    const std::string &content)
{
    std::vector<std::string> result;
    size_t es = 0; int bd = 0; bool ins = false;
    for (size_t i = 0; i <= content.size(); i++) {
        if (i < content.size()) {
            char c = content[i];
            if (ins) {
                if (c=='\\'&&i+1<content.size()) i++;
                else if(c=='"') ins=false;
            } else {
                if (c=='"') ins=true;
                else if (c=='{'||c=='[') bd++;
                else if (c=='}'||c==']') bd--;
                else if (c==','&&bd==0) {
                    std::string e = content.substr(es, i-es);
                    size_t a=0;
                    while(a<e.size()&&(e[a]==' '||e[a]=='\n'||e[a]=='\r'||e[a]=='\t'))a++;
                    size_t b=e.size();
                    while(b>a&&(e[b-1]==' '||e[b-1]=='\n'||e[b-1]=='\r'||e[b-1]=='\t'))b--;
                    if(a<b) result.push_back(e.substr(a,b-a));
                    es=i+1;
                }
            }
        } else {
            std::string e = content.substr(es, i-es);
            size_t a=0;
            while(a<e.size()&&(e[a]==' '||e[a]=='\n'||e[a]=='\r'||e[a]=='\t'))a++;
            size_t b=e.size();
            while(b>a&&(e[b-1]==' '||e[b-1]=='\n'||e[b-1]=='\r'||e[b-1]=='\t'))b--;
            if(a<b) result.push_back(e.substr(a,b-a));
        }
    }
    return result;
}

std::vector<std::string> JellyfinApi::jsonExtractArray(
    const std::string &json, const std::string &key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    while (pos < json.size() && (json[pos]==':'||json[pos]==' '
           ||json[pos]=='\t'||json[pos]=='\n'||json[pos]=='\r')) pos++;
    if (pos >= json.size() || json[pos] != '[') return {};
    pos++;
    size_t as = pos; int d = 1;
    while (pos < json.size() && d > 0) {
        if (json[pos] == '"') { pos++;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\') pos++;
                pos++; }
        } else if (json[pos] == '[') d++;
        else if (json[pos] == ']') { d--; if(d==0) break; }
        pos++;
    }
    if (d != 0) return {};
    return splitJsonArrayContent(json.substr(as, pos - as));
}

MediaItem JellyfinApi::jsonToMediaItem(const std::string &obj)
{
    MediaItem item;
    item.id       = jsonStringField(obj, "Id");
    item.title    = jsonStringField(obj, "Name");
    item.overview = jsonStringField(obj, "Overview");
    item.year     = jsonIntField(obj, "ProductionYear");
    item.rating   = jsonFloatField(obj, "CommunityRating");
    item.type     = jsonStringField(obj, "Type");
    item.etag     = jsonStringField(obj, "Etag");
    if (item.type == "Movie") item.type = "movie";
    else if (item.type == "Series") item.type = "show";
    else if (item.type == "Episode") item.type = "episode";
    else if (item.type == "Season") item.type = "season";

    item.indexNumber = jsonIntField(obj, "IndexNumber");

    // B5e2a: episode metadata
    item.parentIndexNumber = jsonIntField(obj, "ParentIndexNumber");
    {
        std::string rt = jsonRawValue(obj, "RunTimeTicks");
        if (!rt.empty() && rt != "null") {
            try { item.runTimeTicks = std::stoll(rt); } catch (...) { item.runTimeTicks = 0; }
        }
    }
    item.seriesName = jsonStringField(obj, "SeriesName");
    item.seriesId   = jsonStringField(obj, "SeriesId");
    item.seasonId   = jsonStringField(obj, "SeasonId");

    // Genres
    std::string gr = jsonRawValue(obj, "Genres");
    if (!gr.empty() && gr[0] == '[') {
        auto gs = jsonExtractArray(obj, "Genres");
        for (auto &g : gs) {
            if (g.size()>=2 && g.front()=='"' && g.back()=='"')
                g = g.substr(1, g.size()-2);
            g = decodeJsonStringBody(g);
            if (!g.empty()) item.genres.push_back(g);
        }
        if (!item.genres.empty()) item.genre = item.genres[0];
    }

    // UserData
    std::string ur = jsonRawValue(obj, "UserData");
    if (!ur.empty() && ur[0] == '{') {
        item.played   = jsonBoolField(ur, "Played");
        item.progress = jsonFloatField(ur, "PlayedPercentage") / 100.0f;
        if (item.progress < 0.0f) item.progress = 0.0f;
        if (item.progress > 1.0f) item.progress = 1.0f;
        std::string position = jsonRawValue(ur, "PlaybackPositionTicks");
        if (!position.empty() && position != "null") {
            try { item.playbackPositionTicks = std::stoll(position); }
            catch (...) { item.playbackPositionTicks = 0; }
        }
    }

    // ImageTags
    std::string ir = jsonRawValue(obj, "ImageTags");
    if (!ir.empty() && ir[0] == '{') {
        size_t p = 1;
        while (p < ir.size()) {
            while (p < ir.size() && ir[p] != '"') p++;
            if (p >= ir.size()) break;
            p++;
            size_t ks = p;
            while (p < ir.size() && ir[p] != '"') p++;
            std::string ik = ir.substr(ks, p-ks); p++;
            while (p < ir.size() && ir[p] != ':') p++;
            if (p >= ir.size()) break;
            p++;
            while (p < ir.size() && ir[p] == ' ') p++;
            if (p < ir.size() && ir[p] == '"') { p++; size_t vs = p;
                while (p < ir.size() && ir[p] != '"') p++;
                item.imageTags[ik] = ir.substr(vs, p-vs); p++;
            }
            while (p < ir.size() && ir[p] != ',' && ir[p] != '}') p++;
            if (p < ir.size() && ir[p] == ',') p++;
        }
    }

    // Placeholder art colour
    Uint8 r = (Uint8)((item.title.size()*37+80)&0xFF);
    Uint8 g = (Uint8)((item.title.size()*53+160)&0xFF);
    Uint8 b = (Uint8)((item.title.size()*71+240)&0xFF);
    item.artR = 80+r%120; item.artG = 80+g%120; item.artB = 80+b%120;
    return item;
}


} // namespace miyoofin
