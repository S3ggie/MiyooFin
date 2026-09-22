#ifndef MIYOOFIN_JELLYFIN_LIBRARY_EVENT_PARSE_HPP
#define MIYOOFIN_JELLYFIN_LIBRARY_EVENT_PARSE_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace miyoofin {
namespace jellyfin_library_events_detail {

constexpr std::size_t kMaxMessageBytes = 32 * 1024;
constexpr std::size_t kMaxQueuedIds = 512;

void skipWhitespace(const std::string& json, std::size_t& pos);
bool parseString(const std::string& json, std::size_t& pos, std::string& value);
bool findKey(const std::string& json, const std::string& key, std::size_t& valuePos);
bool rawValue(const std::string& json, std::size_t pos, std::string& value);
bool arrayStrings(const std::string& json, std::vector<std::string>& values);
bool optionalArray(const std::string& json, const std::string& key,
                   std::vector<std::string>& values);
std::string urlEscape(const std::string& value);

} // namespace jellyfin_library_events_detail
} // namespace miyoofin

#endif // MIYOOFIN_JELLYFIN_LIBRARY_EVENT_PARSE_HPP
