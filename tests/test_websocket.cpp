// WebSocket framing limits and the reconnect behaviour of the library-change socket.
#include "test_support.hpp"

#include "../src/net/JellyfinLibraryEvents.hpp"
#include "../src/net/WebSocketFrames.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace miyoofin;

namespace {

using Bytes = std::vector<unsigned char>;

Bytes frame(unsigned char first, const std::string& payload, bool masked = false)
{
    Bytes b;
    b.push_back(first);
    const std::size_t n = payload.size();
    const unsigned char maskBit = masked ? 0x80 : 0;
    if (n < 126) {
        b.push_back(static_cast<unsigned char>(maskBit | n));
    } else {
        b.push_back(static_cast<unsigned char>(maskBit | 126));
        b.push_back(static_cast<unsigned char>(n >> 8));
        b.push_back(static_cast<unsigned char>(n & 0xff));
    }
    if (masked)
        b.insert(b.end(), {1, 2, 3, 4});
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

void append(Bytes& to, const Bytes& more)
{
    to.insert(to.end(), more.begin(), more.end());
}

void testFramesBytewise()
{
    // A text message delivered one byte at a time is reassembled; nothing is reported early.
    WebSocketFrameReader reader(1024);
    Bytes wire = frame(0x81, "hello");
    Bytes pending;
    int messages = 0;
    for (std::size_t i = 0; i < wire.size(); ++i) {
        pending.push_back(wire[i]);
        const auto r = reader.next(pending);
        if (i + 1 < wire.size()) {
            CHECK(r.status == WebSocketFrameReader::Status::NeedMore);
        } else {
            CHECK(r.status == WebSocketFrameReader::Status::Message && r.payload == "hello");
            ++messages;
        }
    }
    CHECK(messages == 1 && pending.empty());
}

void testFragmentedMessage()
{
    WebSocketFrameReader reader(1024);
    Bytes wire = frame(0x01, "Hel");        // text, not final
    append(wire, frame(0x89, "ping-data")); // a ping may arrive between fragments
    append(wire, frame(0x80, "lo"));        // continuation, final
    auto r = reader.next(wire);
    CHECK(r.status == WebSocketFrameReader::Status::Ping && r.payload == "ping-data");
    r = reader.next(wire);
    CHECK(r.status == WebSocketFrameReader::Status::Message && r.payload == "Hello");
    CHECK(reader.next(wire).status == WebSocketFrameReader::Status::NeedMore);
}

void testOversizedFrameIsRefusedAtTheHeader()
{
    WebSocketFrameReader reader(1024);
    // Only the 10-byte header of a frame declaring ~1 GB has arrived: it must fail now, not wait
    // for (or buffer) the body.
    Bytes huge = {0x81, 127, 0, 0, 0, 0, 0x40, 0, 0, 0};
    auto r = reader.next(huge);
    CHECK(r.status == WebSocketFrameReader::Status::Error);
    CHECK(r.error.find("size limit") != std::string::npos);

    // A message that stays under the limit per frame but exceeds it in total is stopped too.
    WebSocketFrameReader fragmented(100);
    Bytes wire = frame(0x01, std::string(60, 'a'));
    CHECK(fragmented.next(wire).status == WebSocketFrameReader::Status::NeedMore);
    wire = frame(0x00, std::string(60, 'b'));
    CHECK(fragmented.next(wire).status == WebSocketFrameReader::Status::Error);
}

void testProtocolViolations()
{
    WebSocketFrameReader reader(1024);
    Bytes masked = frame(0x81, "x", true);
    CHECK(reader.next(masked).status ==
          WebSocketFrameReader::Status::Error); // server frames are unmasked
    WebSocketFrameReader r2(1024);
    Bytes reserved = frame(0xC1, "x");
    CHECK(r2.next(reserved).status == WebSocketFrameReader::Status::Error);
    WebSocketFrameReader r3(1024);
    Bytes orphan = frame(0x80, "x"); // continuation with nothing to continue
    CHECK(r3.next(orphan).status == WebSocketFrameReader::Status::Error);
    WebSocketFrameReader r4(1024);
    Bytes bigPing = frame(0x89, std::string(200, 'p'));
    CHECK(r4.next(bigPing).status == WebSocketFrameReader::Status::Error); // control frames <= 125
    WebSocketFrameReader r5(1024);
    Bytes closing = frame(0x88, "");
    CHECK(r5.next(closing).status == WebSocketFrameReader::Status::Close);
}

void testPongIsMaskedAndEchoesThePing()
{
    const std::string pong = buildPong("abc");
    CHECK(pong.size() == 2 + 4 + 3);
    CHECK(static_cast<unsigned char>(pong[0]) == 0x8A);       // FIN + pong
    CHECK((static_cast<unsigned char>(pong[1]) & 0x80) != 0); // masked, as clients must
    CHECK((static_cast<unsigned char>(pong[1]) & 0x7f) == 3); // the ping's length
    std::string unmasked;
    for (int i = 0; i < 3; ++i)
        unmasked.push_back(static_cast<char>(static_cast<unsigned char>(pong[6 + i]) ^
                                             static_cast<unsigned char>(pong[2 + (i % 4)])));
    CHECK(unmasked == "abc");
}

// ---- the socket itself, against a loopback server that misbehaves ----

class MisbehavingServer
{
  public:
    enum class Mode
    {
        CloseAfterUpgrade, // 101, then TCP close without a WebSocket close frame
        OversizedFrame     // 101, then a header declaring a huge frame, then silence
    };
    explicit MisbehavingServer(Mode mode) : m_mode(mode)
    {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(m_fd, 8);
        socklen_t len = sizeof(addr);
        ::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_thread = std::thread([this] { run(); });
    }
    ~MisbehavingServer()
    {
        m_stop = true;
        ::shutdown(m_fd, SHUT_RDWR);
        ::close(m_fd);
        m_thread.join();
    }
    std::string base() const
    {
        return "http://127.0.0.1:" + std::to_string(m_port);
    }
    int connections() const
    {
        return m_connections.load();
    }

  private:
    void run()
    {
        while (!m_stop) {
            const int client = ::accept(m_fd, nullptr, nullptr);
            if (client < 0)
                return;
            ++m_connections;
            std::string request;
            char buf[2048];
            while (request.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
                if (n <= 0)
                    break;
                request.append(buf, static_cast<std::size_t>(n));
            }
            const char upgrade[] = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                   "Connection: Upgrade\r\n\r\n";
            ::send(client, upgrade, sizeof(upgrade) - 1, MSG_NOSIGNAL);
            if (m_mode == Mode::OversizedFrame) {
                const unsigned char huge[] = {0x81, 127, 0, 0, 0, 0, 0x40, 0, 0, 0};
                ::send(client, huge, sizeof(huge), MSG_NOSIGNAL);
                // Keep the connection open until the client hangs up (at most 3 s). A client that
                // waits for the 1 GB body never does, and never makes a second connection.
                timeval timeout{3, 0};
                ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                char sink[4096];
                while (::recv(client, sink, sizeof(sink), 0) > 0) {
                }
            }
            ::close(client);
        }
    }
    Mode m_mode;
    int m_fd = -1;
    unsigned short m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_connections{0};
    std::thread m_thread;
};

// Runs the real event loop against the server and reports how many connections it made in `ms`.
int connectionsWithin(MisbehavingServer& server, int ms)
{
    Session session;
    session.serverUrl = server.base();
    session.accessToken = "t";
    session.userId = "u";
    session.deviceId = "d";
    auto queue = std::make_shared<JellyfinLibraryEventQueue>();
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    JellyfinLibraryEvents events(session);
    std::thread worker([&] { events.run(*queue, cancelled); });
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    cancelled->store(true);
    worker.join();
    return server.connections();
}

void testAbruptDisconnectReconnects()
{
    // The old reader treated "0 bytes read" as success and spun on the dead socket forever, so a
    // server that dropped the TCP connection never saw a second connection.
    MisbehavingServer server(MisbehavingServer::Mode::CloseAfterUpgrade);
    CHECK(connectionsWithin(server, 2500) >= 2);
}

void testOversizedFrameReconnects()
{
    // The old reader returned "need more" for a frame declaring 1 GB and buffered whatever came.
    MisbehavingServer server(MisbehavingServer::Mode::OversizedFrame);
    CHECK(connectionsWithin(server, 2500) >= 2);
}

} // namespace

int main()
{
    testFramesBytewise();
    testFragmentedMessage();
    testOversizedFrameIsRefusedAtTheHeader();
    testProtocolViolations();
    testPongIsMaskedAndEchoesThePing();
    testAbruptDisconnectReconnects();
    testOversizedFrameReconnects();
    return miyoofin_test::finish("websocket");
}
