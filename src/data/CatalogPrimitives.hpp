#ifndef MIYOOFIN_CATALOG_PRIMITIVES_HPP
#define MIYOOFIN_CATALOG_PRIMITIVES_HPP

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace miyoofin::catalog {

inline std::string normalizeIdentityUrl(const std::string& input)
{
    std::string url = input;
    std::size_t start = 0;
    while (start < url.size() && std::isspace(static_cast<unsigned char>(url[start]))) {
        ++start;
    }
    if (start > 0)
        url = url.substr(start);
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) {
        url.pop_back();
    }
    if (url.find("://") == std::string::npos)
        url = "http://" + url;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url;
}

inline std::string scopeKey(const std::string& url, const std::string& user)
{
    const std::string value = normalizeIdentityUrl(url) + "\n" + user;
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    char output[17];
    std::snprintf(output, sizeof(output), "%016llx", static_cast<unsigned long long>(hash));
    return output;
}

inline std::string cachePath(const std::string& root, const std::string& scope)
{
    return root + "/library/" + scope + "/snapshot.v1";
}

inline std::string catalogPath(const std::string& root, const std::string& scope)
{
    return root + "/library/" + scope + "/catalog.sqlite3";
}

inline std::string migratingCatalogPath(const std::string& root, const std::string& scope)
{
    return catalogPath(root, scope) + ".migrating";
}

inline std::string_view organizationalTitle(const std::string& title)
{
    if (title.size() > 4 && (title[0] == 'T' || title[0] == 't') &&
        (title[1] == 'H' || title[1] == 'h') && (title[2] == 'E' || title[2] == 'e') &&
        title[3] == ' ') {
        return std::string_view(title).substr(4);
    }
    return title;
}

inline std::string organizationalSortKey(const std::string& title)
{
    const std::string_view source = organizationalTitle(title);
    std::string key;
    key.reserve(source.size());
    for (unsigned char character : source) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<unsigned char>(character + ('a' - 'A'));
        }
        key.push_back(static_cast<char>(character));
    }
    return key;
}

inline int alphabetIndex(const std::string& title)
{
    const std::string_view value = organizationalTitle(title);
    if (value.empty())
        return -1;
    const unsigned char character = value[0];
    if (character >= 'A' && character <= 'Z')
        return character - 'A';
    if (character >= 'a' && character <= 'z')
        return character - 'a';
    return -1;
}

inline int asciiCaseInsensitiveCompare(std::string_view left, std::string_view right)
{
    const std::size_t count = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t index = 0; index < count; ++index) {
        unsigned char leftCharacter = left[index];
        unsigned char rightCharacter = right[index];
        if (leftCharacter >= 'A' && leftCharacter <= 'Z')
            leftCharacter = static_cast<unsigned char>(leftCharacter + 32);
        if (rightCharacter >= 'A' && rightCharacter <= 'Z')
            rightCharacter = static_cast<unsigned char>(rightCharacter + 32);
        if (leftCharacter != rightCharacter)
            return leftCharacter < rightCharacter ? -1 : 1;
    }
    if (left.size() == right.size())
        return 0;
    return left.size() < right.size() ? -1 : 1;
}
}
#endif
