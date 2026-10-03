#ifndef MIYOOFIN_MUSIC_PATHS_HPP
#define MIYOOFIN_MUSIC_PATHS_HPP

#include "../cache/LibraryCache.hpp"
#include "../net/Session.hpp"
#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace miyoofin {
namespace music {

/// Everything MiyooFin Music keeps for one signed-in account lives under `music/<scope>/`,
/// where the scope is the server + user identity (the same key the video catalog uses). Two
/// accounts (or two servers) never see each other's cached lists, downloads, queue, history or
/// pending plays. Device-wide preferences (`music-settings.txt`) stay at the top.
struct MusicPaths
{
    std::string root;      // music/<scope>
    std::string cache;     // lists, covers, stream
    std::string stream;    // cached audio
    std::string downloads; // offline audio + index
    std::string journal;   // plays waiting to be reported
    std::string queue;     // saved play queue
    std::string uiState;   // tab/cursor state

    static MusicPaths forSession(const Session& session)
    {
        MusicPaths p;
        p.root = "music/" + LibraryCache::scopeKey(session.serverUrl, session.userId);
        p.cache = p.root + "/cache";
        p.stream = p.cache + "/stream";
        p.downloads = p.root + "/downloads";
        p.journal = p.root + "/plays.journal";
        p.queue = p.root + "/queue.txt";
        p.uiState = p.root + "/ui-state.txt";
        return p;
    }

    /// Creates `music/` and the account folder.
    void ensureDirs() const
    {
        ::mkdir("music", 0755);
        ::mkdir(root.c_str(), 0755);
    }

    /// Before accounts were separated everything lived at the top level. The first account to open
    /// Music after the upgrade takes that data over (it is the one that made it), by moving it, so
    /// nothing is copied or lost. Does nothing once the account has data of its own.
    void adoptLegacyState() const
    {
        struct stat st;
        if (::stat(root.c_str(), &st) == 0)
            return; // this account already has its own folder
        ensureDirs();
        const struct
        {
            const char* from;
            const std::string* to;
        } moves[] = {{"music-cache", &cache},      {"music-downloads", &downloads},
                     {"music-queue.txt", &queue},  {"music-plays.journal", &journal},
                     {"music-ui-state.txt", &uiState}};
        for (const auto& m : moves)
            if (::stat(m.from, &st) == 0)
                std::rename(m.from, m.to->c_str());
    }
};

} // namespace music
} // namespace miyoofin

#endif
