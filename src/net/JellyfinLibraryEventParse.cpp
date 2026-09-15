#include "JellyfinLibraryEventParse.hpp"
#include "JellyfinLibraryEvents.hpp"
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace miyoofin {
namespace jellyfin_library_events_detail {

void skipWhitespace(const std::string &json, std::size_t &pos)
{
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'
           || json[pos] == '\r' || json[pos] == '\n')) ++pos;
}

bool parseString(const std::string &json, std::size_t &pos,
                 std::string &value)
{
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    value.clear();
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            if (static_cast<unsigned char>(c) < 0x20) return false;
            value += c;
            continue;
        }
        if (pos >= json.size()) return false;
        const char escaped = json[pos++];
        switch (escaped) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case 'u': {
            if (pos + 4 > json.size()) return false;
            // Jellyfin identifiers are ASCII. Preserve non-ASCII escapes as
            // UTF-8 only where they fit in one byte; reject malformed hex.
            unsigned value16 = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const char h = json[pos++];
                unsigned digit = 0;
                if (h >= '0' && h <= '9') digit = static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') digit = static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') digit = static_cast<unsigned>(h - 'A' + 10);
                else return false;
                value16 = (value16 << 4) | digit;
            }
            if (value16 > 0x7f) return false;
            value += static_cast<char>(value16);
            break;
        }
        default: return false;
        }
    }
    return false;
}

bool findKey(const std::string &json, const std::string &key,
             std::size_t &valuePos)
{
    std::size_t pos = 0;
    while (pos < json.size()) {
        skipWhitespace(json, pos);
        if (pos >= json.size()) break;
        if (json[pos] != '"') {
            ++pos;
            continue;
        }
        std::string candidate;
        const std::size_t keyStart = pos;
        if (!parseString(json, pos, candidate)) return false;
        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != ':') {
            // A quoted value elsewhere in the document is not a key.
            pos = keyStart + 1;
            continue;
        }
        ++pos;
        if (candidate == key) {
            skipWhitespace(json, pos);
            valuePos = pos;
            return true;
        }
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '"') {
            std::string ignored;
            if (!parseString(json, pos, ignored)) return false;
        } else {
            ++pos;
        }
    }
    return false;
}

bool rawValue(const std::string &json, std::size_t pos,
              std::string &value)
{
    if (pos >= json.size()) return false;
    const std::size_t start = pos;
    if (json[pos] == '"') {
        std::string ignored;
        if (!parseString(json, pos, ignored)) return false;
        value = json.substr(start, pos - start);
        return true;
    }
    if (json[pos] != '{' && json[pos] != '[') return false;
    const char open = json[pos];
    const char close = open == '{' ? '}' : ']';
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == open) ++depth;
        else if (c == close && --depth == 0) {
            ++pos;
            value = json.substr(start, pos - start);
            return true;
        }
    }
    return false;
}

bool arrayStrings(const std::string &json,
                  std::vector<std::string> &values)
{
    values.clear();
    std::size_t pos = 0;
    skipWhitespace(json, pos);
    if (pos >= json.size() || json[pos++] != '[') return false;
    skipWhitespace(json, pos);
    if (pos < json.size() && json[pos] == ']') return true;
    while (pos < json.size()) {
        std::string value;
        if (!parseString(json, pos, value) || value.empty()) return false;
        values.push_back(std::move(value));
        if (values.size() > kMaxQueuedIds) return false;
        skipWhitespace(json, pos);
        if (pos >= json.size()) return false;
        if (json[pos] == ']') return ++pos == json.size();
        if (json[pos++] != ',') return false;
        skipWhitespace(json, pos);
    }
    return false;
}

bool optionalArray(const std::string &json, const std::string &key,
                   std::vector<std::string> &values)
{
    std::size_t pos = 0;
    if (!findKey(json, key, pos)) {
        values.clear();
        return true;
    }
    std::string raw;
    return rawValue(json, pos, raw) && arrayStrings(raw, values);
}

std::string urlEscape(const std::string &value)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_'
            || c == '.' || c == '~') result += static_cast<char>(c);
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

} // namespace jellyfin_library_events_detail

JellyfinLibraryEventParse JellyfinLibraryEvents::parseMessage(
    const std::string &message, JellyfinLibraryChangeBatch &batch)
{
    using namespace jellyfin_library_events_detail;
    batch = {};
    if (message.size() > kMaxMessageBytes)
        return JellyfinLibraryEventParse::Oversized;

    std::size_t typePos = 0;
    std::string rawType;
    if (!findKey(message, "MessageType", typePos)
        || !rawValue(message, typePos, rawType))
        return JellyfinLibraryEventParse::Malformed;
    std::size_t stringPos = 0;
    std::string messageType;
    if (rawType.empty() || rawType[0] != '"'
        || !parseString(rawType, stringPos, messageType))
        return JellyfinLibraryEventParse::Malformed;
    if (messageType == "UserDataChanged") {
        batch.userDataChanged = true;
        return JellyfinLibraryEventParse::Parsed;
    }
    if (messageType != "LibraryChanged")
        return JellyfinLibraryEventParse::Ignored;

    std::size_t dataPos = 0;
    std::string rawData;
    if (!findKey(message, "Data", dataPos)
        || !rawValue(message, dataPos, rawData))
        return JellyfinLibraryEventParse::Malformed;
    std::string data = rawData;
    if (!rawData.empty() && rawData[0] == '"') {
        std::size_t dataStringPos = 0;
        if (!parseString(rawData, dataStringPos, data))
            return JellyfinLibraryEventParse::Malformed;
    }
    std::string rawDataObject;
    if (data.empty() || data[0] != '{'
        || !rawValue(data, 0, rawDataObject))
        return JellyfinLibraryEventParse::Malformed;
    data = std::move(rawDataObject);

    if (!optionalArray(data, "ItemsAdded", batch.itemsAdded)
        || !optionalArray(data, "ItemsRemoved", batch.itemsRemoved)
        || !optionalArray(data, "ItemsUpdated", batch.itemsUpdated))
        return JellyfinLibraryEventParse::Malformed;
    return JellyfinLibraryEventParse::Parsed;
}

} // namespace miyoofin
