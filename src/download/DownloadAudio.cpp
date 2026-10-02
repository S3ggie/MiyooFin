#include "DownloadAudio.hpp"
#include <algorithm>
#include <map>

namespace miyoofin {

std::string audioLanguageName(const std::string& code)
{
    static const std::map<std::string, std::string> names = {
        {"eng", "English"}, {"jpn", "Japanese"},  {"spa", "Spanish"}, {"fra", "French"},
        {"fre", "French"},  {"deu", "German"},    {"ger", "German"},  {"ita", "Italian"},
        {"kor", "Korean"},  {"zho", "Chinese"},   {"chi", "Chinese"}, {"por", "Portuguese"},
        {"rus", "Russian"}, {"hin", "Hindi"},     {"ara", "Arabic"},  {"tha", "Thai"},
        {"pol", "Polish"},  {"tur", "Turkish"},   {"nld", "Dutch"},   {"dut", "Dutch"},
        {"swe", "Swedish"}, {"vie", "Vietnamese"}};
    auto it = names.find(code);
    return it != names.end() ? it->second : code;
}

std::vector<DownloadAudioOption> downloadAudioOptions(const std::vector<DownloadItem>& items)
{
    struct Seen
    {
        std::string lang, label;
        int count = 0, order = 0;
    };
    std::vector<Seen> seen;
    for (const DownloadItem& item : items) {
        std::vector<std::string> inThisItem;
        for (const auto& track : item.audioTracks) {
            const std::string& lang = track.first;
            if (lang.empty() ||
                std::find(inThisItem.begin(), inThisItem.end(), lang) != inThisItem.end())
                continue;
            inThisItem.push_back(lang);
            auto it = std::find_if(seen.begin(), seen.end(),
                                   [&](const Seen& s) { return s.lang == lang; });
            if (it == seen.end())
                seen.push_back({lang, audioLanguageName(lang), 1, (int)seen.size()});
            else
                ++it->count;
        }
    }
    std::stable_sort(seen.begin(), seen.end(),
                     [](const Seen& a, const Seen& b) { return a.count > b.count; });
    std::vector<DownloadAudioOption> out = {{"", "Default audio"}};
    for (const Seen& s : seen)
        out.push_back({s.lang, s.label});
    return out;
}

void applyDownloadAudio(std::vector<DownloadItem>& items, const std::string& lang)
{
    for (DownloadItem& item : items)
        item.audioLang = lang;
}

} // namespace miyoofin
