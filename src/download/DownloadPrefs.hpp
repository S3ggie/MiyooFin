#ifndef MIYOOFIN_DOWNLOAD_PREFS_HPP
#define MIYOOFIN_DOWNLOAD_PREFS_HPP

#include <string>

namespace miyoofin {

/// Which audio language new downloads should ask the server for. A downloaded file keeps ONE
/// audio track (the server transcodes a single stream), so the choice has to be made when the
/// download is queued. Empty = whatever the server picks. Persisted in ./download-prefs.txt.
///
/// The value is cached in memory: enqueueing runs on the UI thread and must not touch storage.
void loadDownloadPrefs();                  // reads the file once (call at startup)
const std::string& downloadAudioLang();    // cached; "" = server default
void setDownloadAudioLang(const std::string& lang); // updates the cache and the file

/// Settings-row cycling and display.
std::string nextDownloadAudioLang(const std::string& current);
std::string downloadAudioLabel(const std::string& lang);

} // namespace miyoofin

#endif // MIYOOFIN_DOWNLOAD_PREFS_HPP
