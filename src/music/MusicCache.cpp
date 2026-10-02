#include "MusicCache.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace miyoofin {
namespace music {

namespace {

constexpr const char* kMagic = "miyoofin-music-list 1";

std::string clean(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return out;
}

bool makeDirs(const std::string& path)
{
    std::string partial;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!partial.empty() && partial != "." && ::mkdir(partial.c_str(), 0755) != 0 &&
                errno != EEXIST)
                return false;
        }
        if (i < path.size())
            partial += path[i];
    }
    return true;
}

std::vector<std::string> split(const std::string& line)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

std::int64_t toInt64(const std::string& s)
{
    return s.empty() ? 0 : std::strtoll(s.c_str(), nullptr, 10);
}

} // namespace

std::string MusicCache::fileKey(const std::string& key)
{
    std::string out;
    for (char c : key)
        out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_')
                   ? c
                   : '_';
    return out.empty() ? "_" : out;
}

void MusicCache::erase(const std::string& key) const
{
    std::remove(pathFor(key).c_str());
}

std::string MusicCache::pathFor(const std::string& key) const
{
    return m_dir + "/" + fileKey(key) + ".tsv";
}

bool MusicCache::save(const std::string& key, const std::vector<std::vector<std::string>>& rows,
                      int total) const
{
    if (!makeDirs(m_dir))
        return false;
    const std::string path = pathFor(key), tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out)
            return false;
        out << kMagic << "\ntotal\t" << total << "\n";
        for (const auto& row : rows) {
            for (std::size_t i = 0; i < row.size(); ++i)
                out << (i ? "\t" : "") << clean(row[i]);
            out << "\n";
        }
        if (!out.good())
            return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool MusicCache::load(const std::string& key, std::vector<std::vector<std::string>>& rows,
                      int& total) const
{
    std::ifstream in(pathFor(key));
    std::string line;
    if (!in || !std::getline(in, line) || line != kMagic)
        return false;
    if (!std::getline(in, line) || line.compare(0, 6, "total\t") != 0)
        return false;
    total = std::atoi(line.c_str() + 6);
    rows.clear();
    while (std::getline(in, line))
        if (!line.empty())
            rows.push_back(split(line));
    return true;
}

bool MusicCache::saveTracks(const std::string& key, const std::vector<Track>& items,
                            int total) const
{
    std::vector<std::vector<std::string>> rows;
    for (const Track& t : items)
        rows.push_back({t.id, t.title, t.album, t.albumId, t.artist, t.artistId, t.albumArtist,
                        t.imageTag, t.albumImageTag, std::to_string(t.trackNumber),
                        std::to_string(t.discNumber), std::to_string(t.runTimeTicks),
                        t.favorite ? "1" : "0", t.entryId});
    return save(key, rows, total);
}

bool MusicCache::loadTracks(const std::string& key, std::vector<Track>& items, int& total) const
{
    std::vector<std::vector<std::string>> rows;
    if (!load(key, rows, total))
        return false;
    items.clear();
    for (const auto& r : rows) {
        if (r.size() < 13 || r[0].empty())
            continue;
        Track t;
        t.id = r[0];
        t.title = r[1];
        t.album = r[2];
        t.albumId = r[3];
        t.artist = r[4];
        t.artistId = r[5];
        t.albumArtist = r[6];
        t.imageTag = r[7];
        t.albumImageTag = r[8];
        t.trackNumber = static_cast<int>(toInt64(r[9]));
        t.discNumber = static_cast<int>(toInt64(r[10]));
        t.runTimeTicks = toInt64(r[11]);
        t.favorite = r[12] == "1";
        if (r.size() > 13)
            t.entryId = r[13];
        items.push_back(std::move(t));
    }
    return true;
}

bool MusicCache::saveAlbums(const std::string& key, const std::vector<Album>& items,
                            int total) const
{
    std::vector<std::vector<std::string>> rows;
    for (const Album& a : items)
        rows.push_back({a.id, a.title, a.artist, a.artistId, a.imageTag, std::to_string(a.year),
                        std::to_string(a.trackCount), std::to_string(a.runTimeTicks)});
    return save(key, rows, total);
}

bool MusicCache::loadAlbums(const std::string& key, std::vector<Album>& items, int& total) const
{
    std::vector<std::vector<std::string>> rows;
    if (!load(key, rows, total))
        return false;
    items.clear();
    for (const auto& r : rows) {
        if (r.size() < 8 || r[0].empty())
            continue;
        Album a;
        a.id = r[0];
        a.title = r[1];
        a.artist = r[2];
        a.artistId = r[3];
        a.imageTag = r[4];
        a.year = static_cast<int>(toInt64(r[5]));
        a.trackCount = static_cast<int>(toInt64(r[6]));
        a.runTimeTicks = toInt64(r[7]);
        items.push_back(std::move(a));
    }
    return true;
}

bool MusicCache::saveArtists(const std::string& key, const std::vector<Artist>& items,
                             int total) const
{
    std::vector<std::vector<std::string>> rows;
    for (const Artist& a : items)
        rows.push_back({a.id, a.name, a.imageTag});
    return save(key, rows, total);
}

bool MusicCache::loadArtists(const std::string& key, std::vector<Artist>& items, int& total) const
{
    std::vector<std::vector<std::string>> rows;
    if (!load(key, rows, total))
        return false;
    items.clear();
    for (const auto& r : rows) {
        if (r.size() < 3 || r[0].empty())
            continue;
        items.push_back({r[0], r[1], r[2]});
    }
    return true;
}

bool MusicCache::savePlaylists(const std::string& key, const std::vector<Playlist>& items,
                               int total) const
{
    std::vector<std::vector<std::string>> rows;
    for (const Playlist& p : items)
        rows.push_back({p.id, p.title, p.imageTag, std::to_string(p.trackCount),
                        std::to_string(p.runTimeTicks)});
    return save(key, rows, total);
}

bool MusicCache::loadPlaylists(const std::string& key, std::vector<Playlist>& items,
                               int& total) const
{
    std::vector<std::vector<std::string>> rows;
    if (!load(key, rows, total))
        return false;
    items.clear();
    for (const auto& r : rows) {
        if (r.size() < 5 || r[0].empty())
            continue;
        Playlist p;
        p.id = r[0];
        p.title = r[1];
        p.imageTag = r[2];
        p.trackCount = static_cast<int>(toInt64(r[3]));
        p.runTimeTicks = toInt64(r[4]);
        items.push_back(std::move(p));
    }
    return true;
}

} // namespace music
} // namespace miyoofin
