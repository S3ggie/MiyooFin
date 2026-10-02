#include "MusicQueue.hpp"
#include <algorithm>
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
