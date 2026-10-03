#ifndef MIYOOFIN_HLS_PLAYLIST_HPP
#define MIYOOFIN_HLS_PLAYLIST_HPP
#include <string>
#include <vector>
namespace miyoofin {
class HlsPlaylist
{
  public:
    static std::string resolve(const std::string& playlistUrl, const std::string& entry);
    static std::vector<std::string> variants(const std::string& body, const std::string& url);
    /// True when every URL has exactly the origin (scheme, host, port) of `baseUrl`. Playlist
    /// entries are data from the network: the download token goes only to the server it was for.
    static bool allSameOrigin(const std::vector<std::string>& urls, const std::string& baseUrl);
    static std::vector<std::string> segments(const std::string& body, const std::string& url);
};
}
#endif
