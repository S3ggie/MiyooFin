#ifndef MIYOOFIN_DOWNLOAD_AUDIO_HPP
#define MIYOOFIN_DOWNLOAD_AUDIO_HPP
#include "DownloadTypes.hpp"
#include <string>
#include <vector>

namespace miyoofin {
struct DownloadAudioOption
{
    std::string lang; // "" = the server's default track
    std::string label;
};
/// Audio choices for a download: "Default audio" first, then each language found
/// across the items (most common first). Only the default is returned when no
/// language is offered, so callers can skip the menu.
std::vector<DownloadAudioOption> downloadAudioOptions(const std::vector<DownloadItem>& items);
/// Asks for `lang` on every item; an item without that language keeps its default track.
void applyDownloadAudio(std::vector<DownloadItem>& items, const std::string& lang);
std::string audioLanguageName(const std::string& code);
} // namespace miyoofin
#endif
