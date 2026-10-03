#ifndef MIYOOFIN_PLAYER_PREFS_HPP
#define MIYOOFIN_PLAYER_PREFS_HPP

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace miyoofin {

/// The player's remembered subtitle/audio language (player-prefs.txt, same
/// format the player reads and rewrites when a track is switched in playback).
/// The Settings rows set the starting choice; switching tracks while watching
/// still updates it.
struct PlayerPrefs
{
    std::string subLang; ///< ISO 639-2 code, empty = the file's default
    bool subOff = false;
    std::string audioLang; ///< empty = the server's default

    static PlayerPrefs load(const std::string& path)
    {
        PlayerPrefs p;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == "sub_lang")
                p.subLang = value;
            else if (key == "audio_lang")
                p.audioLang = value;
            else if (key == "sub_off")
                p.subOff = !value.empty() && value[0] == '1';
        }
        return p;
    }
    bool save(const std::string& path) const
    {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << "sub_lang=" << subLang << "\nsub_off=" << (subOff ? 1 : 0)
                << "\naudio_lang=" << audioLang << "\n";
            if (!out)
                return false;
        }
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }
};

} // namespace miyoofin

#endif
