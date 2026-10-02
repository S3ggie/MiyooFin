#ifndef MIYOOFIN_WATCHED_SYNC_HPP
#define MIYOOFIN_WATCHED_SYNC_HPP

#include "Session.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {

/// Sends "mark watched / unwatched" changes to the server from a background worker. A change is
/// written to a small pending file first, so it survives going offline or quitting the app, and
/// is retried until the server takes it (or says the item no longer exists / we are signed out).
/// The latest choice for an item wins, so toggling twice offline sends only the final state.
class WatchedSync
{
  public:
    enum class Result
    {
        Done,  // the server took it
        Retry, // unreachable or a server error: keep it and try again later
        Drop   // will never succeed (item gone, not signed in): forget it
    };
    using Sender = std::function<Result(const Session&, const std::string& itemId, bool played)>;

    /// `path` is the pending file. The default sender talks to Jellyfin.
    explicit WatchedSync(std::string path, Sender sender = {});
    ~WatchedSync();
    WatchedSync(const WatchedSync&) = delete;
    WatchedSync& operator=(const WatchedSync&) = delete;

    void setSession(const Session& session);
    /// Where pending changes are kept (changes with the signed-in account).
    void setPath(std::string path);
    void enqueue(const std::string& itemId, bool played);
    std::size_t pending() const;
    /// Seconds to wait after a failed attempt (overridable so tests do not sleep).
    std::function<int()> retrySeconds = [] { return 15; };

    /// The process-wide instance the screens use (created on first use).
    static WatchedSync& instance();

  private:
    struct Entry
    {
        std::string itemId;
        bool played;
    };
    void loop();
    std::vector<Entry> loadLocked() const;
    void storeLocked(const std::vector<Entry>& entries) const;

    std::string m_path;
    Sender m_sender;
    Session m_session;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false, m_nudge = false;
    std::thread m_thread;
};

} // namespace miyoofin

#endif
