#ifndef MIYOOFIN_PLAYS_JOURNAL_HPP
#define MIYOOFIN_PLAYS_JOURNAL_HPP

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace miyoofin {
namespace music {

/// A track counts as played once half of it, or four minutes, has been heard.
inline bool countsAsPlayed(double positionSeconds, double durationSeconds)
{
    return positionSeconds >= 240.0 ||
           (durationSeconds > 0 && positionSeconds >= durationSeconds * 0.5);
}

/// Plays that could not be reported (offline, server down) wait here, one line each:
/// "<trackId>\t<epoch seconds>". Each is sent later as "played at that time" so play
/// counts and "recently played" stay right. Thread-safe; file I/O belongs on a worker.
class PlaysJournal
{
  public:
    struct Entry
    {
        std::string trackId;
        std::int64_t epochSeconds = 0;
    };
    explicit PlaysJournal(std::string path) : m_path(std::move(path)) {}

    void add(const std::string& trackId, std::int64_t epochSeconds);
    std::vector<Entry> entries() const;
    /// Sends entries oldest first; each accepted one is removed. Stops at the first refusal
    /// (still offline). `send` returns true when the server took the play (or said the item
    /// no longer exists, which will never succeed). Returns how many were sent.
    int flush(const std::function<bool(const Entry&)>& send, int maxEntries = 20);
    static std::string isoTime(std::int64_t epochSeconds);

  private:
    std::vector<Entry> load() const;
    void store(const std::vector<Entry>& entries) const;
    std::string m_path;
    mutable std::mutex m_mutex;
};

} // namespace music
} // namespace miyoofin

#endif
