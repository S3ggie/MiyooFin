#include "DownloadPrefs.hpp"

#include <cstdio>
#include <cstring>

namespace miyoofin {

namespace {
struct Language
{
    const char* code;
    const char* label;
};
constexpr Language kLanguages[] = {{"", "Server default"}, {"eng", "English"}, {"jpn", "Japanese"},
                                   {"spa", "Spanish"},     {"fra", "French"},  {"deu", "German"},
                                   {"ita", "Italian"},     {"kor", "Korean"},  {"zho", "Chinese"},
                                   {"por", "Portuguese"},  {"rus", "Russian"}};
constexpr const char* kPrefsFile = "download-prefs.txt";
std::string g_audioLang;
bool g_loaded = false;
} // namespace

void loadDownloadPrefs()
{
    g_loaded = true;
    g_audioLang.clear();
    FILE* f = std::fopen(kPrefsFile, "rb");
    if (!f)
        return;
    char line[64];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, "audio_lang=", 11) == 0) {
            g_audioLang = line + 11;
            while (!g_audioLang.empty() &&
                   (g_audioLang.back() == '\n' || g_audioLang.back() == '\r'))
                g_audioLang.pop_back();
            if (g_audioLang.size() > 7)
                g_audioLang.clear();
        }
    }
    std::fclose(f);
}

const std::string& downloadAudioLang()
{
    return g_audioLang;
}

void setDownloadAudioLang(const std::string& lang)
{
    g_audioLang = lang;
    if (FILE* f = std::fopen(kPrefsFile, "wb")) {
        std::fprintf(f, "audio_lang=%s\n", lang.c_str());
        std::fclose(f);
    }
}

std::string nextDownloadAudioLang(const std::string& current)
{
    constexpr int n = sizeof(kLanguages) / sizeof(kLanguages[0]);
    for (int i = 0; i < n; ++i)
        if (current == kLanguages[i].code)
            return kLanguages[(i + 1) % n].code;
    return kLanguages[0].code; // unknown value: start over at the default
}

std::string downloadAudioLabel(const std::string& lang)
{
    for (const Language& l : kLanguages)
        if (lang == l.code)
            return l.label;
    return lang; // an ISO code we have no name for
}

} // namespace miyoofin
