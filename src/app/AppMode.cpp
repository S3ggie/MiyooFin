#include "AppMode.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

AppMode loadAppMode(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f)
        return AppMode::Video;
    char line[64] = {0};
    const bool read = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    return read && std::strncmp(line, "mode=music", 10) == 0 ? AppMode::Music : AppMode::Video;
}

bool saveAppMode(AppMode mode, const std::string& path)
{
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    std::fprintf(f, "mode=%s\n", mode == AppMode::Music ? "music" : "video");
    const bool ok = std::fclose(f) == 0;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

} // namespace miyoofin
