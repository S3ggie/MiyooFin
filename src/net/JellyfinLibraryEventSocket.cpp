#include "JellyfinLibraryEvents.hpp"
#include "JellyfinLibraryEventParse.hpp"
#include "RouteRequest.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <curl/curl.h>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {

std::string JellyfinLibraryEvents::buildSocketUrl(const std::string& baseUrl,
                                                  const std::string& accessToken,
                                                  const std::string& deviceId)
{
    std::string url = baseUrl;
    if (url.compare(0, 8, "https://") == 0)
        url.replace(0, 8, "wss://");
    else if (url.compare(0, 7, "http://") == 0)
        url.replace(0, 7, "ws://");
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url + "/socket?api_key=" + jellyfin_library_events_detail::urlEscape(accessToken) +
           "&deviceId=" + jellyfin_library_events_detail::urlEscape(deviceId);
}

JellyfinLibraryEventRun JellyfinLibraryEvents::waitForRetry(const std::atomic<bool>& cancelled,
                                                            unsigned delayMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    while (!cancelled.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return cancelled.load(std::memory_order_acquire) ? JellyfinLibraryEventRun::Cancelled
                                                     : JellyfinLibraryEventRun::Stopped;
}

bool JellyfinLibraryEvents::sendAll(void* curl, const std::string& payload,
                                    const std::atomic<bool>& cancelled)
{
    CURL* c = static_cast<CURL*>(curl);
    std::size_t offset = 0;
    while (offset < payload.size() && !cancelled.load(std::memory_order_acquire)) {
        std::size_t sent = 0;
        const CURLcode code =
            curl_easy_send(c, payload.data() + offset, payload.size() - offset, &sent);
        if (code == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (code != CURLE_OK || sent == 0)
            return false;
        offset += sent;
    }
    return offset == payload.size();
}

int JellyfinLibraryEvents::receiveSome(void* curl, std::vector<unsigned char>& bytes)
{
    CURL* c = static_cast<CURL*>(curl);
    unsigned char buffer[4096];
    std::size_t received = 0;
    const CURLcode code = curl_easy_recv(c, buffer, sizeof(buffer), &received);
    if (code == CURLE_OK && received != 0)
        bytes.insert(bytes.end(), buffer, buffer + received);
    return static_cast<int>(code);
}

std::string JellyfinLibraryEvents::hostHeader(const std::string& url)
{
    const std::size_t scheme = url.find("://");
    const std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const std::size_t slash = url.find('/', start);
    const std::size_t query = url.find('?', start);
    const std::size_t end = std::min(slash == std::string::npos ? url.size() : slash,
                                     query == std::string::npos ? url.size() : query);
    return url.substr(start, end - start);
}

std::string JellyfinLibraryEvents::requestTarget(const std::string& url)
{
    const std::size_t scheme = url.find("://");
    const std::size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    const std::size_t slash = url.find('/', start);
    return slash == std::string::npos ? "/" : url.substr(slash);
}

bool JellyfinLibraryEvents::connectAndConsume(const std::string& baseUrl,
                                              JellyfinLibraryEventQueue& queue,
                                              const std::atomic<bool>& cancelled,
                                              std::string& error) const
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Transport: Failed to initialise WebSocket";
        return false;
    }
    const std::string url = buildSocketUrl(baseUrl, m_session.accessToken, m_session.deviceId);
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
    const std::string request = "GET " + requestTarget(url) + " HTTP/1.1\r\nHost: " + host +
                                "\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
                                "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
                                "bWl5b29maW4ta2V5LTEyMzQ1Ng==\r\n\r\n";
    if (!sendAll(curl, request, cancelled)) {
        error = cancelled.load() ? "Transport: Callback aborted"
                                 : "Transport: WebSocket handshake failed";
        curl_easy_cleanup(curl);
        return false;
    }
    std::vector<unsigned char> bytes;
    while (!cancelled.load(std::memory_order_acquire)) {
        const CURLcode received = static_cast<CURLcode>(receiveSome(curl, bytes));
        if (received == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (received != CURLE_OK)
            break;
        if (bytes.size() >= 4 && bytes[0] == 'H' && bytes[1] == 'T') {
            const std::string headers(bytes.begin(), bytes.end());
            const std::size_t end = headers.find("\r\n\r\n");
            if (end == std::string::npos)
                continue;
            if (headers.find(" 101 ") == std::string::npos) {
                error = "HTTP WebSocket upgrade rejected";
                curl_easy_cleanup(curl);
                return false;
            }
            bytes.erase(bytes.begin(), bytes.begin() + end + 4);
        }
        if (!consumeFrames(curl, bytes, queue, cancelled, error))
            break;
    }
    curl_easy_cleanup(curl);
    if (cancelled.load(std::memory_order_acquire))
        error = "Transport: Callback aborted";
    else if (error.empty())
        error = "Transport: WebSocket disconnected";
    return false;
}

bool JellyfinLibraryEvents::consumeFrames(void* curl, std::vector<unsigned char>& bytes,
                                          JellyfinLibraryEventQueue& queue,
                                          const std::atomic<bool>& cancelled, std::string& error)
{
    CURL* c = static_cast<CURL*>(curl);
    while (bytes.size() >= 2) {
        const unsigned char first = bytes[0];
        const unsigned char second = bytes[1];
        std::size_t headerSize = 2;
        std::uint64_t payloadSize = second & 0x7f;
        if (payloadSize == 126) {
            if (bytes.size() < 4)
                return true;
            payloadSize = (static_cast<std::uint64_t>(bytes[2]) << 8) | bytes[3];
            headerSize = 4;
        } else if (payloadSize == 127) {
            if (bytes.size() < 10)
                return true;
            payloadSize = 0;
            for (unsigned i = 0; i < 8; ++i)
                payloadSize = (payloadSize << 8) | bytes[2 + i];
            headerSize = 10;
        }
        const bool masked = (second & 0x80) != 0;
        if (masked)
            headerSize += 4;
        if (payloadSize > jellyfin_library_events_detail::kMaxMessageBytes ||
            bytes.size() < headerSize + payloadSize)
            return true;
        if ((first & 0x0f) == 0x8)
            return false;
        if ((first & 0x0f) == 0x9) {
            std::string pong("\x8A\x00", 2);
            if (!sendAll(c, pong, cancelled))
                return false;
        } else if ((first & 0x0f) == 0x1) {
            std::string message(bytes.begin() + static_cast<std::ptrdiff_t>(headerSize),
                                bytes.begin() +
                                    static_cast<std::ptrdiff_t>(headerSize + payloadSize));
            JellyfinLibraryChangeBatch batch;
            const auto parsed = parseMessage(message, batch);
            if (parsed == JellyfinLibraryEventParse::Malformed ||
                parsed == JellyfinLibraryEventParse::Oversized) {
                error = "WebSocket LibraryChanged message malformed";
                return false;
            }
            if (parsed == JellyfinLibraryEventParse::Parsed)
                queue.push(batch);
        }
        bytes.erase(bytes.begin(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(headerSize + payloadSize));
    }
    return true;
}

JellyfinLibraryEventRun
JellyfinLibraryEvents::run(JellyfinLibraryEventQueue& queue,
                           const std::shared_ptr<std::atomic_bool>& cancelled) const
{
    const std::atomic<bool> neverCancelled{false};
    const auto* stop = cancelled ? cancelled.get() : &neverCancelled;
    Backoff backoff(100, 5000);
    while (!stop->load(std::memory_order_acquire)) {
        std::string error;
        const bool connected = RouteRequest(m_session).run(
            [&](const std::string& base) { return connectAndConsume(base, queue, *stop, error); },
            error);
        if (stop->load(std::memory_order_acquire))
            return JellyfinLibraryEventRun::Cancelled;
        if (!connected)
            queue.markCatchUpRequired();
        if (connected)
            backoff.reset();
        const auto waited = waitForRetry(*stop, backoff.nextDelayMs());
        if (waited == JellyfinLibraryEventRun::Cancelled)
            return waited;
    }
    return JellyfinLibraryEventRun::Cancelled;
}

std::future<JellyfinLibraryEventRun>
JellyfinLibraryEvents::start(std::shared_ptr<JellyfinLibraryEventQueue> queue,
                             const std::shared_ptr<std::atomic_bool>& cancelled) const
{
    const Session session = m_session;
    return std::async(std::launch::async, [session, queue = std::move(queue), cancelled] {
        JellyfinLibraryEvents events(session);
        return events.run(*queue, cancelled);
    });
}

} // namespace miyoofin
