#ifndef MIYOOFIN_MUSIC_QUEUE_HPP
#define MIYOOFIN_MUSIC_QUEUE_HPP

#include "MusicTypes.hpp"
#include <cstdint>
#include <random>
#include <vector>

namespace miyoofin {
namespace music {

enum class Repeat
{
    Off,
    All,
    One
};

/// The play queue: the tracks in their source order plus the order they play in.
/// Pure logic (no SDL, no I/O), so every rule is unit-tested on the host.
class MusicQueue
{
  public:
    /// Replaces the queue and starts at `startIndex` (an index into `tracks`). With
    /// shuffle on the start track plays first and the rest follow in random order.
    void set(std::vector<Track> tracks, int startIndex = 0);
    void clear();
    bool empty() const
    {
        return m_tracks.empty();
    }
    int size() const
    {
        return static_cast<int>(m_order.size());
    }
    /// Position of the current track in play order, -1 when the queue is empty.
    int position() const
    {
        return empty() ? -1 : m_pos;
    }
    const Track* current() const;
    /// The track at a play-order position (the Queue view lists these top to bottom).
    const Track* at(int position) const;
    /// The track that a natural end of the current one would start, or nullptr.
    const Track* peekNext() const;

    /// The current track ended on its own. False when the queue is finished.
    bool advance();
    /// The user pressed next. Always moves on, wrapping at the end, unless repeat is off
    /// and the last track is playing (returns false then).
    bool skipNext();
    /// The user pressed previous (the caller restarts the track instead when it is already
    /// well in). Moves back one; at the start stays put unless repeat-all wraps.
    bool skipPrevious();
    bool jumpTo(int position);

    void setShuffle(bool on);
    bool shuffle() const
    {
        return m_shuffle;
    }
    void setRepeat(Repeat repeat)
    {
        m_repeat = repeat;
    }
    Repeat repeat() const
    {
        return m_repeat;
    }

    /// Inserts right after the current track / at the very end.
    void playNext(Track track);
    void append(Track track);
    /// Removes an upcoming or past entry; the current track cannot be removed.
    bool removeAt(int position);

    void seed(std::uint32_t seed)
    {
        m_rng.seed(seed);
    }

  private:
    void reshuffleKeepingCurrent();
    void wrapToStart();
    std::vector<Track> m_tracks;
    std::vector<int> m_order; // play order: indexes into m_tracks
    int m_pos = 0;
    bool m_shuffle = false;
    Repeat m_repeat = Repeat::Off;
    std::mt19937 m_rng{0x5eed};
};

} // namespace music
} // namespace miyoofin

#endif
