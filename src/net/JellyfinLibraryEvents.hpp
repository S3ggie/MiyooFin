#ifndef MIYOOFIN_JELLYFIN_LIBRARY_EVENTS_HPP
#define MIYOOFIN_JELLYFIN_LIBRARY_EVENTS_HPP

#include "JellyfinApi.hpp"
#include "RouteRequest.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace miyoofin {

struct JellyfinLibraryChangeBatch {
    std::vector<std::string> itemsAdded;
    std::vector<std::string> itemsRemoved;
    std::vector<std::string> itemsUpdated;
    bool catchUpRequired = false;
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

namespace jellyfin_library_events_detail {

constexpr std::size_t kMaxMessageBytes = 32 * 1024;
constexpr std::size_t kMaxQueuedIds = 512;

inline void skipWhitespace(const std::string &json, std::size_t &pos)
{
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'
           || json[pos] == '\r' || json[pos] == '\n')) ++pos;
}

inline bool parseString(const std::string &json, std::size_t &pos,
                        std::string &value)
{
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    value.clear();
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            if (static_cast<unsigned char>(c) < 0x20) return false;
            value += c;
            continue;
        }
        if (pos >= json.size()) return false;
        const char escaped = json[pos++];
        switch (escaped) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case 'u': {
            if (pos + 4 > json.size()) return false;
            // Jellyfin identifiers are ASCII. Preserve non-ASCII escapes as
            // UTF-8 only where they fit in one byte; reject malformed hex.
            unsigned value16 = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const char h = json[pos++];
                unsigned digit = 0;
                if (h >= '0' && h <= '9') digit = static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f') digit = static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') digit = static_cast<unsigned>(h - 'A' + 10);
                else return false;
                value16 = (value16 << 4) | digit;
            }
            if (value16 > 0x7f) return false;
            value += static_cast<char>(value16);
            break;
        }
        default: return false;
        }
    }
    return false;
}

inline bool findKey(const std::string &json, const std::string &key,
                    std::size_t &valuePos)
{
    std::size_t pos = 0;
    while (pos < json.size()) {
        skipWhitespace(json, pos);
        if (pos >= json.size()) break;
        if (json[pos] != '"') {
            ++pos;
            continue;
        }
        std::string candidate;
        const std::size_t keyStart = pos;
        if (!parseString(json, pos, candidate)) return false;
        skipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] != ':') {
            // A quoted value elsewhere in the document is not a key.
            pos = keyStart + 1;
            continue;
        }
        ++pos;
        if (candidate == key) {
            skipWhitespace(json, pos);
            valuePos = pos;
            return true;
        }
        skipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '"') {
            std::string ignored;
            if (!parseString(json, pos, ignored)) return false;
        } else {
            ++pos;
        }
    }
    return false;
}

inline bool rawValue(const std::string &json, std::size_t pos,
                     std::string &value)
{
    if (pos >= json.size()) return false;
    const std::size_t start = pos;
    if (json[pos] == '"') {
        std::string ignored;
        if (!parseString(json, pos, ignored)) return false;
        value = json.substr(start, pos - start);
        return true;
    }
    if (json[pos] != '{' && json[pos] != '[') return false;
    const char open = json[pos];
    const char close = open == '{' ? '}' : ']';
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == open) ++depth;
        else if (c == close && --depth == 0) {
            ++pos;
            value = json.substr(start, pos - start);
            return true;
        }
    }
    return false;
}

inline bool arrayStrings(const std::string &json,
                         std::vector<std::string> &values)
{
    values.clear();
    std::size_t pos = 0;
    skipWhitespace(json, pos);
    if (pos >= json.size() || json[pos++] != '[') return false;
    skipWhitespace(json, pos);
    if (pos < json.size() && json[pos] == ']') return true;
    while (pos < json.size()) {
        std::string value;
        if (!parseString(json, pos, value) || value.empty()) return false;
        values.push_back(std::move(value));
        if (values.size() > kMaxQueuedIds) return false;
        skipWhitespace(json, pos);
        if (pos >= json.size()) return false;
        if (json[pos] == ']') return ++pos == json.size();
        if (json[pos++] != ',') return false;
        skipWhitespace(json, pos);
    }
    return false;
}

inline bool optionalArray(const std::string &json, const std::string &key,
                          std::vector<std::string> &values)
{
    std::size_t pos = 0;
    if (!findKey(json, key, pos)) {
        values.clear();
        return true;
    }
    std::string raw;
    return rawValue(json, pos, raw) && arrayStrings(raw, values);
}

inline std::string urlEscape(const std::string &value)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '-' || c == '_'
            || c == '.' || c == '~') result += static_cast<char>(c);
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

} // namespace jellyfin_library_events_detail

class JellyfinLibraryEventQueue {
public:
    explicit JellyfinLibraryEventQueue(
        std::size_t capacity = jellyfin_library_events_detail::kMaxQueuedIds)
        : m_capacity(std::max<std::size_t>(1, capacity)) {}

    bool push(const JellyfinLibraryChangeBatch &batch)
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

    bool pop(JellyfinLibraryChangeBatch &batch)
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

    bool overflowed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_overflowed;
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ids.size();
    }
    void markCatchUpRequired() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_overflowed = true;
    }

private:
    std::size_t m_capacity;
    std::map<std::string, unsigned> m_ids;
    mutable std::mutex m_mutex;
    bool m_overflowed = false;
};

class JellyfinLibraryEvents {
public:
    explicit JellyfinLibraryEvents(Session session) : m_session(std::move(session)) {}

    static JellyfinLibraryEventParse parseMessage(
        const std::string &message, JellyfinLibraryChangeBatch &batch)
    {
        using namespace jellyfin_library_events_detail;
        batch = {};
        if (message.size() > kMaxMessageBytes)
            return JellyfinLibraryEventParse::Oversized;

        std::size_t typePos = 0;
        std::string rawType;
        if (!findKey(message, "MessageType", typePos)
            || !rawValue(message, typePos, rawType))
            return JellyfinLibraryEventParse::Malformed;
        std::size_t stringPos = 0;
        std::string messageType;
        if (rawType.empty() || rawType[0] != '"'
            || !parseString(rawType, stringPos, messageType))
            return JellyfinLibraryEventParse::Malformed;
        if (messageType != "LibraryChanged")
            return JellyfinLibraryEventParse::Ignored;

        std::size_t dataPos = 0;
        std::string rawData;
        if (!findKey(message, "Data", dataPos)
            || !rawValue(message, dataPos, rawData))
            return JellyfinLibraryEventParse::Malformed;
        std::string data = rawData;
        if (!rawData.empty() && rawData[0] == '"') {
            std::size_t dataStringPos = 0;
            if (!parseString(rawData, dataStringPos, data))
                return JellyfinLibraryEventParse::Malformed;
        }
        std::string rawDataObject;
        if (data.empty() || data[0] != '{'
            || !rawValue(data, 0, rawDataObject))
            return JellyfinLibraryEventParse::Malformed;
        data = std::move(rawDataObject);

        if (!optionalArray(data, "ItemsAdded", batch.itemsAdded)
            || !optionalArray(data, "ItemsRemoved", batch.itemsRemoved)
            || !optionalArray(data, "ItemsUpdated", batch.itemsUpdated))
            return JellyfinLibraryEventParse::Malformed;
        return JellyfinLibraryEventParse::Parsed;
    }

    static std::string buildSocketUrl(const std::string &baseUrl,
                                      const std::string &accessToken,
                                      const std::string &deviceId)
    {
        std::string url = baseUrl;
        if (url.compare(0, 8, "https://") == 0) url.replace(0, 8, "wss://");
        else if (url.compare(0, 7, "http://") == 0) url.replace(0, 7, "ws://");
        while (!url.empty() && url.back() == '/') url.pop_back();
        return url + "/socket?api_key="
            + jellyfin_library_events_detail::urlEscape(accessToken)
            + "&deviceId="
            + jellyfin_library_events_detail::urlEscape(deviceId);
    }

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
        const std::atomic<bool> &cancelled, unsigned delayMs)
    {
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(delayMs);
        while (!cancelled.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return cancelled.load(std::memory_order_acquire)
            ? JellyfinLibraryEventRun::Cancelled : JellyfinLibraryEventRun::Stopped;
    }

    // Runs on the caller's worker thread. It reconnects through the normal
    // LAN/public route policy and leaves catch-up to LibrarySync consumers.
    JellyfinLibraryEventRun run(
        JellyfinLibraryEventQueue &queue,
        const std::shared_ptr<std::atomic_bool> &cancelled) const
    {
        const std::atomic<bool> neverCancelled{false};
        const auto *stop = cancelled ? cancelled.get() : &neverCancelled;
        Backoff backoff(100, 5000);
        while (!stop->load(std::memory_order_acquire)) {
            std::string error;
            const bool connected = RouteRequest(m_session).run(
                [&](const std::string &base) {
                    return connectAndConsume(base, queue, *stop, error);
                }, error);
            if (stop->load(std::memory_order_acquire))
                return JellyfinLibraryEventRun::Cancelled;
            if (!connected) queue.markCatchUpRequired();
            if (connected) backoff.reset();
            const auto waited = waitForRetry(*stop, backoff.nextDelayMs());
            if (waited == JellyfinLibraryEventRun::Cancelled)
                return waited;
        }
        return JellyfinLibraryEventRun::Cancelled;
    }

    std::future<JellyfinLibraryEventRun> start(
        std::shared_ptr<JellyfinLibraryEventQueue> queue,
        const std::shared_ptr<std::atomic_bool> &cancelled) const
    {
        const Session session = m_session;
        return std::async(std::launch::async,
            [session, queue = std::move(queue), cancelled] {
                JellyfinLibraryEvents events(session);
                return events.run(*queue, cancelled);
            });
    }

private:
    static bool sendAll(CURL *curl, const std::string &payload,
                        const std::atomic<bool> &cancelled)
    {
        std::size_t offset = 0;
        while (offset < payload.size()
               && !cancelled.load(std::memory_order_acquire)) {
            std::size_t sent = 0;
            const CURLcode code = curl_easy_send(curl, payload.data() + offset,
                                                  payload.size() - offset, &sent);
            if (code == CURLE_AGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (code != CURLE_OK || sent == 0) return false;
            offset += sent;
        }
        return offset == payload.size();
    }

    static CURLcode receiveSome(CURL *curl, std::vector<unsigned char> &bytes)
    {
        unsigned char buffer[4096];
        std::size_t received = 0;
        const CURLcode code = curl_easy_recv(curl, buffer, sizeof(buffer), &received);
        if (code == CURLE_OK && received != 0)
            bytes.insert(bytes.end(), buffer, buffer + received);
        return code;
    }

    static std::string hostHeader(const std::string &url)
    {
        const std::size_t scheme = url.find("://");
        const std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
        const std::size_t slash = url.find('/', start);
        const std::size_t query = url.find('?', start);
        const std::size_t end = std::min(slash == std::string::npos ? url.size() : slash,
                                         query == std::string::npos ? url.size() : query);
        return url.substr(start, end - start);
    }

    static std::string requestTarget(const std::string &url)
    {
        const std::size_t scheme = url.find("://");
        const std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
        const std::size_t slash = url.find('/', start);
        return slash == std::string::npos ? "/" : url.substr(slash);
    }

    bool connectAndConsume(const std::string &baseUrl,
                           JellyfinLibraryEventQueue &queue,
                           const std::atomic<bool> &cancelled,
                           std::string &error) const
    {
        CURL *curl = curl_easy_init();
        if (!curl) { error = "Transport: Failed to initialise WebSocket"; return false; }
        const std::string url = buildSocketUrl(baseUrl, m_session.accessToken,
                                               m_session.deviceId);
        std::string connectionUrl = url;
        if (connectionUrl.compare(0, 6, "wss://") == 0)
            connectionUrl.replace(0, 6, "https://");
        else if (connectionUrl.compare(0, 5, "ws://") == 0)
            connectionUrl.replace(0, 5, "http://");
        curl_easy_setopt(curl, CURLOPT_URL, connectionUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const CURLcode connected = curl_easy_perform(curl);
        if (connected != CURLE_OK) {
            error = std::string("Transport: ") + curl_easy_strerror(connected);
            curl_easy_cleanup(curl);
            return false;
        }
        const std::string host = hostHeader(url);
        const std::string request = "GET " + requestTarget(url) +
            " HTTP/1.1\r\nHost: " + host +
            "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
            "bWl5b29maW4ta2V5LTEyMzQ1Ng==\r\n\r\n";
        if (!sendAll(curl, request, cancelled)) {
            error = cancelled.load() ? "Transport: Callback aborted" : "Transport: WebSocket handshake failed";
            curl_easy_cleanup(curl);
            return false;
        }
        std::vector<unsigned char> bytes;
        while (!cancelled.load(std::memory_order_acquire)) {
            const CURLcode received = receiveSome(curl, bytes);
            if (received == CURLE_AGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (received != CURLE_OK) break;
            if (bytes.size() >= 4 && bytes[0] == 'H' && bytes[1] == 'T') {
                const std::string headers(bytes.begin(), bytes.end());
                const std::size_t end = headers.find("\r\n\r\n");
                if (end == std::string::npos) continue;
                if (headers.find(" 101 ") == std::string::npos) {
                    error = "HTTP WebSocket upgrade rejected";
                    curl_easy_cleanup(curl);
                    return false;
                }
                bytes.erase(bytes.begin(), bytes.begin() + end + 4);
            }
            if (!consumeFrames(curl, bytes, queue, cancelled, error)) break;
        }
        curl_easy_cleanup(curl);
        if (cancelled.load(std::memory_order_acquire)) error = "Transport: Callback aborted";
        else if (error.empty()) error = "Transport: WebSocket disconnected";
        return false;
    }

    static bool consumeFrames(CURL *curl, std::vector<unsigned char> &bytes,
                              JellyfinLibraryEventQueue &queue,
                              const std::atomic<bool> &cancelled,
                              std::string &error)
    {
        while (bytes.size() >= 2) {
            const unsigned char first = bytes[0];
            const unsigned char second = bytes[1];
            std::size_t headerSize = 2;
            std::uint64_t payloadSize = second & 0x7f;
            if (payloadSize == 126) {
                if (bytes.size() < 4) return true;
                payloadSize = (static_cast<std::uint64_t>(bytes[2]) << 8) | bytes[3];
                headerSize = 4;
            } else if (payloadSize == 127) {
                if (bytes.size() < 10) return true;
                payloadSize = 0;
                for (unsigned i = 0; i < 8; ++i)
                    payloadSize = (payloadSize << 8) | bytes[2 + i];
                headerSize = 10;
            }
            const bool masked = (second & 0x80) != 0;
            if (masked) headerSize += 4;
            if (payloadSize > jellyfin_library_events_detail::kMaxMessageBytes
                || bytes.size() < headerSize + payloadSize) return true;
            if ((first & 0x0f) == 0x8) return false;
            if ((first & 0x0f) == 0x9) {
                std::string pong("\x8A\x00", 2);
                if (!sendAll(curl, pong, cancelled)) return false;
            } else if ((first & 0x0f) == 0x1) {
                std::string message(bytes.begin() + static_cast<std::ptrdiff_t>(headerSize),
                                    bytes.begin() + static_cast<std::ptrdiff_t>(headerSize + payloadSize));
                JellyfinLibraryChangeBatch batch;
                const auto parsed = parseMessage(message, batch);
                if (parsed == JellyfinLibraryEventParse::Malformed
                    || parsed == JellyfinLibraryEventParse::Oversized) {
                    error = "WebSocket LibraryChanged message malformed";
                    return false;
                }
                if (parsed == JellyfinLibraryEventParse::Parsed) queue.push(batch);
            }
            bytes.erase(bytes.begin(), bytes.begin()
                + static_cast<std::ptrdiff_t>(headerSize + payloadSize));
        }
        return true;
    }

    Session m_session;
};

} // namespace miyoofin

#endif // MIYOOFIN_JELLYFIN_LIBRARY_EVENTS_HPP
