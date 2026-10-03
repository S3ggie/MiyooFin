#include "MusicSettings.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace miyoofin {
namespace music {

void MusicSettings::load(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f)
        return;
    char line[96];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "stream_kbps=", 12) == 0)
            streamKbps.store(sanitize(std::atoi(line + 12)));
        else if (std::strncmp(line, "download_kbps=", 14) == 0)
            downloadKbps.store(sanitize(std::atoi(line + 14)));
        else if (std::strncmp(line, "album_grid=", 11) == 0)
            albumGrid.store(line[11] == '1');
    }
    std::fclose(f);
}

bool MusicSettings::save(const std::string& path) const
{
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    std::fprintf(f, "stream_kbps=%d\ndownload_kbps=%d\nalbum_grid=%d\n", streamKbps.load(),
                 downloadKbps.load(), albumGrid.load() ? 1 : 0);
    const bool ok = std::fclose(f) == 0;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

} // namespace music
} // namespace miyoofin
