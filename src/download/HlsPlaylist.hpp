#ifndef MIYOOFIN_HLS_PLAYLIST_HPP
#define MIYOOFIN_HLS_PLAYLIST_HPP
#include <string>
#include <vector>
namespace miyoofin {
class HlsPlaylist {
public:
    static std::string resolve(const std::string &playlistUrl, const std::string &entry);
    static std::vector<std::string> variants(const std::string &body, const std::string &url);
    static std::vector<std::string> segments(const std::string &body, const std::string &url);
};
}
#endif
