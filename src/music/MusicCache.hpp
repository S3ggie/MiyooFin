#ifndef MIYOOFIN_MUSIC_CACHE_HPP
#define MIYOOFIN_MUSIC_CACHE_HPP

#include "MusicTypes.hpp"
#include <string>
#include <vector>

namespace miyoofin {
namespace music {

/// Last-known listings, one small tab-separated file per listing key, so every music
/// screen can paint instantly (and keep working offline) before the server answers.
/// Writes are atomic (temp file + rename). All calls do file I/O: never from the UI thread.
class MusicCache
{
  public:
    explicit MusicCache(std::string dir) : m_dir(std::move(dir)) {}

    bool saveTracks(const std::string& key, const std::vector<Track>& items, int total) const;
    bool saveAlbums(const std::string& key, const std::vector<Album>& items, int total) const;
    bool saveArtists(const std::string& key, const std::vector<Artist>& items, int total) const;
    bool savePlaylists(const std::string& key, const std::vector<Playlist>& items, int total) const;

    bool loadTracks(const std::string& key, std::vector<Track>& items, int& total) const;
    bool loadAlbums(const std::string& key, std::vector<Album>& items, int& total) const;
    bool loadArtists(const std::string& key, std::vector<Artist>& items, int& total) const;
    bool loadPlaylists(const std::string& key, std::vector<Playlist>& items, int& total) const;

    /// A key reduced to characters safe in a file name.
    static std::string fileKey(const std::string& key);
    const std::string& dir() const
    {
        return m_dir;
    }

  private:
    std::string pathFor(const std::string& key) const;
    bool save(const std::string& key, const std::vector<std::vector<std::string>>& rows,
              int total) const;
    bool load(const std::string& key, std::vector<std::vector<std::string>>& rows,
              int& total) const;
    std::string m_dir;
};

} // namespace music
} // namespace miyoofin

#endif
