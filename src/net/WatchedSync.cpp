#include "WatchedSync.hpp"
#include "JellyfinApi.hpp"
#include "RouteRequest.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>

namespace miyoofin {

namespace {

WatchedSync::Result defaultSend(const Session& session, const std::string& itemId, bool played)
{
    if (!session.valid())
        return WatchedSync::Result::Drop;
    PlaybackSyncStatus status = PlaybackSyncStatus::Transient;
    std::string error;
    RouteRequest(session).run(
        [&](const std::string& base) {
            status = JellyfinApi::setPlayed(base, session.accessToken, session.userId,
                                            session.deviceId, itemId, played, error);
            return status == PlaybackSyncStatus::Success;
        },
        error);
    switch (status) {
    case PlaybackSyncStatus::Success:
        return WatchedSync::Result::Done;
    case PlaybackSyncStatus::Missing:
    case PlaybackSyncStatus::Unauthorized:
        return WatchedSync::Result::Drop;
    default:
        return WatchedSync::Result::Retry;
    }
}

} // namespace

WatchedSync::WatchedSync(std::string path, Sender sender)
    : m_path(std::move(path)), m_sender(sender ? std::move(sender) : Sender(defaultSend))
{
    m_thread = std::thread([this] { loop(); });
}

WatchedSync::~WatchedSync()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

WatchedSync& WatchedSync::instance()
{
    static WatchedSync sync("watched-pending.txt");
    return sync;
}

void WatchedSync::setSession(const Session& session)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_session = session;
        m_nudge = true; // a new sign-in may be able to send what was waiting
    }
    m_wake.notify_all();
}

void WatchedSync::configure(std::string path, const Session& session)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_path = std::move(path);
        m_session = session;
        m_nudge = true;
    }
    m_wake.notify_all();
}

void WatchedSync::setPath(std::string path)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_path = std::move(path);
        m_nudge = true;
    }
    m_wake.notify_all();
}

std::vector<WatchedSync::Entry> WatchedSync::loadFrom(const std::string& path)
{
    std::vector<Entry> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0 || tab + 1 >= line.size())
            continue;
        out.push_back({line.substr(0, tab), line[tab + 1] == '1'});
    }
    return out;
}

void WatchedSync::storeTo(const std::string& path, const std::vector<Entry>& entries)
{
    if (entries.empty()) {
        std::remove(path.c_str());
        return;
    }
    // The account's cache folder may not exist yet.
    for (std::size_t i = path.find('/'); i != std::string::npos; i = path.find('/', i + 1))
        ::mkdir(path.substr(0, i).c_str(), 0755);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const Entry& e : entries)
            out << e.itemId << '\t' << (e.played ? 1 : 0) << '\n';
        if (!out.good())
            return;
    }
    std::rename(tmp.c_str(), path.c_str());
}

void WatchedSync::enqueue(const std::string& itemId, bool played)
{
    if (itemId.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Entry> all = loadLocked();
        all.erase(std::remove_if(all.begin(), all.end(),
                                 [&](const Entry& e) { return e.itemId == itemId; }),
                  all.end());
        all.push_back({itemId, played});
        storeLocked(all);
        m_nudge = true;
    }
    m_wake.notify_all();
}

std::size_t WatchedSync::pending() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return loadLocked().size();
}

void WatchedSync::loop()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stop) {
        std::vector<Entry> all = loadLocked();
        if (all.empty() || !m_session.valid()) {
            m_nudge = false;
            m_wake.wait(lock, [this] { return m_stop || m_nudge; });
            continue;
        }
        const Entry entry = all.front();
        const Session session = m_session;
        const std::string sentFrom = m_path; // the queue this change came out of
        lock.unlock();
        const Result result = m_sender(session, entry.itemId, entry.played);
        lock.lock();
        if (result == Result::Retry) {
            m_nudge = false;
            m_wake.wait_for(lock, std::chrono::seconds(retrySeconds()),
                            [this] { return m_stop || m_nudge; });
            continue;
        }
        // Done or Drop: forget it in the queue it came from (the account may have changed while it
        // was in flight), unless the user changed their mind about it meanwhile.
        std::vector<Entry> now = loadFrom(sentFrom);
        now.erase(std::remove_if(now.begin(), now.end(),
                                 [&](const Entry& e) {
                                     return e.itemId == entry.itemId && e.played == entry.played;
                                 }),
                  now.end());
        storeTo(sentFrom, now);
    }
}

} // namespace miyoofin
