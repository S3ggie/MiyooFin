#include "DownloadSubtitles.hpp"

#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include "miyoofin/playback_tracks.hpp"

#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace miyoofin {

namespace {

bool fileExists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

bool writeAtomic(const std::string& path, const std::string& data)
{
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace

bool fetchSubtitleSidecars(const Session& session, const std::string& itemDir,
                           const std::string& itemId, const std::string& mediaSourceId)
{
    const std::string tracksPath = itemDir + "/playback-tracks.txt";
    if (fileExists(tracksPath))
        return true;
    std::string body, error;
    if (!RouteRequest(session).run(
            [&](const std::string& base) {
                return JellyfinApi::getItemJson(base, session.accessToken, session.userId,
                                                session.deviceId, itemId, body, error);
            },
            error))
        return false;
    PlaybackTracks all;
    if (!playback_parse_tracks(body, all))
        return false;
    // Sidecar list: subtitles only. Offline playback has a single fixed audio track.
    PlaybackTracks subtitles;
    subtitles.mediaSourceId = all.mediaSourceId;
    for (const PlaybackTrack& t : playback_subtitle_fetch_order(all, 12))
        subtitles.tracks.push_back(t);
    if (subtitles.tracks.empty())
        return false;
    const std::string source = mediaSourceId.empty() ? all.mediaSourceId : mediaSourceId;
    ::mkdir((itemDir + "/subs").c_str(), 0755);
    PlaybackTracks saved;
    saved.mediaSourceId = source;
    for (const PlaybackTrack& t : subtitles.tracks) {
        std::string srt;
        const bool ok = RouteRequest(session).run(
            [&](const std::string& base) {
                return JellyfinApi::getSubtitleSrt(base, session.accessToken, session.deviceId,
                                                   itemId, source, t.index, srt, error);
            },
            error);
        if (ok && writeAtomic(itemDir + "/subs/" + std::to_string(t.index) + ".srt", srt))
            saved.tracks.push_back(t);
    }
    if (saved.tracks.empty())
        return false;
    // Written last: its presence means the sidecars are complete.
    return writeAtomic(tracksPath, playback_format_tracks(saved));
}

void removeSubtitleSidecars(const std::string& itemDir)
{
    std::remove((itemDir + "/playback-tracks.txt").c_str());
    std::remove((itemDir + "/playback-tracks.txt.tmp").c_str());
    const std::string dir = itemDir + "/subs";
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name != "." && name != "..")
                std::remove((dir + "/" + name).c_str());
        }
        closedir(d);
    }
    ::rmdir(dir.c_str());
}

} // namespace miyoofin
