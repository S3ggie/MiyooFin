#ifndef MIYOOFIN_JELLYFIN_LIBRARY_EVENTS_HPP
#define MIYOOFIN_JELLYFIN_LIBRARY_EVENTS_HPP

#include "JellyfinLibraryEventParse.hpp"
#include "Session.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace miyoofin {

struct JellyfinLibraryChangeBatch {
    std::vector<std::string> itemsAdded;
    std::vector<std::string> itemsRemoved;
    std::vector<std::string> itemsUpdated;
    bool catchUpRequired = false;
    bool userDataChanged = false;
};

enum class JellyfinLibraryEventParse {
    Parsed,
    Ignored,
    Malformed,
    Oversized
};

enum class JellyfinLibraryEventRun {
    Stopped,
    Cancelled
};

class JellyfinLibraryEventQueue {
public:
    explicit JellyfinLibraryEventQueue(
        std::size_t capacity = jellyfin_library_events_detail::kMaxQueuedIds);

    bool push(const JellyfinLibraryChangeBatch &batch);
    bool pop(JellyfinLibraryChangeBatch &batch);
    bool overflowed() const;
    std::size_t size() const;
    void markCatchUpRequired();

private:
    std::size_t m_capacity;
    std::map<std::string, unsigned> m_ids;
    mutable std::mutex m_mutex;
    bool m_overflowed = false;
    bool m_userDataChanged = false;
};

class JellyfinLibraryEvents {
public:
    explicit JellyfinLibraryEvents(Session session) : m_session(std::move(session)) {}

    static JellyfinLibraryEventParse parseMessage(
        const std::string &message, JellyfinLibraryChangeBatch &batch);

    static std::string buildSocketUrl(const std::string &baseUrl,
                                      const std::string &accessToken,
                                      const std::string &deviceId);

    class Backoff {
    public:
        Backoff(unsigned initialMs, unsigned maximumMs)
            : m_initial(initialMs), m_current(initialMs), m_maximum(maximumMs) {}
        unsigned nextDelayMs()
        {
            const unsigned delay = m_current;
            m_current = std::min(m_maximum, m_current > m_maximum / 2
                ? m_maximum : m_current * 2);
            return delay;
        }
        void reset() { m_current = m_initial; }
    private:
        unsigned m_initial;
        unsigned m_current;
        unsigned m_maximum;
    };

    static JellyfinLibraryEventRun waitForRetry(
        const std::atomic<bool> &cancelled, unsigned delayMs);

    // Runs on the caller's worker thread. It reconnects through the normal
    // LAN/public route policy and leaves catch-up to LibrarySync consumers.
    JellyfinLibraryEventRun run(
        JellyfinLibraryEventQueue &queue,
        const std::shared_ptr<std::atomic_bool> &cancelled) const;

    std::future<JellyfinLibraryEventRun> start(
        std::shared_ptr<JellyfinLibraryEventQueue> queue,
        const std::shared_ptr<std::atomic_bool> &cancelled) const;

private:
    static bool sendAll(void *curl, const std::string &payload,
                        const std::atomic<bool> &cancelled);
    static int receiveSome(void *curl, std::vector<unsigned char> &bytes);
    static std::string hostHeader(const std::string &url);
    static std::string requestTarget(const std::string &url);

    bool connectAndConsume(const std::string &baseUrl,
                           JellyfinLibraryEventQueue &queue,
                           const std::atomic<bool> &cancelled,
                           std::string &error) const;

    static bool consumeFrames(void *curl, std::vector<unsigned char> &bytes,
                              JellyfinLibraryEventQueue &queue,
                              const std::atomic<bool> &cancelled,
                              std::string &error);

    Session m_session;
};

} // namespace miyoofin

#endif // MIYOOFIN_JELLYFIN_LIBRARY_EVENTS_HPP
