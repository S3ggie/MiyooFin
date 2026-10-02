#include "MusicQueue.hpp"
#include <algorithm>
#include <cstdlib>
#include <numeric>

namespace miyoofin {
namespace music {

void MusicQueue::set(std::vector<Track> tracks, int startIndex)
{
    m_tracks = std::move(tracks);
    m_order.resize(m_tracks.size());
    std::iota(m_order.begin(), m_order.end(), 0);
    m_pos = m_tracks.empty() ? 0 : std::max(0, std::min(startIndex, size() - 1));
    if (m_shuffle)
        reshuffleKeepingCurrent();
}

void MusicQueue::clear()
{
    m_tracks.clear();
    m_order.clear();
    m_pos = 0;
}

const Track* MusicQueue::current() const
{
    return at(m_pos);
}

const Track* MusicQueue::at(int position) const
{
    if (position < 0 || position >= size())
        return nullptr;
    return &m_tracks[m_order[position]];
}

const Track* MusicQueue::peekNext() const
{
    if (empty())
        return nullptr;
    if (m_repeat == Repeat::One)
        return current();
    if (m_pos + 1 < size())
        return at(m_pos + 1);
    return m_repeat == Repeat::All ? at(0) : nullptr;
}

void MusicQueue::reshuffleKeepingCurrent()
{
    if (m_order.empty())
        return;
    const int currentIndex = m_order[m_pos];
    std::vector<int> rest;
    rest.reserve(m_order.size());
    for (int i = 0; i < static_cast<int>(m_tracks.size()); ++i)
        if (i != currentIndex)
            rest.push_back(i);
    std::shuffle(rest.begin(), rest.end(), m_rng);
    m_order.clear();
    m_order.push_back(currentIndex);
    m_order.insert(m_order.end(), rest.begin(), rest.end());
    m_pos = 0;
}

SavedQueue MusicQueue::snapshot(double seconds) const
{
    SavedQueue s;
    s.shuffle = m_shuffle;
    s.repeat = m_repeat;
    s.seconds = seconds;
    int first = 0, last = size();
    if (last > kSavedQueueMaxTracks) {
        first = std::max(0, std::min(m_pos - 50, last - kSavedQueueMaxTracks));
        last = first + kSavedQueueMaxTracks;
    }
    for (int i = first; i < last; ++i)
        s.tracks.push_back(m_tracks[m_order[i]]);
    s.position = m_pos - first;
    return s;
}

void MusicQueue::restore(const SavedQueue& saved)
{
    m_tracks = saved.tracks;
    m_order.resize(m_tracks.size());
    std::iota(m_order.begin(), m_order.end(), 0);
    m_pos = m_tracks.empty() ? 0 : std::max(0, std::min(saved.position, size() - 1));
    m_shuffle = saved.shuffle;
    m_repeat = saved.repeat;
}

namespace {

std::string clean(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return out;
}

} // namespace

std::string serializeQueue(const SavedQueue& q)
{
    std::string out = "MFMQ=1\n";
    out += "state\t" + std::to_string(q.position) + "\t" + std::to_string(q.seconds) + "\t" +
           (q.shuffle ? "1" : "0") + "\t" + std::to_string(static_cast<int>(q.repeat)) + "\n";
    for (const Track& t : q.tracks)
        out += "t\t" + clean(t.id) + "\t" + clean(t.title) + "\t" + clean(t.album) + "\t" +
               clean(t.albumId) + "\t" + clean(t.artist) + "\t" + clean(t.artistId) + "\t" +
               clean(t.albumArtist) + "\t" + clean(t.imageTag) + "\t" + clean(t.albumImageTag) +
               "\t" + std::to_string(t.trackNumber) + "\t" + std::to_string(t.discNumber) + "\t" +
               std::to_string(t.runTimeTicks) + "\n";
    return out;
}

bool parseQueue(const std::string& text, SavedQueue& out)
{
    SavedQueue q;
    std::size_t pos = text.find('\n');
    if (pos == std::string::npos || text.compare(0, pos, "MFMQ=1") != 0)
        return false;
    bool haveState = false;
    while (pos != std::string::npos && pos + 1 < text.size()) {
        const std::size_t start = pos + 1;
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos)
            break; // a truncated last line is dropped
        std::vector<std::string> f;
        std::size_t s = start;
        for (;;) {
            const std::size_t tab = text.find('\t', s);
            if (tab == std::string::npos || tab >= end) {
                f.push_back(text.substr(s, end - s));
                break;
            }
            f.push_back(text.substr(s, tab - s));
            s = tab + 1;
        }
        if (f[0] == "state" && f.size() >= 5) {
            q.position = std::atoi(f[1].c_str());
            q.seconds = std::atof(f[2].c_str());
            q.shuffle = f[3] == "1";
            const int repeat = std::atoi(f[4].c_str());
            q.repeat = repeat == 1 ? Repeat::All : (repeat == 2 ? Repeat::One : Repeat::Off);
            haveState = true;
        } else if (f[0] == "t" && f.size() >= 13 && !f[1].empty() &&
                   static_cast<int>(q.tracks.size()) < kSavedQueueMaxTracks) {
            Track t;
            t.id = f[1];
            t.title = f[2];
            t.album = f[3];
            t.albumId = f[4];
            t.artist = f[5];
            t.artistId = f[6];
            t.albumArtist = f[7];
            t.imageTag = f[8];
            t.albumImageTag = f[9];
            t.trackNumber = std::atoi(f[10].c_str());
            t.discNumber = std::atoi(f[11].c_str());
            t.runTimeTicks = std::strtoll(f[12].c_str(), nullptr, 10);
            q.tracks.push_back(std::move(t));
        }
        pos = end;
    }
    if (!haveState || q.tracks.empty())
        return false;
    q.position = std::max(0, std::min(q.position, static_cast<int>(q.tracks.size()) - 1));
    if (q.seconds < 0)
        q.seconds = 0;
    out = std::move(q);
    return true;
}

void MusicQueue::wrapToStart()
{
    if (m_shuffle) {
        // A new pass: reshuffle, and never play the track that just finished again first.
        reshuffleKeepingCurrent();
        m_pos = size() > 1 ? 1 : 0;
    } else {
        m_pos = 0;
    }
}

bool MusicQueue::advance()
{
    if (empty())
        return false;
    if (m_repeat == Repeat::One)
        return true;
    if (m_pos + 1 < size()) {
        ++m_pos;
        return true;
    }
    if (m_repeat == Repeat::All) {
        wrapToStart();
        return true;
    }
    return false;
}

bool MusicQueue::skipNext()
{
    if (empty())
        return false;
    if (m_pos + 1 < size()) {
        ++m_pos;
        return true;
    }
    if (m_repeat == Repeat::Off)
        return false;
    wrapToStart();
    return true;
}

bool MusicQueue::skipPrevious()
{
    if (empty())
        return false;
    if (m_pos > 0) {
        --m_pos;
        return true;
    }
    if (m_repeat == Repeat::All) {
        m_pos = size() - 1;
        return true;
    }
    return false;
}

bool MusicQueue::jumpTo(int position)
{
    if (position < 0 || position >= size())
        return false;
    m_pos = position;
    return true;
}

void MusicQueue::setShuffle(bool on)
{
    if (on == m_shuffle)
        return;
    m_shuffle = on;
    if (empty())
        return;
    if (on) {
        reshuffleKeepingCurrent();
    } else {
        const int currentIndex = m_order[m_pos];
        std::iota(m_order.begin(), m_order.end(), 0);
        m_pos = currentIndex;
    }
}

void MusicQueue::playNext(Track track)
{
    m_tracks.push_back(std::move(track));
    const int index = static_cast<int>(m_tracks.size()) - 1;
    if (m_order.empty()) {
        m_order.push_back(index);
        m_pos = 0;
        return;
    }
    m_order.insert(m_order.begin() + m_pos + 1, index);
}

void MusicQueue::append(Track track)
{
    m_tracks.push_back(std::move(track));
    m_order.push_back(static_cast<int>(m_tracks.size()) - 1);
    if (m_order.size() == 1)
        m_pos = 0;
}

bool MusicQueue::removeAt(int position)
{
    if (position < 0 || position >= size() || position == m_pos)
        return false;
    m_order.erase(m_order.begin() + position);
    if (position < m_pos)
        --m_pos;
    return true;
}

} // namespace music
} // namespace miyoofin
