#include "PlaysJournal.hpp"
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace miyoofin {
namespace music {

namespace {
constexpr std::size_t kMaxEntries = 500; // an offline month of listening; oldest dropped
}

std::vector<PlaysJournal::Entry> PlaysJournal::load() const
{
    std::vector<Entry> out;
    std::ifstream in(m_path);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0)
            continue;
        out.push_back({line.substr(0, tab), std::strtoll(line.c_str() + tab + 1, nullptr, 10)});
    }
    return out;
}

void PlaysJournal::store(const std::vector<Entry>& entries) const
{
    if (entries.empty()) {
        std::remove(m_path.c_str());
        return;
    }
    const std::string tmp = m_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        for (const Entry& e : entries)
            out << e.trackId << '\t' << e.epochSeconds << '\n';
        if (!out.good())
            return;
    }
    std::rename(tmp.c_str(), m_path.c_str());
}

void PlaysJournal::add(const std::string& trackId, std::int64_t epochSeconds)
{
    if (trackId.empty())
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Entry> all = load();
    all.push_back({trackId, epochSeconds});
    if (all.size() > kMaxEntries)
        all.erase(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(all.size() - kMaxEntries));
    store(all);
}

std::vector<PlaysJournal::Entry> PlaysJournal::entries() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return load();
}

int PlaysJournal::flush(const std::function<bool(const Entry&)>& send, int maxEntries)
{
    std::vector<Entry> all;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        all = load();
    }
    int sent = 0;
    for (const Entry& e : all) {
        if (sent >= maxEntries || !send(e))
            break;
        ++sent;
    }
    if (sent > 0) {
        // Re-read: new entries may have been added while the network calls ran.
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Entry> now = load();
        now.erase(now.begin(), now.begin() + static_cast<std::ptrdiff_t>(
                                                 std::min<std::size_t>(sent, now.size())));
        store(now);
    }
    return sent;
}

std::string PlaysJournal::isoTime(std::int64_t epochSeconds)
{
    const std::time_t t = static_cast<std::time_t>(epochSeconds);
    std::tm utc{};
    gmtime_r(&t, &utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buf;
}

} // namespace music
} // namespace miyoofin
