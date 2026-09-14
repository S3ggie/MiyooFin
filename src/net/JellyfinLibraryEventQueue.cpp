#include "JellyfinLibraryEvents.hpp"
#include <algorithm>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace miyoofin {

JellyfinLibraryEventQueue::JellyfinLibraryEventQueue(
    std::size_t capacity)
    : m_capacity(std::max<std::size_t>(1, capacity)) {}

bool JellyfinLibraryEventQueue::push(const JellyfinLibraryChangeBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    bool accepted = true;
    const auto merge = [&](const std::vector<std::string> &ids,
                           unsigned flag) {
        for (const auto &id : ids) {
            if (id.empty()) continue;
            auto found = m_ids.find(id);
            if (found == m_ids.end()) {
                if (m_ids.size() >= m_capacity) {
                    m_overflowed = true;
                    accepted = false;
                    continue;
                }
                found = m_ids.emplace(id, 0u).first;
            }
            found->second |= flag;
        }
    };
    merge(batch.itemsAdded, 1u);
    merge(batch.itemsRemoved, 2u);
    merge(batch.itemsUpdated, 4u);
    if (batch.catchUpRequired) m_overflowed = true;
    return accepted;
}

bool JellyfinLibraryEventQueue::pop(JellyfinLibraryChangeBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ids.empty()) {
        batch = {};
        if (m_overflowed) {
            batch.catchUpRequired = true;
            m_overflowed = false;
            return true;
        }
        return false;
    }
    batch = {};
    batch.catchUpRequired = m_overflowed;
    for (const auto &entry : m_ids) {
        if (entry.second & 1u) batch.itemsAdded.push_back(entry.first);
        if (entry.second & 2u) batch.itemsRemoved.push_back(entry.first);
        if (entry.second & 4u) batch.itemsUpdated.push_back(entry.first);
    }
    m_ids.clear();
    m_overflowed = false;
    return true;
}

bool JellyfinLibraryEventQueue::overflowed() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_overflowed;
}

std::size_t JellyfinLibraryEventQueue::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ids.size();
}

void JellyfinLibraryEventQueue::markCatchUpRequired() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_overflowed = true;
}

} // namespace miyoofin
