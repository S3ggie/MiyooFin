#include "UpdateManifest.hpp"
#include <cctype>
#include <algorithm>

namespace miyoofin {

// -------------------------------------------------------------------
// Minimal hand-rolled JSON helpers — same approach as JellyfinApiJson
// but self-contained in the update module.
// -------------------------------------------------------------------

/// Skip whitespace and colon separators, return position or npos.
static size_t skipToValue(const std::string& json, size_t pos)
{
    while (pos < json.size()) {
        char c = json[pos];
        if (c == ':' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pos++;
        } else {
            break;
        }
    }
    return pos;
}

/// Extract a JSON string value at the current position (caller must have
/// positioned *pos* at the opening quote).  Returns the decoded string
/// and advances *pos* past the closing quote.
static std::string extractQuoted(const std::string& json, size_t& pos)
{
    if (pos >= json.size() || json[pos] != '"')
        return {};
    pos++;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            switch (json[pos]) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                // Minimal \uXXXX — decode BMP, skip surrogate pairs
                if (pos + 4 < json.size()) {
                    auto hx = [&](unsigned idx) -> int {
                        char c = json[pos + 1 + idx];
                        if (c >= '0' && c <= '9')
                            return c - '0';
                        if (c >= 'a' && c <= 'f')
                            return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F')
                            return c - 'A' + 10;
                        return -1;
                    };
                    int h0 = hx(0), h1 = hx(1), h2 = hx(2), h3 = hx(3);
                    if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                        uint32_t cp = (uint32_t(h0) << 12) | (uint32_t(h1) << 8) |
                                      (uint32_t(h2) << 4) | uint32_t(h3);
                        if (cp < 0x80)
                            out += char(cp);
                        else if (cp < 0x800) {
                            out += char(0xC0 | (cp >> 6));
                            out += char(0x80 | (cp & 0x3F));
                        } else {
                            out += char(0xE0 | (cp >> 12));
                            out += char(0x80 | ((cp >> 6) & 0x3F));
                            out += char(0x80 | (cp & 0x3F));
                        }
                        pos += 4;
                    }
                }
                break;
            }
            default:
                out += json[pos];
                break;
            }
        } else {
            out += json[pos];
        }
        pos++;
    }
    if (pos < json.size())
        pos++; // skip closing quote
    return out;
}

/// Extract a string value for a top-level key.  Searches the entire JSON
/// for `"key"` and returns the string value after it.  Because the
/// manifest is flat at the top level, this is safe.
static std::string topString(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos)
        return {};
    pos += needle.size();
    pos = skipToValue(json, pos);
    return extractQuoted(json, pos);
}

/// Extract a raw JSON value (string, object, array, or number) for a key
/// scoped within a parent substring.  Returns the raw text including
/// quotes (for strings) or braces/brackets (for objects/arrays).
/// Returns empty if not found.
static std::string scopedRaw(const std::string& scope, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    auto pos = scope.find(needle);
    if (pos == std::string::npos)
        return {};
    pos += needle.size();
    pos = skipToValue(scope, pos);
    if (pos >= scope.size())
        return {};

    char start = scope[pos];
    if (start == '"') {
        // String — find matching close
        size_t s = pos;
        pos++;
        while (pos < scope.size() && scope[pos] != '"') {
            if (scope[pos] == '\\')
                pos++;
            pos++;
        }
        if (pos < scope.size())
            pos++; // past closing quote
        return scope.substr(s, pos - s);
    } else if (start == '{' || start == '[') {
        char close = (start == '{') ? '}' : ']';
        int depth = 1;
        size_t s = pos;
        pos++;
        while (pos < scope.size() && depth > 0) {
            if (scope[pos] == '"') {
                pos++;
                while (pos < scope.size() && scope[pos] != '"') {
                    if (scope[pos] == '\\')
                        pos++;
                    pos++;
                }
            } else if (scope[pos] == start)
                depth++;
            else if (scope[pos] == close)
                depth--;
            pos++;
        }
        return scope.substr(s, pos - s);
    } else {
        // Number / bool / null — read to delimiter
        size_t s = pos;
        while (pos < scope.size() && scope[pos] != ',' && scope[pos] != '}' && scope[pos] != ']' &&
               scope[pos] != ' ' && scope[pos] != '\n' && scope[pos] != '\t' && scope[pos] != '\r')
            pos++;
        return scope.substr(s, pos - s);
    }
}

/// Extract the JSON object body for a nested key inside a parent object
/// found by scanning the full JSON.  E.g. extractNestedObject(json,
/// "assets", "tar_gz") returns the raw `{...}` string for assets.tar_gz.
static std::string nestedObject(const std::string& json, const std::string& parent,
                                const std::string& child)
{
    // Find parent key and extract its raw value (the object body)
    std::string parentNeedle = "\"" + parent + "\"";
    auto ppos = json.find(parentNeedle);
    if (ppos == std::string::npos)
        return {};
    ppos += parentNeedle.size();
    ppos = skipToValue(json, ppos);
    std::string parentVal = scopedRaw(json.substr(ppos), "\"dummy\"");
    // scopedRaw started at ppos; we need the raw value at ppos not under a key
    // Actually let me redo: find the object value for the parent key directly.
    parentVal.clear();
    if (ppos < json.size() && json[ppos] == '{') {
        int depth = 0;
        size_t s = ppos;
        bool inStr = false;
        for (size_t i = ppos; i < json.size(); i++) {
            if (inStr) {
                if (json[i] == '\\') {
                    i++;
                    continue;
                }
                if (json[i] == '"')
                    inStr = false;
            } else {
                if (json[i] == '"')
                    inStr = true;
                else if (json[i] == '{')
                    depth++;
                else if (json[i] == '}') {
                    depth--;
                    if (depth == 0) {
                        parentVal = json.substr(s, i + 1 - s);
                        break;
                    }
                }
            }
        }
    }
    if (parentVal.empty())
        return {};
    return scopedRaw(parentVal, child);
}

/// Convert a raw number string to uint64_t.  Returns 0 on failure.
static std::uint64_t toUint64(const std::string& s)
{
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            return 0;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

/// Validate a SHA-256 hex string and normalise to lowercase.
static bool normalizeSha256(const std::string& in, std::string& out)
{
    if (in.size() != 64)
        return false;
    out.clear();
    out.reserve(64);
    for (char c : in) {
        if (c >= '0' && c <= '9')
            out += c;
        else if (c >= 'a' && c <= 'f')
            out += c;
        else if (c >= 'A' && c <= 'F')
            out += static_cast<char>(c + 32);
        else
            return false;
    }
    return true;
}

// -------------------------------------------------------------------
bool parseUpdateManifest(const std::string& json, UpdateManifest& out, bool allowNonHttpsAssets)
{
    out = UpdateManifest{};

    // Top-level string fields
    out.name = topString(json, "name");
    out.version = topString(json, "version");
    out.tag = topString(json, "tag");
    out.minVersion = topString(json, "min_version");
    out.notes = topString(json, "notes");

    // Required: version must be non-empty
    if (out.version.empty())
        return false;

    // --- assets.tar_gz (required) ---
    std::string tarRaw = nestedObject(json, "assets", "tar_gz");
    if (tarRaw.empty())
        return false;

    // Extract url, sha256, size from tarRaw
    std::string tarUrl = scopedRaw(tarRaw, "url");
    // Strip surrounding quotes if present
    if (tarUrl.size() >= 2 && tarUrl.front() == '"' && tarUrl.back() == '"')
        tarUrl = tarUrl.substr(1, tarUrl.size() - 2);

    std::string tarSha = scopedRaw(tarRaw, "sha256");
    if (tarSha.size() >= 2 && tarSha.front() == '"' && tarSha.back() == '"')
        tarSha = tarSha.substr(1, tarSha.size() - 2);

    std::string tarSize = scopedRaw(tarRaw, "size");

    if (tarUrl.empty())
        return false;
    if (tarSha.empty())
        return false;
    if (tarSize.empty())
        return false;

    // Defense-in-depth: reject non-HTTPS URLs and non-GitHub hosts
    // unless dev override is active (allowNonHttpsAssets).
    if (!allowNonHttpsAssets) {
        if (tarUrl.compare(0, 8, "https://") != 0)
            return false;
        {
            // Must be hosted on github.com or objects.githubusercontent.com
            bool okHost = false;
            static const char* trusted[] = {"github.com/", "objects.githubusercontent.com/"};
            for (auto* h : trusted) {
                if (tarUrl.find(h) != std::string::npos) {
                    okHost = true;
                    break;
                }
            }
            if (!okHost)
                return false;
        }
    }

    std::string shaNorm;
    if (!normalizeSha256(tarSha, shaNorm))
        return false;

    out.tarGz.url = tarUrl;
    out.tarGz.sha256 = shaNorm;
    out.tarGz.size = toUint64(tarSize);

    // --- assets.zip (optional) ---
    std::string zipRaw = nestedObject(json, "assets", "zip");
    if (!zipRaw.empty()) {
        std::string zipUrl = scopedRaw(zipRaw, "url");
        if (zipUrl.size() >= 2 && zipUrl.front() == '"' && zipUrl.back() == '"')
            zipUrl = zipUrl.substr(1, zipUrl.size() - 2);

        std::string zipSha = scopedRaw(zipRaw, "sha256");
        if (zipSha.size() >= 2 && zipSha.front() == '"' && zipSha.back() == '"')
            zipSha = zipSha.substr(1, zipSha.size() - 2);

        std::string zipSize = scopedRaw(zipRaw, "size");

        if (!zipUrl.empty() && !zipSha.empty()) {
            std::string zipShaNorm;
            if (normalizeSha256(zipSha, zipShaNorm)) {
                out.zip.url = zipUrl;
                out.zip.sha256 = zipShaNorm;
                out.zip.size = zipSize.empty() ? 0 : toUint64(zipSize);
            }
        }
    }

    return true;
}

} // namespace miyoofin
