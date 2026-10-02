#ifndef MIYOOFIN_DOWNLOAD_SUBTITLES_HPP
#define MIYOOFIN_DOWNLOAD_SUBTITLES_HPP

#include "../net/Session.hpp"
#include <string>

namespace miyoofin {

/// Saves an item's selectable text subtitle tracks next to its downloaded
/// segments so offline playback can show them:
///   <itemDir>/playback-tracks.txt   subtitle lines only (a download has one audio track)
///   <itemDir>/subs/<index>.srt
/// Best effort and idempotent (does nothing when the track list already exists);
/// call from a background thread only. Returns true when sidecars are present.
bool fetchSubtitleSidecars(const Session& session, const std::string& itemDir,
                           const std::string& itemId, const std::string& mediaSourceId);

/// Removes the sidecar files created above (used when a download is deleted).
void removeSubtitleSidecars(const std::string& itemDir);

} // namespace miyoofin

#endif // MIYOOFIN_DOWNLOAD_SUBTITLES_HPP
